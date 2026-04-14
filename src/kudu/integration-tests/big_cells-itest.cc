// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kudu/client/client.h"
#include "kudu/client/scan_batch.h"
#include "kudu/client/schema.h"
#include "kudu/client/shared_ptr.h" // IWYU pragma: keep
#include "kudu/client/write_op.h"
#include "kudu/common/partial_row.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/integration-tests/external_mini_cluster-itest-base.h"
#include "kudu/util/random.h"
#include "kudu/util/random_util.h"
#include "kudu/util/slice.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using std::string;
using std::unique_ptr;
using std::vector;

namespace kudu {
namespace client {

using sp::shared_ptr;

class BigCellsItest : public ExternalMiniClusterITestBase,
                      public ::testing::WithParamInterface<size_t> {
};

INSTANTIATE_TEST_SUITE_P(CellSizes, BigCellsItest,
    ::testing::Values(
        size_t{1} * 1024 * 1024,
        size_t{10} * 1024 * 1024,
        size_t{100} * 1024 * 1024));

// Writes max-sized cells (random, high-entropy) to a nullable STRING column
// and verifies round-trip reads using a different session than the writer.
TEST_P(BigCellsItest, TestRoundTripMaxSizedStringCells) {
  constexpr int kNumServers = 3;
  constexpr int kNumTablets = 3;
  constexpr int kNumRows = 5;
  const size_t kMaxCellBytes = GetParam();
  const char* const kTableName = "big_cells_table";

  vector<string> ts_flags;
  ts_flags.emplace_back(strings::Substitute("--max_cell_size_bytes=$0", kMaxCellBytes));
  NO_FATALS(StartCluster(std::move(ts_flags), {}, kNumServers));

  KuduSchemaBuilder builder;
  builder.AddColumn("k_int")->Type(KuduColumnSchema::INT32)->NotNull();
  builder.AddColumn("k_str")->Type(KuduColumnSchema::STRING)->NotNull();
  builder.AddColumn("payload")->Type(KuduColumnSchema::STRING)->Nullable();
  builder.SetPrimaryKey({ "k_int", "k_str" });
  KuduSchema schema;
  ASSERT_OK(builder.Build(&schema));

  unique_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(kTableName)
      .schema(&schema)
      .num_replicas(kNumServers)
      .add_hash_partitions({ "k_int", "k_str" }, kNumTablets)
      .Create());
  shared_ptr<KuduTable> table;
  ASSERT_OK(client_->OpenTable(kTableName, &table));

  Random rng(SeedRandom());
  std::map<std::pair<int32_t, string>, string> expected;
  for (int i = 0; i < kNumRows; ++i) {
    const int32_t k_int = i;
    const string k_str = strings::Substitute("k$0", i);
    expected[std::make_pair(k_int, k_str)] = RandomString(kMaxCellBytes, &rng);
  }

  shared_ptr<KuduSession> write_session = client_->NewSession();
  ASSERT_OK(write_session->SetMutationBufferSpace(kMaxCellBytes + 2 * 1024 * 1024));
  ASSERT_OK(write_session->SetFlushMode(KuduSession::AUTO_FLUSH_SYNC));
  for (const auto& entry : expected) {
    const int32_t k_int = entry.first.first;
    const string& k_str = entry.first.second;
    const string& payload = entry.second;
    KuduInsert* insert = table->NewInsert();
    KuduPartialRow* write = insert->mutable_row();
    ASSERT_OK(write->SetInt32("k_int", k_int));
    ASSERT_OK(write->SetString("k_str", k_str));
    ASSERT_OK(write->SetString("payload", payload));
    ASSERT_OK(write_session->Apply(insert));
  }
  ASSERT_OK(write_session->Flush());

  shared_ptr<KuduSession> read_session = client_->NewSession();
  ASSERT_NE(write_session.get(), read_session.get());

  KuduScanner scanner(table.get());
  ASSERT_OK(scanner.SetFaultTolerant());
  ASSERT_OK(scanner.Open());
  int rows_seen = 0;
  while (scanner.HasMoreRows()) {
    KuduScanBatch batch;
    ASSERT_OK(scanner.NextBatch(&batch));
    for (const KuduScanBatch::RowPtr& row : batch) {
      int32_t k_int;
      ASSERT_OK(row.GetInt32("k_int", &k_int));
      Slice k_str_slice;
      ASSERT_OK(row.GetString("k_str", &k_str_slice));
      const string k_str = k_str_slice.ToString();
      ASSERT_FALSE(row.IsNull("payload"));
      Slice payload_slice;
      ASSERT_OK(row.GetString("payload", &payload_slice));
      auto it = expected.find(std::make_pair(k_int, k_str));
      ASSERT_NE(it, expected.end())
          << "unexpected row k_int=" << k_int << " k_str=" << k_str;
      ASSERT_EQ(it->second, payload_slice.ToString());
      ++rows_seen;
    }
  }
  ASSERT_EQ(kNumRows, rows_seen);
}

} // namespace client
} // namespace kudu

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
//
// Test: start a single in-process MiniTabletServer with a 1 GiB hard memory
// limit and a 1 MiB maximum cell size, write rows whose STRING columns hold
// large values, then exercise several read paths (parallel ranged reads, diff
// scans, snapshot reads, and full reads) while tracking peak process memory.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "kudu/clock/clock.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/partial_row.h"
#include "kudu/common/row_operations.h"
#include "kudu/common/schema.h"
#include "kudu/common/timestamp.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/common/wire_protocol.pb.h"
#include "kudu/consensus/consensus.proxy.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/rpc/rpc_header.pb.h"
#include "kudu/server/server_base.proxy.h"
#include "kudu/tablet/mvcc.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_metadata.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tserver/mini_tablet_server.h"
#include "kudu/tserver/tablet_copy.proxy.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/tserver/tablet_server_test_util.h"
#include "kudu/tserver/ts_tablet_manager.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/tserver/tserver_admin.proxy.h"
#include "kudu/tserver/tserver_service.proxy.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/process_memory.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

DECLARE_bool(enable_maintenance_manager);
DECLARE_bool(enable_rowset_compaction);
DECLARE_int64(memory_limit_hard_bytes);
DECLARE_int32(max_cell_size_bytes);
DECLARE_int32(scanner_max_batch_size_bytes);

using std::atomic;
using std::string;
using std::thread;
using std::unique_ptr;
using std::vector;

namespace kudu {
namespace tserver {

namespace {

using rpc::RpcController;

constexpr const char* const kTableId = "MemLimitReadTestTable";
constexpr const char* const kTabletId = "00000000000000000000000000000001";

constexpr int64_t kMemLimitBytes = 1LL << 30;          // 1 GiB
constexpr int64_t kTargetWriteBatchBytes = 4LL << 20;  // 4 MiB

struct ReadTestParam {
  // Candidate STRING cell lengths (bytes).  One length is drawn uniformly from
  // this list per row and reused for every STRING cell in that row.
  vector<int> cell_sizes;

  // Number of non-PK STRING columns in the schema.
  int string_column_count;

  // Human-readable suffix used to name the parameterized test instance.
  string test_case_name;

  // If true, the table is marked 'huge' at the table level (via the
  // kudu.table.huge extra config) and the individual STRING columns are NOT
  // marked 'huge'. If false, each STRING column is marked 'huge' individually.
  bool table_level_huge;
};

// Schema:
//   pk1       INT32  (PK col 1) - monotonically increasing
//   val_int_a INT32  - random
//   str_0..str_{N-1} STRING - random values sized per-row from cell_sizes
Schema MakeTestSchema(int string_column_count, bool mark_columns_huge) {
  vector<ColumnSchema> cols = {
      ColumnSchema("pk1",       INT32),
      ColumnSchema("val_int_a", INT32, ColumnSchema::NULLABLE),
  };
  for (int i = 0; i < string_column_count; i++) {
    cols.emplace_back(ColumnSchemaBuilder()
                          .name(strings::Substitute("str_$0", i))
                          .type(STRING)
                          .nullable(true)
                          .huge(mark_columns_huge));
  }
  return Schema(cols, 1);
}

// Return a random string of exactly 'len' bytes, using byte values in [1, 255]
// (0 is avoided so the payload is non-empty in the usual C-string sense).
string RandomString(int len) {
  string s;
  s.reserve(len);
  for (int i = 0; i < len; i++) {
    s.push_back(rand() % 255 + 1);
  }
  return s;
}

// Samples the process's peak memory consumption in the background from
// construction until Stop() is called.  Runs a single sampler thread that polls
// process_memory::CurrentConsumption() every 100 ms.
class PeakMemorySampler {
 public:
  PeakMemorySampler() {
    thread_ = thread([this]() {
      while (!done_.load(std::memory_order_acquire)) {
        peak_ = std::max(peak_, process_memory::CurrentConsumption());
        SleepFor(MonoDelta::FromMilliseconds(100));
      }
      // Take one final sample so a spike right before Stop() is not missed.
      peak_ = std::max(peak_, process_memory::CurrentConsumption());
    });
  }

  // Stops sampling, joins the sampler thread, and returns the peak observed.
  int64_t Stop() {
    done_ = true;
    thread_.join();
    return peak_;
  }

 private:
  int64_t peak_ = 0;
  atomic<bool> done_ = false;
  thread thread_;
};

// Open a scanner described by 'new_scan', drain all batches, count rows read,
// and fail the test if any RPC errors occur.
// 'projection' must match the columns listed in new_scan.projected_columns().
void OpenAndDrainScan(TabletServerServiceProxy* proxy,
                      const NewScanRequestPB& new_scan,
                      const Schema& projection,
                      int64_t* rows_read) {
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(60));

  ScanRequestPB req;
  ScanResponsePB resp;
  *req.mutable_new_scan_request() = new_scan;
  req.set_call_seq_id(0);
  req.set_batch_size_bytes(0);  // open without data

  Status s = proxy->Scan(req, &resp, &rpc);
  if (!s.ok()) {
    ADD_FAILURE() << "Scan open RPC failed: " << s.ToString();
    return;
  }
  if (resp.has_error()) {
    ADD_FAILURE() << "Scan open error: " << resp.error().ShortDebugString();
    return;
  }

  req.clear_new_scan_request();
  req.set_scanner_id(resp.scanner_id());
  int call_seq = 1;

  while (resp.has_more_results()) {
    rpc.Reset();
    rpc.set_timeout(MonoDelta::FromSeconds(60));
    req.set_call_seq_id(call_seq++);
    req.set_batch_size_bytes(1024 * 1024);  // 1 MiB batches

    s = proxy->Scan(req, &resp, &rpc);
    if (!s.ok()) {
      ADD_FAILURE() << "Scan continue RPC failed: " << s.ToString();
      return;
    }
    if (resp.has_error()) {
      ADD_FAILURE() << "Scan continue error: " << resp.error().ShortDebugString();
      return;
    }

    if (resp.data().num_rows() > 0) {
      RowwiseRowBlockPB* rrpb = resp.mutable_data();
      Slice direct, indirect;
      CHECK_OK(rpc.GetInboundSidecar(rrpb->rows_sidecar(), &direct));
      if (rrpb->has_indirect_data_sidecar()) {
        CHECK_OK(rpc.GetInboundSidecar(rrpb->indirect_data_sidecar(), &indirect));
      }
      vector<const uint8_t*> rows;
      CHECK_OK(ExtractRowsFromRowBlockPB(projection, *rrpb, indirect, &direct, &rows));
      *rows_read += static_cast<int64_t>(rows.size());
    }
  }
}

// Build a READ_LATEST scan request over all columns of 'schema' (full table).
NewScanRequestPB MakeLatestScanRequest(const string& tablet_id,
                                       const Schema& schema) {
  NewScanRequestPB new_scan;
  new_scan.set_tablet_id(tablet_id);
  CHECK_OK(SchemaToColumnPBs(schema, new_scan.mutable_projected_columns()));
  new_scan.set_read_mode(READ_LATEST);
  return new_scan;
}

// Build an ORDERED (primary-key-sorted) scan request over all columns of
// 'schema' at 'snap_timestamp'.  ORDERED scans are only valid as snapshot
// reads and are served by the MergeIterator, whose per-sub-iterator internal
// buffer is what we want to exercise here.
NewScanRequestPB MakeOrderedSnapshotScanRequest(const string& tablet_id,
                                                const Schema& schema,
                                                uint64_t snap_timestamp) {
  NewScanRequestPB new_scan;
  new_scan.set_tablet_id(tablet_id);
  CHECK_OK(SchemaToColumnPBs(schema, new_scan.mutable_projected_columns()));
  new_scan.set_read_mode(READ_AT_SNAPSHOT);
  new_scan.set_order_mode(ORDERED);
  new_scan.set_snap_timestamp(snap_timestamp);
  return new_scan;
}

// Build a READ_LATEST scan request restricted to pk1 in
// [pk_lower, pk_upper_exclusive) via a range column predicate.
NewScanRequestPB MakeRangedLatestScanRequest(const string& tablet_id,
                                             const Schema& schema,
                                             int32_t pk_lower,
                                             int32_t pk_upper_exclusive) {
  NewScanRequestPB new_scan;
  new_scan.set_tablet_id(tablet_id);
  CHECK_OK(SchemaToColumnPBs(schema, new_scan.mutable_projected_columns()));
  new_scan.set_read_mode(READ_LATEST);

  ColumnPredicatePB* pred = new_scan.add_column_predicates();
  pred->set_column("pk1");
  ColumnPredicatePB::Range* range = pred->mutable_range();
  range->mutable_lower()->append(
      reinterpret_cast<char*>(&pk_lower), sizeof(pk_lower));
  range->mutable_upper()->append(
      reinterpret_cast<char*>(&pk_upper_exclusive), sizeof(pk_upper_exclusive));
  return new_scan;
}

// Build a diff scan (READ_AT_SNAPSHOT with a start timestamp) covering the
// range (t0, t1].  'projection' must include an IS_DELETED virtual column.
NewScanRequestPB MakeDiffScanRequest(const string& tablet_id,
                                     const Schema& projection,
                                     uint64_t t0,
                                     uint64_t t1) {
  NewScanRequestPB new_scan;
  new_scan.set_tablet_id(tablet_id);
  CHECK_OK(SchemaToColumnPBs(projection, new_scan.mutable_projected_columns()));
  new_scan.set_read_mode(READ_AT_SNAPSHOT);
  new_scan.set_order_mode(ORDERED);
  new_scan.set_snap_start_timestamp(t0);
  new_scan.set_snap_timestamp(t1);
  return new_scan;
}

NewScanRequestPB MakeSnapshotScanRequest(const string& tablet_id,
                                         const Schema& schema,
                                         uint64_t snap_timestamp) {
  NewScanRequestPB new_scan;
  new_scan.set_tablet_id(tablet_id);
  CHECK_OK(SchemaToColumnPBs(schema, new_scan.mutable_projected_columns()));
  new_scan.set_read_mode(READ_AT_SNAPSHOT);
  new_scan.set_snap_timestamp(snap_timestamp);
  return new_scan;
}

Schema MakeDiffScanProjection(const Schema& schema) {
  SchemaBuilder builder(schema);
  static const bool kIsDeletedDefault = false;
  CHECK_OK(builder.AddColumn(
      ColumnSchemaBuilder().name("is_deleted").type(IS_DELETED)
                           .read_default(&kIsDeletedDefault)));
  return builder.BuildWithoutIds();
}

void AssertPeakWithinBudget(int64_t peak, const char* phase) {
  const int64_t overshoot_bytes = peak - kMemLimitBytes;
  const double peak_gib = static_cast<double>(peak) / (1 << 30);
  // Clamp negative overshoot (peak under the limit) to 0%.
  const int overshoot_pct =
      static_cast<int>(std::max(0.0, 100.0 * overshoot_bytes / kMemLimitBytes));

  LOG(INFO) << "phase: " << phase
            << "\n    Peak:   " << StringPrintf("%.2f", peak_gib) << " GiB"
            << "\n    Overshoot: " << overshoot_pct << "%";

  const int64_t max_allowed_overshoot =
      static_cast<int64_t>(kMemLimitBytes * 0.50);
  ASSERT_LE(overshoot_bytes, max_allowed_overshoot)
      << "[" << phase << "] peak " << peak << " bytes overshot the hard limit by "
      << overshoot_bytes << " bytes (max allowed " << max_allowed_overshoot
      << " bytes / 50% of the hard limit).";
}

}  // anonymous namespace

// Test fixture: owns a single MiniTabletServer with the 1 GiB hard memory
// limit, a 1 MiB max cell size, and the maintenance manager enabled (so the
// server can flush the MemRowSet to relieve memory pressure).
class MemoryLimitOvershootReadTest
    : public KuduTest,
      public ::testing::WithParamInterface<ReadTestParam> {
 public:
  void SetUp() override {
    KuduTest::SetUp();

    const ReadTestParam& p = GetParam();
    const auto max_cell_size =
        *std::max_element(p.cell_sizes.begin(), p.cell_sizes.end());
    const int64_t max_row_bytes =
        static_cast<int64_t>(p.string_column_count) * max_cell_size;
    max_rows_per_write_batch_ = static_cast<int>(std::max<int64_t>(
        1, kTargetWriteBatchBytes / std::max<int64_t>(1, max_row_bytes)));

    FLAGS_memory_limit_hard_bytes    = kMemLimitBytes;
    FLAGS_max_cell_size_bytes        = max_cell_size;
    // Re-enable the maintenance manager (disabled by default in TS unit tests)
    // so that the server can flush under memory pressure.
    FLAGS_enable_maintenance_manager = true;

    // When exercising the table-level 'huge' flag, leave the individual columns
    // unmarked so the reduced batching is driven purely by the table property.
    schema_ = MakeTestSchema(p.string_column_count,
                             /*mark_columns_huge=*/!p.table_level_huge);

    mini_server_.reset(new MiniTabletServer(
        GetTestPath("MemLimitOvershootReadTest-fsroot"),
        HostPort("127.0.0.1", 0), /*num_data_dirs=*/1));
    // Use an unreachable master address so heartbeats never succeed, keeping
    // the tserver running but standalone.
    mini_server_->options()->master_addresses.clear();
    mini_server_->options()->master_addresses.emplace_back("255.255.255.255", 1);

    ASSERT_OK(mini_server_->Start());
    ASSERT_OK(mini_server_->AddTestTablet(kTableId, kTabletId, schema_));
    ASSERT_OK(WaitForTabletRunning());

    // Mark the whole table 'huge' via the tablet metadata extra config so the
    // read/write paths use the reduced batch/buffer sizes for every column.
    if (p.table_level_huge) {
      scoped_refptr<tablet::TabletReplica> replica;
      ASSERT_OK(mini_server_->server()->tablet_manager()->GetTabletReplica(
          kTabletId, &replica));
      TableExtraConfigPB extra_config;
      extra_config.set_huge(true);
      replica->tablet_metadata()->SetExtraConfig(std::move(extra_config));
    }

    rpc::MessengerBuilder bld("MemLimitReadTestClient");
    ASSERT_OK(bld.Build(&messenger_));
    CreateTsClientProxies(mini_server_->bound_rpc_addr(), messenger_,
                          &tablet_copy_proxy_, &proxy_, &admin_proxy_,
                          &consensus_proxy_, &generic_proxy_);
  }

  void TearDown() override {
    mini_server_->Shutdown();
    KuduTest::TearDown();
  }

 protected:
  // Replicates TabletServerTestBase::WaitForTabletRunning() without relying
  // on that class's fixed schema.
  Status WaitForTabletRunning() {
    auto* tablet_manager = mini_server_->server()->tablet_manager();
    const MonoDelta kTimeout = MonoDelta::FromSeconds(30);

    scoped_refptr<tablet::TabletReplica> replica;
    RETURN_NOT_OK(tablet_manager->GetTabletReplica(kTabletId, &replica));
    RETURN_NOT_OK(replica->WaitUntilConsensusRunning(kTimeout));
    RETURN_NOT_OK(replica->consensus()->WaitUntilLeader(kTimeout));

    // Wait for MVCC safe time to be initialized (KUDU-2463).
    const MonoTime deadline = MonoTime::Now() + kTimeout;
    while (!replica->tablet()->mvcc_manager()->CheckIsCleanTimeInitialized().ok()) {
      if (MonoTime::Now() >= deadline) {
        return Status::TimedOut(
            "MVCC clean time did not initialize within timeout");
      }
      SleepFor(MonoDelta::FromMilliseconds(10));
    }

    return tablet_manager->WaitForNoTransitionsForTests(kTimeout);
  }

  // Flushes the tablet's MemRowSet to a new DiskRowSet.  Used to materialize
  // overlapping on-disk rowsets so an ORDERED scan must merge across them.
  void FlushTablet() {
    scoped_refptr<tablet::TabletReplica> replica;
    ASSERT_OK(mini_server_->server()->tablet_manager()->GetTabletReplica(
        kTabletId, &replica));
    ASSERT_OK(replica->tablet()->Flush());
  }

  // Returns the current number of rowsets in the tablet.
  size_t NumRowSets() {
    scoped_refptr<tablet::TabletReplica> replica;
    CHECK_OK(mini_server_->server()->tablet_manager()->GetTabletReplica(
        kTabletId, &replica));
    return replica->tablet()->num_rowsets();
  }

  // Upsert rows with PKs in [pk_start, pk_end) in a single RPC batch.
  // Each row picks a single STRING length uniformly from the configured
  // 'cell_sizes' (once per row, reused for every STRING cell) and fills the
  // int columns with random values.  Returns the server-assigned write
  // timestamp.
  uint64_t UpsertBigRowSubBatch(int pk_start, int pk_end) {
    const vector<int>& cell_sizes = GetParam().cell_sizes;

    WriteRequestPB req;
    req.set_tablet_id(kTabletId);
    CHECK_OK(SchemaToPB(schema_, req.mutable_schema()));

    RowOperationsPBEncoder encoder(req.mutable_row_operations());
    for (int pk = pk_start; pk < pk_end; pk++) {
      KuduPartialRow row(&schema_);
      CHECK_OK(row.SetInt32("pk1", pk));
      CHECK_OK(row.SetInt32("val_int_a", rand()));
      const int str_len = cell_sizes[rand() % cell_sizes.size()];
      for (int c = 0; c < static_cast<int>(schema_.num_columns()); c++) {
        const ColumnSchema& col = schema_.column(c);
        if (col.type_info()->type() == STRING) {
          CHECK_OK(row.SetStringCopy(c, RandomString(str_len)));
        }
      }
      encoder.Add(RowOperationsPB::UPSERT, row);
    }

    return WriteWithRetries(req);
  }

  // Sends 'req' via the Write RPC, retrying with backoff while the server
  // reports ERROR_SERVER_TOO_BUSY (due to memory pressure).  Returns the
  // server-assigned write timestamp.
  uint64_t WriteWithRetries(const WriteRequestPB& req) {
    int attempts = 0;
    while (true) {
      WriteResponsePB resp;
      RpcController rpc;
      rpc.set_timeout(MonoDelta::FromSeconds(30));

      const Status s = proxy_->Write(req, &resp, &rpc);

      if (s.IsRemoteError()) {
        const auto* err = rpc.error_response();
        if (err && err->has_code() &&
            err->code() == rpc::ErrorStatusPB::ERROR_SERVER_TOO_BUSY) {
          const int sleep_ms = attempts + (rand() % 5);
          SleepFor(MonoDelta::FromMilliseconds(sleep_ms));
          attempts++;
          continue;
        }
      }

      CHECK_OK(s);
      CHECK(!resp.has_error()) << resp.error().ShortDebugString();
      return resp.timestamp();
    }
  }

  // Upsert huge rows for a single interleaved key stripe: the PKs
  // 'stripe, stripe + stride, stripe + 2*stride, ...' that are < pk_end.
  // Rows are written in sub-batches of at most max_rows_per_write_batch_ rows.
  // Writing distinct stripes and flushing between them yields multiple
  // DiskRowSets whose [min, max] key bounds all overlap.
  void UpsertBigRowStripe(int stripe, int stride, int pk_end) {
    const vector<int>& cell_sizes = GetParam().cell_sizes;

    vector<int> pks;
    for (int pk = stripe; pk < pk_end; pk += stride) {
      pks.push_back(pk);
    }

    for (size_t start = 0; start < pks.size();
         start += max_rows_per_write_batch_) {
      const size_t end =
          std::min<size_t>(start + max_rows_per_write_batch_, pks.size());

      WriteRequestPB req;
      req.set_tablet_id(kTabletId);
      CHECK_OK(SchemaToPB(schema_, req.mutable_schema()));

      RowOperationsPBEncoder encoder(req.mutable_row_operations());
      for (size_t i = start; i < end; i++) {
        KuduPartialRow row(&schema_);
        CHECK_OK(row.SetInt32("pk1", pks[i]));
        CHECK_OK(row.SetInt32("val_int_a", rand()));
        const int str_len = cell_sizes[rand() % cell_sizes.size()];
        for (int c = 0; c < static_cast<int>(schema_.num_columns()); c++) {
          const ColumnSchema& col = schema_.column(c);
          if (col.type_info()->type() == STRING) {
            CHECK_OK(row.SetStringCopy(c, RandomString(str_len)));
          }
        }
        encoder.Add(RowOperationsPB::UPSERT, row);
      }

      WriteWithRetries(req);
    }
  }

  // Upsert rows with PKs in [pk_start, pk_end), splitting the range into
  // sub-batches of at most max_rows_per_write_batch_ rows per Write RPC.
  // Returns the timestamp of the last write.
  uint64_t UpsertBigRowRange(int pk_start, int pk_end) {
    uint64_t last_ts = 0;
    for (int start = pk_start; start < pk_end;
         start += max_rows_per_write_batch_) {
      const int end = std::min(start + max_rows_per_write_batch_, pk_end);
      last_ts = UpsertBigRowSubBatch(start, end);
    }
    return last_ts;
  }

  Schema schema_;
  int max_rows_per_write_batch_ = 1;
  unique_ptr<MiniTabletServer> mini_server_;
  std::shared_ptr<rpc::Messenger> messenger_;

  // RPC proxies (all required by CreateTsClientProxies).
  unique_ptr<TabletCopyServiceProxy>           tablet_copy_proxy_;
  unique_ptr<TabletServerServiceProxy>         proxy_;
  unique_ptr<TabletServerAdminServiceProxy>    admin_proxy_;
  unique_ptr<consensus::ConsensusServiceProxy> consensus_proxy_;
  unique_ptr<server::GenericServiceProxy>      generic_proxy_;
};

// Insert many rows, then start multiple read threads that each scan a disjoint
// PK range in parallel, and verify the combined result covers every row.
TEST_P(MemoryLimitOvershootReadTest, TestParallelRangeReads) {
  constexpr int kNumReadThreads = 20;
  constexpr int kNumRows = 8000;
  constexpr int kRangeSize = kNumRows / kNumReadThreads;

  for (int start = 0; start < kNumRows; start += kRangeSize) {
    UpsertBigRowRange(start, start + kRangeSize);
  }

  PeakMemorySampler sampler;
  CountDownLatch start_latch(1);
  std::atomic<int64_t> total_rows_read(0);

  vector<thread> threads;
  threads.reserve(kNumReadThreads);
  for (int i = 0; i < kNumReadThreads; i++) {
    threads.emplace_back([&, i]() {
      start_latch.Wait();
      NewScanRequestPB new_scan = MakeRangedLatestScanRequest(
          kTabletId, schema_, i * kRangeSize, i * kRangeSize + kRangeSize);
      int64_t rows = 0;
      OpenAndDrainScan(proxy_.get(), new_scan, schema_, &rows);
      total_rows_read.fetch_add(rows, std::memory_order_relaxed);
    });
  }

  start_latch.CountDown();  // release all reader threads at once
  for (auto& t : threads) {
    t.join();
  }
  const int64_t peak = sampler.Stop();

  LOG(INFO) << "[ParallelRangeReads] Total rows read across "
            << kNumReadThreads << " threads: " << total_rows_read.load();
  NO_FATALS(AssertPeakWithinBudget(peak, "ParallelRangeReads"));
  ASSERT_EQ(total_rows_read.load(), kNumRows)
      << "Parallel ranged reads returned wrong number of rows: "
      << total_rows_read.load() << " instead of " << kNumRows;
}

// Repeatedly upsert the same rows to accumulate deltas, then run a single diff
// scan spanning all the writes and verify it observes every row exactly once.
TEST_P(MemoryLimitOvershootReadTest, TestDiffScanAfterUpserts) {
  constexpr int kNumRows = 200;
  constexpr int kRounds = 60;

  const uint64_t diff_scan_start_ts = mini_server_->server()->clock()->Now().ToUint64();

  uint64_t last_write_ts = 0;
  for (int r = 0; r < kRounds; r++) {
    last_write_ts = UpsertBigRowRange(0, kNumRows);
  }
  const uint64_t diff_scan_end_ts = last_write_ts + 1;

  const Schema projection = MakeDiffScanProjection(schema_);
  const NewScanRequestPB new_scan =
      MakeDiffScanRequest(kTabletId, projection, diff_scan_start_ts, diff_scan_end_ts);

  PeakMemorySampler sampler;
  int64_t total_rows_read = 0;
  OpenAndDrainScan(proxy_.get(), new_scan, projection, &total_rows_read);
  const int64_t peak = sampler.Stop();

  LOG(INFO) << "[DiffScan] Total rows read: " << total_rows_read;
  NO_FATALS(AssertPeakWithinBudget(peak, "DiffScan"));
  ASSERT_EQ(total_rows_read, kNumRows)
      << "Diff scan returned wrong number of rows: " << total_rows_read
      << " instead of " << kNumRows;
}

// Upsert 200 rows 20 times, pinning T0 just after the first write, then
// repeatedly scan READ_AT_SNAPSHOT at T0 so the server must reconstruct the
// historical snapshot by merging MemRowSet and DeltaStores on each scan.
TEST_P(MemoryLimitOvershootReadTest, TestSnapshotReadAfterUpserts) {
  constexpr int kNumRows = 200;
  constexpr int kRounds = 20;
  constexpr int kReadRounds = 20;

  const uint64_t first_write_ts = UpsertBigRowRange(0, kNumRows);
  // Shift one tick forward so the first write is included in the snapshot.
  const uint64_t t0 = first_write_ts + 1;
  for (int r = 1; r < kRounds; r++) {
    UpsertBigRowRange(0, kNumRows);
  }

  const NewScanRequestPB new_scan =
      MakeSnapshotScanRequest(kTabletId, schema_, t0);

  PeakMemorySampler sampler;
  int64_t total_rows_read = 0;
  for (int i = 0; i < kReadRounds; i++) {
    int64_t rows = 0;
    OpenAndDrainScan(proxy_.get(), new_scan, schema_, &rows);
    total_rows_read += rows;
  }
  const int64_t peak = sampler.Stop();

  LOG(INFO) << "[SnapshotRead] Total rows read across "
            << kReadRounds << " rounds: " << total_rows_read;
  NO_FATALS(AssertPeakWithinBudget(peak, "SnapshotRead"));
  ASSERT_GT(total_rows_read, 0)
      << "Snapshot scan returned no rows; T0=" << t0;
}

// Upsert 200 rows 20 times, then run a simple READ_LATEST full-table scan while
// sampling peak memory.
TEST_P(MemoryLimitOvershootReadTest, TestFullReadAfterUpserts) {
  constexpr int kNumRows = 200;
  constexpr int kRounds = 20;

  for (int r = 0; r < kRounds; r++) {
    UpsertBigRowRange(0, kNumRows);
  }

  const NewScanRequestPB new_scan = MakeLatestScanRequest(kTabletId, schema_);

  PeakMemorySampler sampler;
  int64_t total_rows_read = 0;
  OpenAndDrainScan(proxy_.get(), new_scan, schema_, &total_rows_read);
  const int64_t peak = sampler.Stop();

  LOG(INFO) << "[FullRead] Total rows read: " << total_rows_read;
  NO_FATALS(AssertPeakWithinBudget(peak, "FullRead"));
  ASSERT_GT(total_rows_read, 0) << "Full read returned no rows";
}

// Build several overlapping on-disk rowsets by writing interleaved key stripes
// (flushing after each), then run an ORDERED full-table scan.  ORDERED scans
// are served by the MergeIterator, whose per-sub-iterator internal buffer is a
// fixed kMergeRowBuffer (1024) rows by default -- independent of the scanner's
// output batch size.  For 'huge' tables/projections the buffer is reduced to
// --merge_iterator_buffer_size_rows_huge to bound the large-cell data buffered
// per sub-iterator.  With multiple overlapping rowsets holding large cells this
// exercises a memory path distinct from the (scanner_batch_size_rows) output
// batch the other tests hit.
TEST_P(MemoryLimitOvershootReadTest, TestOrderedMergeScanOverlappingRowsets) {
  constexpr int kNumStripes = 125;
  constexpr int kNumRows = 8000;

  // Keep the overlapping rowsets separate: rowset compaction would otherwise
  // merge the small DiskRowSets and eliminate the multi-way merge.
  FLAGS_enable_rowset_compaction = false;

  // Each stripe covers PKs {s, s + kNumStripes, ...}, so every stripe's
  // [min, max] key bounds span nearly the whole range and thus overlap.  A
  // flush after each stripe materializes it as its own DiskRowSet.
  for (int stripe = 0; stripe < kNumStripes; stripe++) {
    UpsertBigRowStripe(stripe, kNumStripes, kNumRows);
    NO_FATALS(FlushTablet());
  }

  ASSERT_GE(NumRowSets(), kNumStripes)
      << "expected at least " << kNumStripes << " overlapping rowsets so the "
      << "ORDERED scan exercises a multi-way merge";

  // ORDERED scans must be snapshot reads; pin the snapshot at "now" so it sees
  // all the rows written above.
  const uint64_t snap_ts = mini_server_->server()->clock()->Now().ToUint64();
  const NewScanRequestPB new_scan =
      MakeOrderedSnapshotScanRequest(kTabletId, schema_, snap_ts);

  PeakMemorySampler sampler;
  int64_t total_rows_read = 0;
  OpenAndDrainScan(proxy_.get(), new_scan, schema_, &total_rows_read);
  const int64_t peak = sampler.Stop();

  LOG(INFO) << "[OrderedMergeScan] Rowsets: " << NumRowSets()
            << ", total rows read: " << total_rows_read;
  NO_FATALS(AssertPeakWithinBudget(peak, "OrderedMergeScan"));
  ASSERT_EQ(total_rows_read, kNumRows)
      << "Ordered merge scan returned wrong number of rows: " << total_rows_read
      << " instead of " << kNumRows;
}

// Run every scenario against three layouts:
//   - a single 1 MiB STRING column,
//   - a single column whose rows are either tiny (100 B) or 1 MiB, and
//   - 32 columns of 32 KiB each (~1 MiB of STRING data per row total).
INSTANTIATE_TEST_SUITE_P(
    BigColumnConfigs, MemoryLimitOvershootReadTest,
    ::testing::Values(
        ReadTestParam{{1 << 20}, 1, "Big1MiBx1", /*table_level_huge=*/false},
        ReadTestParam{{1 << 15}, 32, "Small32KiBx32", /*table_level_huge=*/true}),
    [](const ::testing::TestParamInfo<ReadTestParam>& info) {
      return info.param.test_case_name;
    });

}  // namespace tserver
}  // namespace kudu

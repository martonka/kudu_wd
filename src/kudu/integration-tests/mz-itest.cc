#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>

#include "kudu/client/client.h"
#include "kudu/client/schema.h"
#include "kudu/client/shared_ptr.h" // IWYU pragma: keep
#include "kudu/common/common.pb.h"
#include "kudu/common/partial_row.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/integration-tests/cluster_itest_util.h"
#include "kudu/master/catalog_manager.h"
#include "kudu/master/master-test-util.h"
#include "kudu/master/master.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master.proxy.h"
#include "kudu/master/mini_master.h"
#include "kudu/mini-cluster/internal_mini_cluster.h"
#include "kudu/rpc/messenger.h"
#include "kudu/util/atomic.h"
#include "kudu/util/cow_object.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/scoped_cleanup.h"
#include "kudu/util/status.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/test_util.h"

using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using kudu::client::KuduColumnSchema;
using kudu::client::KuduSchema;
using kudu::client::KuduSchemaBuilder;
using kudu::client::KuduTableCreator;
using kudu::cluster::InternalMiniCluster;
using kudu::cluster::InternalMiniClusterOptions;
using kudu::itest::CreateTabletServerMap;
using kudu::itest::TabletServerMap;
using kudu::master::MasterServiceProxy;
using kudu::rpc::Messenger;
using kudu::rpc::MessengerBuilder;
using std::nullopt;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_ptr;
using std::vector;
using strings::Substitute;


DECLARE_bool(enable_multi_raft_heartbeat_batcher);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_bool(log_preallocate_segments);

namespace kudu {

const char* kTableName = "test_table";

class CreateTableStressTest : public KuduTest {
 public:
  CreateTableStressTest() {
    KuduSchemaBuilder b;
    b.AddColumn("key")->Type(KuduColumnSchema::INT32)->NotNull()->PrimaryKey();
    b.AddColumn("v1")->Type(KuduColumnSchema::INT64)->NotNull();
    b.AddColumn("v2")->Type(KuduColumnSchema::STRING)->NotNull();
    CHECK_OK(b.Build(&schema_));
  }

  void SetUp() override {
    // Make heartbeats faster to speed test runtime.
    FLAGS_heartbeat_interval_ms = 100;

    // Don't preallocate log segments, since we're creating thousands
    // of tablets here. If each preallocates 64M or so, we use
    // a ton of disk space in this test, and it fails on normal
    // sized /tmp dirs.
    // TODO: once we collapse multiple tablets into shared WAL files,
    // this won't be necessary.
    FLAGS_log_preallocate_segments = false;

    KuduTest::SetUp();
    InternalMiniClusterOptions opts;
    opts.num_tablet_servers = 3;
    cluster_.reset(new InternalMiniCluster(env_, opts));
    ASSERT_OK(cluster_->Start());

    ASSERT_OK(KuduClientBuilder()
                     .add_master_server_addr(cluster_->mini_master()->bound_rpc_addr_str())
                     .Build(&client_));

  }

  void TearDown() override {
    cluster_->Shutdown();
  }

  void CreateBigTable(const string& table_name, int num_tablets);

 protected:
  client::sp::shared_ptr<KuduClient> client_;
  unique_ptr<InternalMiniCluster> cluster_;
  KuduSchema schema_;
};

void CreateTableStressTest::CreateBigTable(const string& table_name, int num_tablets) {
  vector<const KuduPartialRow*> split_rows;
  int num_splits = num_tablets - 1; // 4 tablets == 3 splits.
  // Let the "\x8\0\0\0" keys end up in the first split; start splitting at 1.
  for (int i = 1; i <= num_splits; i++) {
    KuduPartialRow* row = schema_.NewRow();
    CHECK_OK(row->SetInt32(0, i));
    split_rows.push_back(row);
  }

  unique_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
  ASSERT_OK(table_creator->table_name(table_name)
            .schema(&schema_)
            .set_range_partition_columns({ "key" })
            .split_rows(split_rows)
            .num_replicas(3)
            .wait(false)
            .Create());
}

TEST_F(CreateTableStressTest, CreateAndRun) {
  // FLAGS_enable_multi_raft_heartbeat_batcher = true;
  // constexpr int kNumTestTablets = 1;
  // string table_name = "test_table";
  // NO_FATALS(CreateBigTable(table_name, kNumTestTablets));
  // master::GetTableLocationsResponsePB resp;
  // ASSERT_OK(WaitForRunningTabletCount(cluster_->mini_master(), table_name,
  //                                     kNumTestTablets, &resp));
  // LOG(INFO) << "Created table successfully!";

  // while (!cfs::exists("/tmp/mz_trigger")) {
  //   std::this_thread::sleep_for(1s);
  // }
  // cfs::remove("/tmp/mz_trigger");

}

} // namespace kudu

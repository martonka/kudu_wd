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

#include <kudu/consensus/multi_raft_batcher.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <mutex>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus.proxy.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/gutil/macros.h"
#include "kudu/rpc/periodic.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"

namespace kudu {
class DnsResolver;
namespace rpc {
class Messenger;
}  // namespace rpc
}  // namespace kudu

METRIC_DEFINE_counter(server,
                      heartbeat_batch_count,
                      "Heartbeat batch messages",
                      kudu::MetricUnit::kRequests,
                      "Number of heartbeat batch messages sent out",
                      kudu::MetricLevel::kInfo);

METRIC_DEFINE_counter(server,
                      no_op_heartbeat_count,
                      "Noop Heartbeat messages",
                      kudu::MetricUnit::kRequests,
                      "Number of no-op heathbeat messages sent",
                      kudu::MetricLevel::kInfo);

DEFINE_int32(multi_raft_heartbeat_interval_ms,
             500,
             "The heartbeat interval for batch Raft replication.");
TAG_FLAG(multi_raft_heartbeat_interval_ms, experimental);
TAG_FLAG(multi_raft_heartbeat_interval_ms, runtime);
DECLARE_int32(raft_heartbeat_interval_ms);

DEFINE_bool(enable_multi_raft_heartbeat_batcher,
            false,
            "Whether to enable the batching of raft heartbeats.");
TAG_FLAG(enable_multi_raft_heartbeat_batcher, experimental);
TAG_FLAG(enable_multi_raft_heartbeat_batcher, runtime);

DEFINE_int32(multi_raft_batch_size, 10, "Maximum batch size for a multi-raft consensus payload.");
TAG_FLAG(multi_raft_batch_size, experimental);
TAG_FLAG(multi_raft_batch_size, runtime);

DECLARE_int32(consensus_rpc_timeout_ms);

namespace kudu {
namespace consensus {

scoped_refptr<Counter> heartbeat_batch_count;
scoped_refptr<kudu::Counter> no_op_heartbeat_count;

using kudu::DnsResolver;
using rpc::PeriodicTimer;

namespace {
BatchedNoOpConsensusRequestPB ToBatchRequest(const ConsensusRequestPB& req) {
  BatchedNoOpConsensusRequestPB res;
  res.set_tablet_id(req.tablet_id());
  res.set_caller_term(req.caller_term());
  *res.mutable_preceding_id() = req.preceding_id();
  res.set_committed_index(req.committed_index());
  res.set_all_replicated_index(req.all_replicated_index());
  res.set_safe_timestamp(req.safe_timestamp());
  res.set_last_idx_appended_to_leader(req.last_idx_appended_to_leader());

  return res;
}
}  // namespace
struct MultiRaftHeartbeatBatcher::MultiRaftConsensusData {
  MultiRaftConsensusRequestPB batch_req;
  MultiRaftConsensusResponsePB batch_res;
  rpc::RpcController controller;
  std::vector<HeartbeatResponseCallback> response_callback_data;
};

MultiRaftHeartbeatBatcher::MultiRaftHeartbeatBatcher(
    const kudu::HostPort& hostport,
    DnsResolver* dns_resolver,
    std::shared_ptr<kudu::rpc::Messenger> messenger)
    : messenger_(messenger),
      consensus_proxy_(std::make_unique<ConsensusServiceProxy>(messenger, hostport, dns_resolver)),
      current_batch_(std::make_shared<MultiRaftConsensusData>()),
      buffer_start_idx(0) {}

void MultiRaftHeartbeatBatcher::Start() {
  std::weak_ptr<MultiRaftHeartbeatBatcher> const weak_peer = shared_from_this();
  const auto flush_interval =
      std::min(FLAGS_raft_heartbeat_interval_ms / 2, FLAGS_multi_raft_heartbeat_interval_ms);
  if (flush_interval < FLAGS_multi_raft_heartbeat_interval_ms) {
    LOG(WARNING) << "multi_raft_heartbeat_interval_ms should be at most half of"
                 << "heartbeat_interval_ms, forcing its value to: " << flush_interval;
  }
  batch_sender_ = PeriodicTimer::Create(
      messenger_,
      [weak_peer]() {
        if (auto peer = weak_peer.lock()) {
          peer->PrepareAndSendBatchRequest();
        }
      },
      MonoDelta::FromMilliseconds(flush_interval));
  batch_sender_->Start();
}

MultiRaftHeartbeatBatcher::~MultiRaftHeartbeatBatcher() = default;

uint64_t MultiRaftHeartbeatBatcher::AddRequestToBatch(ConsensusRequestPB* request,
                                                      HeartbeatResponseCallback callback) {
  std::shared_ptr<MultiRaftConsensusData> data = nullptr;
  uint64_t res;
  VLOG(1) << "Adding request to batch ";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request->has_caller_uuid()) {
      if (!current_batch_->batch_req.has_caller_uuid()) {
        current_batch_->batch_req.set_caller_uuid(request->caller_uuid());
      }
      DCHECK(request->caller_uuid() == current_batch_->batch_req.caller_uuid());
    }

    if (request->has_dest_uuid()) {
      if (!current_batch_->batch_req.has_dest_uuid()) {
        current_batch_->batch_req.set_dest_uuid(request->dest_uuid());
      }
      DCHECK(request->dest_uuid() == current_batch_->batch_req.dest_uuid());
    }

    res = buffer_start_idx.load(std::memory_order_relaxed) +
          current_batch_->response_callback_data.size();

    current_batch_->response_callback_data.push_back(std::move(callback));

    // Add a ConsensusRequestPB to the batch
    *current_batch_->batch_req.add_consensus_requests() = ToBatchRequest(*request);

    if (FLAGS_multi_raft_batch_size > 0 &&
        current_batch_->response_callback_data.size() >= FLAGS_multi_raft_batch_size) {
      data = PrepareNextBatchRequestUnlocked();
    }
  }
  if (data) {
    SendBatchRequest(data);
  }
  return res;
}

void MultiRaftHeartbeatBatcher::PrepareAndSendBatchRequest() {
  std::shared_ptr<MultiRaftConsensusData> data;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data = PrepareNextBatchRequestUnlocked();
  }
  SendBatchRequest(data);
}

std::shared_ptr<MultiRaftHeartbeatBatcher::MultiRaftConsensusData>
MultiRaftHeartbeatBatcher::PrepareNextBatchRequestUnlocked() {
  if (current_batch_->response_callback_data.size() == 0) {
    return nullptr;
  }
  batch_sender_->Snooze();
  auto data = std::move(current_batch_);
  current_batch_ = std::make_shared<MultiRaftConsensusData>();
  buffer_start_idx.fetch_add(current_batch_->response_callback_data.size(),
                             std::memory_order_relaxed);
  return data;
}

void MultiRaftHeartbeatBatcher::IncrementNoOpPackageCounter() {
  no_op_heartbeat_count->Increment();
}

void MultiRaftHeartbeatBatcher::SendBatchRequest(std::shared_ptr<MultiRaftConsensusData> data) {
  if (!data) {
    return;
  }
  VLOG(1) << "Sending BatchRequest";
  heartbeat_batch_count->Increment();

  data->controller.Reset();
  // should we just add a separate flag?
  data->controller.set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));

  DCHECK(data->batch_req.IsInitialized());

  consensus_proxy_->MultiRaftUpdateConsensusAsync(
      data->batch_req, &data->batch_res, &data->controller, [data, inst = shared_from_this()]() {
        inst->MultiRaftUpdateHeartbeatResponseCallback(data);
      });
}

void MultiRaftHeartbeatBatcher::MultiRaftUpdateHeartbeatResponseCallback(
    std::shared_ptr<MultiRaftConsensusData> data) {
  auto status = data->controller.status();
  for (int i = 0; i < data->batch_req.consensus_requests_size(); i++) {
    const auto* resp = data->batch_res.consensus_responses_size() > i
                           ? &data->batch_res.consensus_responses(i)
                           : nullptr;
    data->response_callback_data[i](data->controller, data->batch_res, resp);
  }
}

MultiRaftManager::MultiRaftManager(std::shared_ptr<rpc::Messenger> messenger,
                                   kudu::DnsResolver* dns_resolver,
                                   const scoped_refptr<MetricEntity>& entity)
    : messenger_(messenger), dns_resolver_(dns_resolver) {
  no_op_heartbeat_count = METRIC_no_op_heartbeat_count.Instantiate(entity);
  heartbeat_batch_count = METRIC_heartbeat_batch_count.Instantiate(entity);
}

MultiRaftHeartbeatBatcherPtr MultiRaftManager::AddOrGetBatcher(
    const kudu::consensus::RaftPeerPB& remote_peer_pb) {
  if (!FLAGS_enable_multi_raft_heartbeat_batcher) {
    return nullptr;
  }

  auto hostport = HostPortFromPB(remote_peer_pb.last_known_addr());
  std::lock_guard<std::mutex> lock(mutex_);
  MultiRaftHeartbeatBatcherPtr batcher;

  // After taking the lock, check if there is already a batcher
  // for the same remote host and return it.
  auto res = batchers_.find(hostport);
  if (res != batchers_.end() && (batcher = res->second.lock())) {
    return batcher;
  }
  batcher = std::make_shared<MultiRaftHeartbeatBatcher>(hostport, dns_resolver_, messenger_);
  batchers_[hostport] = batcher;
  batcher->Start();
  return batcher;
}

}  // namespace consensus
}  // namespace kudu

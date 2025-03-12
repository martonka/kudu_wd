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

#include "multi_raft_batcher.h"
#include <functional>
#include <memory>
#include <utility>
#include <mutex>
#include <vector>

#include "kudu/common/wire_protocol.h"

#include "kudu/consensus/consensus_meta.h"
#include "kudu/rpc/proxy.h"

#include "kudu/rpc/periodic.h"

#include "kudu/util/flag_tags.h"

#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus.proxy.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/util/metrics.h"
#include "kudu/util/monotime.h"

using namespace std::literals;
using namespace std::placeholders;

METRIC_DEFINE_counter(server, heartbeat_batch_count,
                      "Heartbeat batch messages", kudu::MetricUnit::kRequests,
                      "Number of heartbeat batch messages sent out",
                      kudu::MetricLevel::kInfo);



METRIC_DEFINE_counter(server, no_op_heartbeat_count,
                      "Noop Heartbeat messages", kudu::MetricUnit::kRequests,
                      "Number of no-op heathbeat messages sent",
                      kudu::MetricLevel::kInfo);


DEFINE_int32(multi_raft_heartbeat_interval_ms,
             500,
             "The heartbeat interval for batch Raft replication.");
TAG_FLAG(multi_raft_heartbeat_interval_ms, experimental);
TAG_FLAG(multi_raft_heartbeat_interval_ms, runtime);

DEFINE_bool(enable_multi_raft_heartbeat_batcher,
            true,
            "Whether to enable the batching of raft heartbeats.");
TAG_FLAG(enable_multi_raft_heartbeat_batcher, experimental);
TAG_FLAG(enable_multi_raft_heartbeat_batcher, runtime);

DEFINE_int32(multi_raft_batch_size, 30, "Maximum batch size for a multi-raft consensus payload.");
TAG_FLAG(multi_raft_batch_size, experimental);
TAG_FLAG(multi_raft_batch_size, runtime);


DECLARE_int32(consensus_rpc_timeout_ms);

namespace kudu {
namespace consensus {

scoped_refptr<Counter> heartbeat_batch_count;
scoped_refptr<kudu::Counter> no_op_heartbeat_count;

using rpc::PeriodicTimer;
using kudu::DnsResolver;

class ConsensusServiceProxy;
typedef std::unique_ptr<ConsensusServiceProxy> ConsensusServiceProxyPtr;

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
}
struct MultiRaftHeartbeatBatcher::MultiRaftConsensusData  {
  MultiRaftConsensusRequestPB batch_req;
  MultiRaftConsensusResponsePB batch_res;
  rpc::RpcController controller;
  std::vector<ResponseCallbackData> response_callback_data;
};

MultiRaftHeartbeatBatcher::MultiRaftHeartbeatBatcher(const kudu::HostPort& hostport,
                                                     DnsResolver* dns_resolver,
                                                     std::shared_ptr<kudu::rpc::Messenger> messenger)
    : messenger_(messenger),
      consensus_proxy_(std::make_unique<ConsensusServiceProxy>(messenger, hostport, dns_resolver)),
      current_batch_(std::make_shared<MultiRaftConsensusData>()) {}

void MultiRaftHeartbeatBatcher::Start() {
  std::weak_ptr<MultiRaftHeartbeatBatcher> const weak_peer = shared_from_this();
  batch_sender_ = PeriodicTimer::Create(
      messenger_,
      [weak_peer]() {
        if (auto peer = weak_peer.lock()) {
          peer->PrepareAndSendBatchRequest();
        }
      },
      MonoDelta::FromMilliseconds(FLAGS_multi_raft_heartbeat_interval_ms));
  batch_sender_->Start();
}

MultiRaftHeartbeatBatcher::~MultiRaftHeartbeatBatcher() = default;

void MultiRaftHeartbeatBatcher::AddRequestToBatch(ConsensusRequestPB* request,
                                                  ConsensusResponsePB* response,
                                                  HeartbeatResponseCallback callback) {
  std::string reqs = request->DebugString();
  std::shared_ptr<MultiRaftConsensusData> data = nullptr;
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

    current_batch_->response_callback_data.push_back({response, std::move(callback)});
    // Add a ConsensusRequestPB to the batch
    *current_batch_->batch_req.add_consensus_requests() = ToBatchRequest(*request);

    if (FLAGS_multi_raft_batch_size > 0 &&
        current_batch_->response_callback_data.size() >= FLAGS_multi_raft_batch_size) {
      data = PrepareNextBatchRequest();
    }
  }
  if (data) {
    SendBatchRequest(data);
  }
}

void MultiRaftHeartbeatBatcher::PrepareAndSendBatchRequest() {
  std::shared_ptr<MultiRaftConsensusData> data;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    data = PrepareNextBatchRequest();
  }
  SendBatchRequest(data);
}

std::shared_ptr<MultiRaftHeartbeatBatcher::MultiRaftConsensusData>
MultiRaftHeartbeatBatcher::PrepareNextBatchRequest() {
  if (current_batch_->batch_req.consensus_requests_size() == 0) {
    return nullptr;
  }
  batch_sender_->Snooze();
  auto data = std::move(current_batch_);
  current_batch_ = std::make_shared<MultiRaftConsensusData>();
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
  data->controller.set_timeout(MonoDelta::FromMilliseconds(
      FLAGS_consensus_rpc_timeout_ms));

  DCHECK(data->batch_req.IsInitialized());

  consensus_proxy_->MultiRaftUpdateConsensusAsync(
      data->batch_req,
      &data->batch_res,
      &data->controller,
      [data,inst=shared_from_this()](){
        inst->MultiRaftUpdateHeartbeatResponseCallback(data);
      }
  );
}

void MultiRaftHeartbeatBatcher::MultiRaftUpdateHeartbeatResponseCallback(
    std::shared_ptr<MultiRaftConsensusData> data) {
  auto status = data->controller.status();
  auto ss = data->batch_res.DebugString();
  if (!status.ok()) {
    LOG(INFO) << "MultiRaftUpdate not ok, status: " << status.ToString() << std::endl;
    return;
  }
  for (int i = 0; i < data->batch_req.consensus_requests_size(); i++) {
    auto callback_data = data->response_callback_data[i];
    // todo check local status
    // mzzzz
    //callback_data.resp->Swap(data->batch_res.mutable_consensus_responses(i));

    LOG(INFO) << "calling multiraftupdateheartbeatresponse callback and is init:" << callback_data.resp->IsInitialized() << std::endl;
    callback_data.callback(data->controller, data->batch_res, data->batch_res.consensus_responses(i));
  }
}

MultiRaftManager::MultiRaftManager(std::shared_ptr<rpc::Messenger> messenger, kudu::DnsResolver* dns_resolver, const scoped_refptr<MetricEntity>& entity)
    : messenger_(messenger),
      dns_resolver_(dns_resolver) {
        no_op_heartbeat_count = METRIC_no_op_heartbeat_count.Instantiate(entity);
        heartbeat_batch_count = METRIC_heartbeat_batch_count.Instantiate(entity);
      }

MultiRaftHeartbeatBatcherPtr MultiRaftManager::AddOrGetBatcher(const kudu::consensus::RaftPeerPB& remote_peer_pb) {
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
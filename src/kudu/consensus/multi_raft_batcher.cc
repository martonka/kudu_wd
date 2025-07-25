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

#include "kudu/consensus/multi_raft_batcher.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <type_traits>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus.proxy.h"
#include "kudu/consensus/metadata.pb.h"
#include "kudu/consensus/multi_raft_consensus_data.h"
#include "kudu/gutil/macros.h"
#include "kudu/rpc/periodic.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/monotime.h"
#include "kudu/util/status.h"
#include "kudu/util/threadpool.h"

namespace kudu {
class DnsResolver;

namespace rpc {
class Messenger;
}  // namespace rpc
}  // namespace kudu

DECLARE_int32(consensus_rpc_timeout_ms);
DECLARE_int32(raft_heartbeat_interval_ms);

DEFINE_int32(multi_raft_heartbeat_interval_ms,
             100,
             "The heartbeat interval for batch Raft replication.");
TAG_FLAG(multi_raft_heartbeat_interval_ms, experimental);

DEFINE_bool(enable_multi_raft_heartbeat_batcher,
            false,
            "Whether to enable the batching of raft heartbeats.");
TAG_FLAG(enable_multi_raft_heartbeat_batcher, experimental);

DEFINE_int32(multi_raft_batch_size, 30, "Maximum batch size for a multi-raft consensus payload.");
TAG_FLAG(multi_raft_batch_size, experimental);

namespace kudu {
namespace consensus {

using kudu::DnsResolver;
using rpc::PeriodicTimer;

namespace {}  // namespace

uint64_t MultiRaftHeartbeatBatcher::Subscribe(const PeriodicHeartbeater& heartbeater) {
  DCHECK(!closed_);
  std::lock_guard<std::mutex> l(heartbeater_lock_);
  auto it = peers_.insert({next_id, heartbeater});
  DCHECK(it.second) << "Peer with id " << next_id << " already exists";
  queue_.push_back(
      {MonoTime::Now() + MonoDelta::FromMilliseconds(FLAGS_raft_heartbeat_interval_ms), next_id});
  return next_id++;
}

void MultiRaftHeartbeatBatcher::Unsubscribe(uint64_t id) {
  std::lock_guard<std::mutex> l(heartbeater_lock_);
  DCHECK(peers_.count(id) == 1);
  peers_.erase(id);
}

MultiRaftHeartbeatBatcher::MultiRaftHeartbeatBatcher(
    const kudu::HostPort& hostport,
    DnsResolver* dns_resolver,
    std::shared_ptr<kudu::rpc::Messenger> messenger,
    int flush_interval,
    ThreadPoolToken* raft_pool_token)
    : messenger_(messenger),
      consensus_proxy_(std::make_unique<ConsensusServiceProxy>(messenger, hostport, dns_resolver)),
      flush_interval_(flush_interval),
      raft_pool_token_(raft_pool_token),
      closed_(false) {}

void MultiRaftHeartbeatBatcher::StartTimer() {
  std::weak_ptr<MultiRaftHeartbeatBatcher> const weak_peer = shared_from_this();
  heartbeat_timer_ = PeriodicTimer::Create(
      messenger_,
      [weak_peer]() {
        if (auto peer = weak_peer.lock()) {
          peer->PrepareNextBatch();
        }
      },
      MonoDelta::FromMilliseconds(flush_interval_));
  heartbeat_timer_->Start();
}

void MultiRaftHeartbeatBatcher::PrepareNextBatch() {
  std::lock_guard<std::mutex> lock(heartbeater_lock_);
  if (closed_) {
    return;  // Batcher is closed, raft_pool_token_ might be invalid.
  }
  auto send_until =
      MonoTime::Now() + MonoDelta::FromMilliseconds(FLAGS_multi_raft_heartbeat_interval_ms);

  std::vector<ScheduledCallback> current_calls;
  while (queue_.size() && queue_.front().time <= send_until) {
    auto front = queue_.front();
    queue_.pop_front();
    auto peer_it = peers_.find(front.id);
    if (peer_it == peers_.end()) {
      continue;  // Peer was unsubscribed.
    }
    auto next_time = front.time + MonoDelta::FromMilliseconds(FLAGS_raft_heartbeat_interval_ms);
    queue_.push_back({next_time, front.id});

    current_calls.emplace_back(peer_it->second, front.id);
  }
  if (!current_calls.empty()) {
    auto this_ptr = shared_from_this();
    KUDU_WARN_NOT_OK(
        raft_pool_token_->Submit([this_ptr, current_calls = std::move(current_calls)]() {
          this_ptr->SendOutScheduled(current_calls);
        }),
        "Failed to submit multi-raft heartbeat batcher task");
  }
}

void MultiRaftHeartbeatBatcher::SendBatchRequest(std::shared_ptr<MultiRaftConsensusData> data) {
  DCHECK(data);
  DCHECK(data->batch_req.IsInitialized());

  VLOG(1) << "Sending BatchRequest with size: " << data->batch_req.consensus_requests_size();

  data->controller.set_timeout(MonoDelta::FromMilliseconds(FLAGS_consensus_rpc_timeout_ms));

  // Copy data shared pointer to ensure that it remains valid
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

void MultiRaftHeartbeatBatcher::Shutdown() {
  std::lock_guard<std::mutex> lock(heartbeater_lock_);
  if (closed_) {
    return;  // Already closed.
  }
  closed_ = true;
  if (heartbeat_timer_) {
    heartbeat_timer_->Stop();
    heartbeat_timer_.reset();
  }
}

void MultiRaftHeartbeatBatcher::SendOutScheduled(
    const std::vector<ScheduledCallback>& scheduled_callbacks) {
  // No need to hold the lock here, as we are not modifying the state of the batcher.
  auto data = std::make_shared<MultiRaftConsensusData>(FLAGS_multi_raft_batch_size);

  for (const auto& cb : scheduled_callbacks) {
    cb.heartbeater.heartbeat_callback(data.get());
    if (data->batch_req.consensus_requests_size() >= FLAGS_multi_raft_batch_size) {
      SendBatchRequest(data);
      data = std::make_shared<MultiRaftConsensusData>(FLAGS_multi_raft_batch_size);
    }
  }
  if (data->batch_req.consensus_requests_size() > 0) {
    SendBatchRequest(data);
  }
}

MultiRaftManager::MultiRaftManager(kudu::DnsResolver* dns_resolver,
                                   const scoped_refptr<MetricEntity>& entity)
    : dns_resolver_(dns_resolver) {
  flush_interval_ =
      std::min(FLAGS_multi_raft_heartbeat_interval_ms, FLAGS_consensus_rpc_timeout_ms / 2);
  if (flush_interval_ != FLAGS_multi_raft_heartbeat_interval_ms) {
    LOG(ERROR) << "multi_raft_heartbeat_interval_ms should not be more than "
               << " consensus_rpc_timeout_ms / 2. , forcing multi_raft_heartbeat_interval_ms = "
               << flush_interval_;
  }
}

void MultiRaftManager::Init(const std::shared_ptr<rpc::Messenger>& messenger,
                            ThreadPool* raft_pool) {
  messenger_ = messenger;
  raft_pool_ = raft_pool;
  raft_pool_token_ = raft_pool_->NewToken(ThreadPool::ExecutionMode::CONCURRENT);
}

MultiRaftHeartbeatBatcherPtr MultiRaftManager::AddOrGetBatcher(
    const kudu::consensus::RaftPeerPB& remote_peer_pb) {
  if (!FLAGS_enable_multi_raft_heartbeat_batcher) {
    return nullptr;  // Batching is disabled.
  }

  DCHECK(messenger_);
  DCHECK(raft_pool_);
  auto hostport = HostPortFromPB(remote_peer_pb.last_known_addr());
  std::lock_guard<std::mutex> lock(mutex_);
  MultiRaftHeartbeatBatcherPtr batcher;

  // After taking the lock, check if there is already a batcher
  // for the same remote host and return it.
  auto res = batchers_.find(hostport);
  if (res != batchers_.end() && (batcher = res->second.lock())) {
    return batcher;
  }
  batcher = std::make_shared<MultiRaftHeartbeatBatcher>(
      hostport, dns_resolver_, messenger_, flush_interval_, raft_pool_token_.get());
  batcher->StartTimer();
  batchers_[hostport] = batcher;
  return batcher;
}

void MultiRaftManager::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : batchers_) {
    if (auto batcher = entry.second.lock()) {
      batcher->Shutdown();
    }
  }
  batchers_.clear();
  if (raft_pool_token_) {
    raft_pool_token_->Shutdown();
    raft_pool_token_.reset();
  }
}

MultiRaftManager::~MultiRaftManager() {}

}  // namespace consensus
}  // namespace kudu

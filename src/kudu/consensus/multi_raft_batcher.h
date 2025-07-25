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

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "kudu/consensus/consensus.proxy.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"

namespace kudu {
class DnsResolver;
class MetricEntity;
class ThreadPool;
class ThreadPoolToken;

namespace rpc {
class Messenger;
class PeriodicTimer;
class RpcController;
}  // namespace rpc

namespace consensus {
class BatchedNoOpConsensusResponsePB;
class MultiRaftConsensusResponsePB;
class RaftPeerPB;

typedef std::unique_ptr<ConsensusServiceProxy> ConsensusServiceProxyPtr;

using HeartbeatResponseCallback = std::function<void(const rpc::RpcController&,
                                                     const MultiRaftConsensusResponsePB&,
                                                     const BatchedNoOpConsensusResponsePB*)>;

// - MultiRaftHeartbeatBatcher is responsible for the batching of NoOp heartbeats
//   among peers that are communicating with remote peers at the same tserver
// - A heartbeat is added to a batch upon calling AddRequestToBatch and a batch is sent
//   out every FLAGS_multi_raft_heartbeat_interval_ms ms or once the batch size reaches
//   FLAGS_multi_raft_batch_size
struct MultiRaftConsensusData;
class MultiRaftHeartbeatBatcher : public std::enable_shared_from_this<MultiRaftHeartbeatBatcher> {
 public:
  MultiRaftHeartbeatBatcher(const kudu::HostPort& hostport,
                            ::kudu::DnsResolver* dns_resolver,
                            std::shared_ptr<rpc::Messenger> messenger,
                            int flush_interval,
                            ThreadPoolToken* raft_pool_token);

  ~MultiRaftHeartbeatBatcher() = default;

  struct PeriodicHeartbeater {
    std::function<void(MultiRaftConsensusData*)> heartbeat_callback;
  };

  uint64_t Subscribe(const PeriodicHeartbeater& heartbeater);
  void Unsubscribe(uint64_t id);

 private:
  void PrepareNextBatch();

  void Shutdown();
  // This method will return a nullptr if the current batch is empty.
  std::shared_ptr<MultiRaftConsensusData> PrepareNextBatchRequestUnlocked();

  std::shared_ptr<rpc::PeriodicTimer> heartbeat_timer_;

  // If data is null then we will not send a batch request.
  void SendBatchRequest(std::shared_ptr<MultiRaftConsensusData> data);

  void MultiRaftUpdateHeartbeatResponseCallback(std::shared_ptr<MultiRaftConsensusData> data);

  struct ScheduledCallback {
    PeriodicHeartbeater heartbeater;
    int id;

    ScheduledCallback(const PeriodicHeartbeater& hb, int id_) : heartbeater(hb), id(id_) {}
  };

  void SendOutScheduled(const std::vector<ScheduledCallback>& scheduled_callbacks);

  std::shared_ptr<rpc::Messenger> messenger_;

  ConsensusServiceProxyPtr consensus_proxy_;

  uint64_t next_id = 1;
  std::unordered_map<int, PeriodicHeartbeater> peers_;
  struct Callback {
    MonoTime time;
    uint64_t id;
  };
  // Elements are put in with a fix Dealay so we do not need a priority queue.
  std::deque<Callback> queue_;
  std::mutex heartbeater_lock_;

  int flush_interval_;
  ThreadPoolToken* raft_pool_token_;
  void StartTimer();
  friend class MultiRaftManager;
  bool closed_;
};

using MultiRaftHeartbeatBatcherPtr = std::shared_ptr<MultiRaftHeartbeatBatcher>;

// MultiRaftManager is responsible for managing all MultiRaftHeartbeatBatchers
// for a given tserver (utilizes a mapping between a HostPort and the corresponding batcher).
// MultiRaftManager allows multiple peers to share the same batcher
// if they are connected to the same remote host.
class MultiRaftManager : public std::enable_shared_from_this<MultiRaftManager> {
 public:
  MultiRaftManager(kudu::DnsResolver* dns_resolver, const scoped_refptr<MetricEntity>& entity);
  ~MultiRaftManager();

  void Init(const std::shared_ptr<rpc::Messenger>& messenger, ThreadPool* raft_pool);

  void Shutdown();

  MultiRaftHeartbeatBatcherPtr AddOrGetBatcher(const kudu::consensus::RaftPeerPB& remote_peer_pb);

 private:
  std::shared_ptr<rpc::Messenger> messenger_;

  kudu::DnsResolver* dns_resolver_;

  ThreadPool* raft_pool_ = nullptr;
  std::unique_ptr<ThreadPoolToken> raft_pool_token_;

  std::mutex mutex_;

  // Uses a weak_ptr to allow deallocation of unused batchers once no more consensus
  // peer use it.
  std::unordered_map<HostPort, std::weak_ptr<MultiRaftHeartbeatBatcher>, HostPortHasher> batchers_;

  int flush_interval_;
};

}  // namespace consensus
}  // namespace kudu

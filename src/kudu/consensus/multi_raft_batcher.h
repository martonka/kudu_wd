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

#ifndef KUDU_MULTI_RAFT_BATCHER_H
#define KUDU_MULTI_RAFT_BATCHER_H

#include <memory>
#include <unordered_map>
#include "kudu/util/net/net_util.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/rpc/proxy.h"
#include "kudu/rpc/periodic.h"

namespace kudu {
namespace consensus {
class MultiRaftManager;
class ConsensusServiceProxy;
class ConsensusRequestPB;
class ConsensusResponsePB;
class BatchedNoOpConsensusResponsePB;
class MultiRaftConsensusRequestPB;
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
class MultiRaftHeartbeatBatcher: public std::enable_shared_from_this<MultiRaftHeartbeatBatcher> {
 public:
  MultiRaftHeartbeatBatcher(const kudu::HostPort& hostport,
                            ::kudu::DnsResolver* dns_resolver,
                            std::shared_ptr<rpc::Messenger> messenger);

  ~MultiRaftHeartbeatBatcher();

  // Start a periodic timer to send out batches.
  void Start();

  // Returns an id, that can be used to flush the message if it is still buffered.
  // This is needed so writes does not need to wait for the no-op hearthbeat.
  uint64_t AddRequestToBatch(ConsensusRequestPB* request,
                         HeartbeatResponseCallback callback);

  void IncrementNoOpPackageCounter();

  // Flushes the buffer if the given message is still in it
  void FlushMessage(uint64 msg_idx) {
    // No need locking for the check. If the message is already flushed, and there
    // are new messages in the buffer, than an unecessary (but fine) flush will hapen.
    if (msg_idx >= buffer_start_idx.load(std::memory_order_relaxed)) {
      PrepareAndSendBatchRequest();
    }
  }
 private:

  // Tracks all the requests, responses and callbacks in the batch.
  struct MultiRaftConsensusData;

  void PrepareAndSendBatchRequest();

  // This method will return a nullptr if the current batch is empty.
  std::shared_ptr<MultiRaftConsensusData> PrepareNextBatchRequestUnlocked();

  // If data is null then we will not send a batch request.
  void SendBatchRequest(std::shared_ptr<MultiRaftConsensusData> data);

  void MultiRaftUpdateHeartbeatResponseCallback(std::shared_ptr<MultiRaftConsensusData> data);

  std::shared_ptr<rpc::Messenger> messenger_;

  ConsensusServiceProxyPtr consensus_proxy_;

  std::shared_ptr<rpc::PeriodicTimer> batch_sender_;

  std::mutex mutex_;

  std::shared_ptr<MultiRaftConsensusData> current_batch_;

  std::atomic<uint64_t> buffer_start_idx;

};

using MultiRaftHeartbeatBatcherPtr = std::shared_ptr<MultiRaftHeartbeatBatcher>;

// MultiRaftManager is responsible for managing all MultiRaftHeartbeatBatchers
// for a given tserver (utilizes a mapping between a hostport and the corresponding batcher).
// MultiRaftManager allows multiple peers to share the same batcher
// if they are connected to the same remote host.
class MultiRaftManager: public std::enable_shared_from_this<MultiRaftManager> {
 public:
  MultiRaftManager(std::shared_ptr<rpc::Messenger> messenger,
                   kudu::DnsResolver* dns_resolver, const scoped_refptr<MetricEntity>& entity);

  MultiRaftHeartbeatBatcherPtr AddOrGetBatcher(const kudu::consensus::RaftPeerPB& remote_peer_pb);

 private:
  std::shared_ptr<rpc::Messenger> messenger_;

  kudu::DnsResolver* dns_resolver_;

  std::mutex mutex_;

  // Uses a weak_ptr to allow decation of unused batchers once no more consensus
  // peer use it.
  std::unordered_map<HostPort, std::weak_ptr<MultiRaftHeartbeatBatcher>,
                     HostPortHasher> batchers_;

};

}  // namespace consensus
} // namespace kudu
#endif  // KUDU_MULTI_RAFT_BATCHER_H

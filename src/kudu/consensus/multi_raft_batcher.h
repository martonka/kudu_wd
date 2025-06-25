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

#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "kudu/consensus/consensus.proxy.h"
#include "kudu/gutil/integral_types.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/util/net/net_util.h"

namespace kudu {
class DnsResolver;
class MetricEntity;
namespace rpc {
class Messenger;
class PeriodicTimer;
class RpcController;
}  // namespace rpc

namespace consensus {
class BatchedNoOpConsensusResponsePB;
class ConsensusRequestPB;
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
  // This is needed so writes does not need to wait for the no-op heartbeat.
  uint64_t AddRequestToBatch(ConsensusRequestPB* request,
                         HeartbeatResponseCallback callback);

  void IncrementNoOpPackageCounter();

  // Returns true if the message was found in the buffer and discarded.
  bool DiscardMessage(uint64_t msg_idx);

  // Flushes the buffer if the given message is still in it
  void FlushMessage(uint64_t msg_idx) {
    // False positive is fine here, as we will just make an unecesary flush
    if (msg_idx >= buffer_start_idx_.load(std::memory_order_relaxed)) {
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

  // Timer to flush the buffer if it was not flushed by the batch size for
  // FLAGS_multi_raft_heartbeat_interval_ms ms.
  std::shared_ptr<rpc::PeriodicTimer> batch_sender_;

  // Mutex for data in current_batch_
  std::mutex current_batch_mutex_;

  // Contains the current queue of requests (+ placeholder for responses)
  // and callbacks. When it is either full or the timer expires, it is replaced
  // by a new object and sent out (but we do not need to hold the lock while
  // sending).
  std::shared_ptr<MultiRaftConsensusData> current_batch_;

  // Each message put into the queue is assigned a strictly increasing id which
  // can be used to check if the message is still in the buffer and discard it
  // if it is not flushed yet.
  // This allows for a non-blocking way to check (id >= buffer_start_idx) that
  // has a very slim chance of false positive (in which case we will just make
  // an unnecessary flush or unnecessarily acquire the current_batch_mutex_ and
  // recheck).
  uint64_t next_idx_ = 0;
  std::atomic<uint64_t> buffer_start_idx_;

};

using MultiRaftHeartbeatBatcherPtr = std::shared_ptr<MultiRaftHeartbeatBatcher>;

// MultiRaftManager is responsible for managing all MultiRaftHeartbeatBatchers
// for a given tserver (utilizes a mapping between a hostport and the corresponding batcher).
// MultiRaftManager allows multiple peers to share the same batcher
// if they are connected to the same remote host.
class MultiRaftManager: public std::enable_shared_from_this<MultiRaftManager> {
 public:
  MultiRaftManager(kudu::DnsResolver* dns_resolver, const scoped_refptr<MetricEntity>& entity);

  void Init(const std::shared_ptr<rpc::Messenger>& messenger) {
    messenger_ = messenger;
  }

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

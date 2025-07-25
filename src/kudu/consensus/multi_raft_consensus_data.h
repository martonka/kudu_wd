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

#include <vector>

#include "kudu/consensus/consensus.pb.h"
#include "kudu/rpc/rpc_controller.h"

namespace kudu {
namespace consensus {

using HeartbeatResponseCallback = std::function<void(const rpc::RpcController&,
                                                     const MultiRaftConsensusResponsePB&,
                                                     const BatchedNoOpConsensusResponsePB*)>;

struct MultiRaftConsensusData {
  MultiRaftConsensusRequestPB batch_req;
  MultiRaftConsensusResponsePB batch_res;
  rpc::RpcController controller;
  std::vector<HeartbeatResponseCallback> response_callback_data;
  explicit MultiRaftConsensusData(int expected_size) {
    response_callback_data.reserve(expected_size);
  }
};

using PeriodicHeartbeatCall = std::function<void(MultiRaftConsensusData* current_batch)>;

struct MultiRaftHeartbeater {
  PeriodicHeartbeatCall periodic_call;
};

inline BatchedNoOpConsensusRequestPB ToNoOpRequest(const ConsensusRequestPB& req) {
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

}  // namespace consensus
}  // namespace kudu

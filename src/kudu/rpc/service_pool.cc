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

#include "kudu/rpc/service_pool.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/rpc/inbound_call.h"
#include "kudu/rpc/remote_method.h"
#include "kudu/rpc/rpc_header.pb.h"
#include "kudu/rpc/service_if.h"
#include "kudu/rpc/service_queue.h"
#include "kudu/util/logging.h"
#include "kudu/util/metrics.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/status.h"
#include "kudu/util/thread.h"
#include "kudu/util/trace.h"

using std::string;
using std::unique_ptr;
using std::vector;
using strings::Substitute;

METRIC_DEFINE_histogram(server, rpc_incoming_queue_time,
                        "RPC Queue Time",
                        kudu::MetricUnit::kMicroseconds,
                        "Number of microseconds incoming RPC requests spend in the worker queue",
                        kudu::MetricLevel::kInfo,
                        60000000LU, 3);

METRIC_DEFINE_counter(server, rpcs_timed_out_in_queue,
                      "RPC Queue Timeouts",
                      kudu::MetricUnit::kRequests,
                      "Number of RPCs whose timeout elapsed while waiting "
                      "in the service queue, and thus were not processed.",
                      kudu::MetricLevel::kWarn);

METRIC_DEFINE_counter(server, rpcs_queue_overflow,
                      "RPC Queue Overflows",
                      kudu::MetricUnit::kRequests,
                      "Number of RPCs dropped because the service queue was full.",
                      kudu::MetricLevel::kWarn);

METRIC_DEFINE_counter(server, rpcs_call_count,
                      "RPC Calls received",
                      kudu::MetricUnit::kRequests,
                      "Number of RPCs calls received.",
                      kudu::MetricLevel::kWarn);

namespace kudu {
namespace rpc {

ServicePool::ServicePool(unique_ptr<ServiceIf> service,
                         const scoped_refptr<MetricEntity>& entity,
                         size_t service_queue_length)
  : service_(std::move(service)),
    service_queue_(service_queue_length),
    incoming_queue_time_(METRIC_rpc_incoming_queue_time.Instantiate(entity)),
    rpcs_timed_out_in_queue_(METRIC_rpcs_timed_out_in_queue.Instantiate(entity)),
    rpcs_queue_overflow_(METRIC_rpcs_queue_overflow.Instantiate(entity)),
    rpcs_call_count_(METRIC_rpcs_call_count.Instantiate(entity)),
    closing_(false) {
}

ServicePool::~ServicePool() {
  Shutdown();
}

Status ServicePool::Init(int num_threads) {
  for (int i = 0; i < num_threads; i++) {
    scoped_refptr<kudu::Thread> new_thread;
    RETURN_NOT_OK(kudu::Thread::Create(
        Substitute("service pool $0", service_->service_name()),
        "rpc worker",
        [this]() { this->RunThread(); }, &new_thread));
    threads_.push_back(new_thread);
  }
  return Status::OK();
}

void ServicePool::Shutdown() {
  service_queue_.Shutdown();

  bool is_shut_down = false;
  if (!closing_.compare_exchange_strong(is_shut_down, true)) {
    return;
  }
  // TODO(mpercy): Use a proper thread pool implementation.
  for (scoped_refptr<kudu::Thread>& thread : threads_) {
    CHECK_OK(ThreadJoiner(thread.get()).Join());
  }

  // Now we must drain the service queue.
  Status status = Status::ServiceUnavailable("Service is shutting down");
  std::unique_ptr<InboundCall> incoming;
  while (service_queue_.BlockingGet(&incoming)) {
    incoming.release()->RespondFailure(ErrorStatusPB::FATAL_SERVER_SHUTTING_DOWN, status);
  }

  service_->Shutdown();
}

void ServicePool::RejectTooBusy(InboundCall* c) {
  rpcs_queue_overflow_->Increment();
  if (const auto* minfo = c->method_info(); minfo != nullptr) {
    minfo->queue_overflow_rejections->Increment();
  }
  const string err_msg = Substitute(
      "$0 request on $1 from $2 dropped due to backpressure: "
      "service queue is full with $3 items",
      c->remote_method().method_name(),
      service_->service_name(),
      c->remote_address().ToString(),
      service_queue_.max_size());
  c->RespondFailure(ErrorStatusPB::ERROR_SERVER_TOO_BUSY,
                    Status::ServiceUnavailable(err_msg));
  KLOG_EVERY_N_SECS(WARNING, 1) << err_msg << THROTTLE_MSG;
  DLOG(INFO) << err_msg << " Contents of service queue:\n"
             << service_queue_.ToString();

  if (too_busy_hook_) {
    too_busy_hook_();
  }
}

RpcMethodInfo* ServicePool::LookupMethod(const RemoteMethod& method) {
  return service_->LookupMethod(method);
}

Status ServicePool::QueueInboundCall(unique_ptr<InboundCall> call) {
  rpcs_call_count_->Increment();
  InboundCall* c = call.release();

  vector<uint32_t> unsupported_features;
  for (uint32_t feature : c->GetRequiredFeatures()) {
    if (!service_->SupportsFeature(feature)) {
      unsupported_features.push_back(feature);
    }
  }

  if (!unsupported_features.empty()) {
    c->RespondUnsupportedFeature(unsupported_features);
    return Status::NotSupported("call requires unsupported application feature flags",
                                JoinMapped(unsupported_features,
                                           [] (uint32_t flag) { return std::to_string(flag); },
                                           ", "));
  }

  TRACE_TO(c->trace(), "Inserting onto call queue");

  // Queue message on service queue
  std::optional<InboundCall*> evicted;
  const auto queue_status = service_queue_.Put(c, &evicted);
  if (queue_status == QUEUE_FULL) {
    RejectTooBusy(c);
    return Status::OK();
  }

  if (PREDICT_FALSE(evicted.has_value())) {
    RejectTooBusy(*evicted);
  }

  if (PREDICT_TRUE(queue_status == QUEUE_SUCCESS)) {
    // NB: do not do anything with 'c' after it is successfully queued --
    // a service thread may have already dequeued it, processed it, and
    // responded by this point, in which case the pointer would be invalid.
    return Status::OK();
  }

  Status status = Status::OK();
  if (queue_status == QUEUE_SHUTDOWN) {
    status = Status::ServiceUnavailable("Service is shutting down");
    c->RespondFailure(ErrorStatusPB::FATAL_SERVER_SHUTTING_DOWN, status);
  } else {
    status = Status::RuntimeError(Substitute("Unknown error from BlockingQueue: $0", queue_status));
    c->RespondFailure(ErrorStatusPB::FATAL_UNKNOWN, status);
  }
  return status;
}

void ServicePool::RunThread() {
  while (true) {
    std::unique_ptr<InboundCall> incoming;
    if (!service_queue_.BlockingGet(&incoming)) {
      VLOG(1) << "ServicePool: messenger shutting down.";
      return;
    }

    incoming->RecordHandlingStarted(incoming_queue_time_.get());
    ADOPT_TRACE(incoming->trace());

    if (PREDICT_FALSE(incoming->ClientTimedOut())) {
      TRACE_TO(incoming->trace(), "Skipping call since client already timed out");
      rpcs_timed_out_in_queue_->Increment();

      // Respond as a failure, even though the client will probably ignore
      // the response anyway. Must release the raw pointer since the
      // RespondFailure() call below ends up taking ownership of the object.
      incoming.release()->RespondFailure(
        ErrorStatusPB::ERROR_SERVER_TOO_BUSY,
        Status::TimedOut("Call waited in the queue past client deadline"));

      continue;
    }

    TRACE_TO(incoming->trace(), "Handling call");

    // Release the InboundCall pointer -- when the call is responded to,
    // it will get deleted at that point.
    service_->Handle(incoming.release());
  }
}

const string& ServicePool::service_name() const {
  return service_->service_name();
}

} // namespace rpc
} // namespace kudu

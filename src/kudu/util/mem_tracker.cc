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

#include "kudu/util/mem_tracker.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <ostream>
#include <stack>
#include <type_traits>

#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/mem_tracker.pb.h"
#include "kudu/util/mutex.h"
#include "kudu/util/process_memory.h"

using std::deque;
using std::stack;
using std::list;
using std::shared_ptr;
using std::string;
using std::vector;
using std::weak_ptr;
using strings::Substitute;

namespace kudu {

// NOTE: this class has been adapted from Impala, so the code style varies
// somewhat from kudu.

shared_ptr<MemTracker> MemTracker::CreateTracker(int64_t byte_limit,
                                                 const string& id,
                                                 shared_ptr<MemTracker> parent) {
  shared_ptr<MemTracker> real_parent = parent ? std::move(parent) : GetRootTracker();
  shared_ptr<MemTracker> tracker(MemTracker::make_shared(byte_limit, id, real_parent));
  real_parent->AddChildTracker(tracker);
  tracker->Init();

  return tracker;
}

MemTracker::MemTracker(int64_t byte_limit, const string& id, shared_ptr<MemTracker> parent)
    : limit_(byte_limit),
      id_(id),
      descr_(Substitute("memory consumption for $0", id)),
      parent_(std::move(parent)),
      consumption_(0) {
  VLOG(1) << "Creating tracker " << ToString();
}

MemTracker::~MemTracker() {
  VLOG(1) << "Destroying tracker " << ToString();
  if (parent_) {
    DCHECK(consumption() == 0) << "Memory tracker " << ToString()
        << " has unreleased consumption " << consumption();
    parent_->Release(consumption());

    std::lock_guard l(parent_->child_trackers_lock_);
    if (child_tracker_it_ != parent_->child_trackers_.end()) {
      parent_->child_trackers_.erase(child_tracker_it_);
      child_tracker_it_ = parent_->child_trackers_.end();
    }
  }
}

string MemTracker::ToString() const {
  string s;
  const MemTracker* tracker = this;
  while (tracker) {
    if (s != "") {
      s += "->";
    }
    s += tracker->id();
    tracker = tracker->parent_.get();
  }
  return s;
}

bool MemTracker::FindTracker(const string& id,
                             shared_ptr<MemTracker>* tracker,
                             const shared_ptr<MemTracker>& parent) {
  return FindTrackerInternal(id, tracker, parent ? parent : GetRootTracker());
}

bool MemTracker::FindTrackerInternal(const string& id,
                                     shared_ptr<MemTracker>* tracker,
                                     const shared_ptr<MemTracker>& parent) {
  DCHECK(parent);

  list<weak_ptr<MemTracker>> children;
  {
    std::lock_guard l(parent->child_trackers_lock_);
    children = parent->child_trackers_;
  }

  // Search for the matching child without holding the parent's lock.
  //
  // If the lock were held while searching, it'd be possible for 'child' to be
  // the last live ref to a tracker, which would lead to a recursive
  // acquisition of the parent lock during the 'child' destructor call.
  vector<shared_ptr<MemTracker>> found;
  for (const auto& child_weak : children) {
    shared_ptr<MemTracker> child = child_weak.lock();
    if (child && child->id() == id) {
      found.emplace_back(std::move(child));
    }
  }
  if (PREDICT_TRUE(found.size() == 1)) {
    *tracker = found[0];
    return true;
  } else if (found.size() > 1) {
    LOG(DFATAL) <<
        Substitute("Multiple memtrackers with same id ($0) found on parent $1",
                   id, parent->ToString());
    *tracker = found[0];
    return true;
  }
  return false;
}

shared_ptr<MemTracker> MemTracker::FindOrCreateGlobalTracker(
    int64_t byte_limit,
    const string& id) {
  // The calls below comprise a critical section, but we can't use the root
  // tracker's child_trackers_lock_ to synchronize it as the lock must be
  // released during FindTrackerInternal(). Since this function creates
  // globally-visible MemTrackers which are the exception rather than the rule,
  // it's reasonable to synchronize their creation on a singleton lock.
  static Mutex find_or_create_lock;
  std::lock_guard l(find_or_create_lock);

  shared_ptr<MemTracker> found;
  if (FindTrackerInternal(id, &found, GetRootTracker())) {
    return found;
  }
  return CreateTracker(byte_limit, id, GetRootTracker());
}

void MemTracker::ListTrackers(vector<shared_ptr<MemTracker>>* trackers) {
  trackers->clear();
  deque<shared_ptr<MemTracker>> to_process;
  to_process.push_front(GetRootTracker());
  while (!to_process.empty()) {
    shared_ptr<MemTracker> t = to_process.back();
    to_process.pop_back();

    trackers->push_back(t);
    {
      std::lock_guard l(t->child_trackers_lock_);
      for (const auto& child_weak : t->child_trackers_) {
        shared_ptr<MemTracker> child = child_weak.lock();
        if (child) {
          to_process.emplace_back(std::move(child));
        }
      }
    }
  }
}

void MemTracker::TrackersToPb(MemTrackerPB* pb) {
  DCHECK(pb);
  stack<std::pair<shared_ptr<MemTracker>, MemTrackerPB*>> to_process;
  to_process.emplace(GetRootTracker(), pb);
  while (!to_process.empty()) {
    auto tracker_and_pb = std::move(to_process.top());
    to_process.pop();
    auto& tracker = tracker_and_pb.first;
    auto* tracker_pb = tracker_and_pb.second;
    tracker_pb->set_id(tracker->id());
    if (tracker->parent()) {
      tracker_pb->set_parent_id(tracker->parent()->id());
    }
    tracker_pb->set_limit(tracker->limit());
    tracker_pb->set_current_consumption(tracker->consumption());
    tracker_pb->set_peak_consumption(tracker->peak_consumption());
    {
      std::lock_guard l(tracker->child_trackers_lock_);
      for (const auto& child_weak : tracker->child_trackers_) {
        shared_ptr<MemTracker> child = child_weak.lock();
        if (child) {
          auto* child_pb = tracker_pb->add_child_trackers();
          to_process.emplace(std::move(child), child_pb);
        }
      }
    }
  }
}

shared_ptr<MemTracker> MemTracker::GetRootTracker() {
  // The ancestor for all trackers. Every tracker is visible from the root down.
  static shared_ptr<MemTracker> root_tracker;
  static std::once_flag once;
  std::call_once(once, [&] {
    root_tracker = MemTracker::make_shared(-1, "root", std::shared_ptr<MemTracker>());
    root_tracker->Init();
  });
  return root_tracker;
}

void MemTracker::Consume(int64_t bytes) {
  if (bytes < 0) {
    Release(-bytes);
    return;
  }

  if (bytes == 0) {
    return;
  }
  for (auto& tracker : all_trackers_) {
    tracker->consumption_.IncrementBy(bytes);
  }
}

bool MemTracker::TryConsume(int64_t bytes) {
  if (bytes <= 0) {
    Release(-bytes);
    return true;
  }

  int i = 0;
  // Walk the tracker tree top-down, consuming memory from each in turn.
  for (i = all_trackers_.size() - 1; i >= 0; --i) {
    MemTracker *tracker = all_trackers_[i];
    if (tracker->limit_ < 0) {
      tracker->consumption_.IncrementBy(bytes);
    } else {
      if (!tracker->consumption_.TryIncrementBy(bytes, tracker->limit_)) {
        break;
      }
    }
  }
  // Everyone succeeded, return.
  if (i == -1) {
    return true;
  }

  // Someone failed, roll back the ones that succeeded.
  // TODO(todd): this doesn't roll it back completely since the max values for
  // the updated trackers aren't decremented. The max values are only used
  // for error reporting so this is probably okay. Rolling those back is
  // pretty hard; we'd need something like 2PC.
  for (int j = all_trackers_.size() - 1; j > i; --j) {
    all_trackers_[j]->consumption_.IncrementBy(-bytes);
  }
  return false;
}

bool MemTracker::CanConsumeNoAncestors(int64_t bytes) {
  if (limit_ < 0) {
    return true;
  }
  return consumption_.CanIncrementBy(bytes, limit_);
}

void MemTracker::Release(int64_t bytes) {
  if (bytes < 0) {
    Consume(-bytes);
    return;
  }

  if (bytes == 0) {
    return;
  }

  for (auto& tracker : all_trackers_) {
    tracker->consumption_.IncrementBy(-bytes);
  }
  process_memory::MaybeGCAfterRelease(bytes);
}

bool MemTracker::AnyLimitExceeded() const {
  return std::any_of(limit_trackers_.cbegin(),
                     limit_trackers_.cend(),
                     [](const auto& tracker) {
                       return tracker->LimitExceeded();
                     });
}

int64_t MemTracker::SpareCapacity() const {
  int64_t result = std::numeric_limits<int64_t>::max();
  for (const auto& tracker : limit_trackers_) {
    int64_t mem_left = tracker->limit() - tracker->consumption();
    result = std::min(result, mem_left);
  }
  return result;
}


void MemTracker::Init() {
  // populate all_trackers_ and limit_trackers_
  MemTracker* tracker = this;
  while (tracker) {
    all_trackers_.push_back(tracker);
    if (tracker->has_limit()) {
      limit_trackers_.push_back(tracker);
    }
    tracker = tracker->parent_.get();
  }
  DCHECK_GT(all_trackers_.size(), 0);
  DCHECK_EQ(all_trackers_[0], this);
}

void MemTracker::AddChildTracker(const shared_ptr<MemTracker>& tracker) {
  std::lock_guard l(child_trackers_lock_);
  tracker->child_tracker_it_ = child_trackers_.insert(child_trackers_.end(), tracker);
}

} // namespace kudu

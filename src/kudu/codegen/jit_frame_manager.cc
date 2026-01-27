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

#include "kudu/codegen/jit_frame_manager.h"

#include <cstdint>
#include <iterator>
#include <iostream>
#include <tuple>

#include <llvm/ExecutionEngine/SectionMemoryManager.h>


// External symbols from libgcc/libunwind.
extern "C" void __register_frame(void*);  // NOLINT(bugprone-reserved-identifier)
extern "C" void __deregister_frame(void*);// NOLINT(bugprone-reserved-identifier)
extern "C" void __register_frame_log(void *, void (*)(int, const void*, const void*));
extern "C" void __deregister_frame_log (void *, void (*)(int, const void*, const void*));

using llvm::SectionMemoryManager;
using llvm::StringRef;
using std::lock_guard;

namespace kudu {
namespace codegen {

// Initialize the static mutex
std::mutex JITFrameManager::kRegistrationMutex;

JITFrameManager::~JITFrameManager() {
  // Be explicit about avoiding the virtual dispatch: invoke
  // deregisterEHFramesImpl() instead of deregisterEHFrames().
  deregisterEHFramesImpl();
}

uint8_t* JITFrameManager::allocateCodeSection(uintptr_t size,
                                              unsigned alignment,
                                              unsigned section_id,
                                              StringRef section_name) {
    // Add extra padding for EH frame section: it's zeroed out later upon
    // registerEHFrames() calls.
    if (section_name == ".eh_frame") {
      size += 4;
    }
    return SectionMemoryManager::allocateCodeSection(
        size, alignment, section_id, section_name);
  }

class Checker {
  std::map<uint8_t*, uint8_t*> by_start;
  std::map<uint8_t*, uint8_t*> by_end;

public:
  void add(uint8_t* start, uint8_t* end) {
    if (end <= start) {
      return;
    }
    if (by_start.find(start) != by_start.end()) {
      std::cerr << "Error: Attempting to register already registered EH frames at "
                << static_cast<void*>(start) << std::endl;
    }
    if (by_end.find(end) != by_end.end()) {
      std::cerr << "Error: Attempting to register EH frames with same ending "
                << static_cast<void*>(end) << std::endl;
    }

    // Check for overlapping intervals.
    auto it = by_end.lower_bound(start + 1);
    if(it != by_end.end() && it->second < end && start < it->first) {
      std::cerr << "Error: Attempting to register overlapping EH frames at "
                << static_cast<void*>(start) << " <-> " << static_cast<void*>(end) 
                << " vs. " << static_cast<void*>(it->second) << " <-> " 
                << static_cast<void*>(it->first) << std::endl;
    }
    by_start[start] = end;
    by_end[end] = start;
  }

  void remove( uint8_t* start, uint8_t* end) {
    if (end <= start) {
      return;
    }

    if (by_start.count(start) == 0) {
      std::cerr << "Error: Attempting to deregister unregistered EH frames at "
                << static_cast<void*>(start) << std::endl;
    }

    if (by_end.count(end) == 0) {
      std::cerr << "Error: Attempting to deregister unregistered EH frames at "
                << static_cast<void*>(end) << std::endl;
    }
    by_start.erase(start);
    by_end.erase(end);
  }

};

static Checker frame_checker;
static Checker range_checker;

void log_frame_event(int action, const void* begin, const void* end) {
  uint8_t* b = (uint8_t*)begin;
  uint8_t* e = (uint8_t*)end;
  std::cerr << "JITFrameManager: log_frame_event action=" << action
            << " begin=" << static_cast<void*>(b)
            << " end=" << static_cast<void*>(e) << std::endl;
  if (action == 1) {
    frame_checker.add(b, e);
  } else if (action == 3) {
    frame_checker.remove(b, e);
  } else if (action == 2) {
    range_checker.add(b, e);
  } else if (action == 4) {
    range_checker.remove(b, e);
  } else {
    std::cerr << "Error: Unknown frame event action " << action << std::endl;
    exit(1);
  }
}


extern void get_pc_range_unhidden(const struct object *ob, __UINTPTR_TYPE__ *range);

void JITFrameManager::registerEHFrames(uint8_t* addr,
                                       uint64_t /*load_addr*/,
                                       size_t size) {
  lock_guard guard(kRegistrationMutex);
   
  // libgcc expects a null-terminated list of FDEs: write 4 zero bytes in the
  // end of the allocated section.
  auto* terminator = reinterpret_cast<uint32_t*>(addr + size);
  *terminator = 0;

  __register_frame_log(addr, log_frame_event);
  registered_frames_.push_back(addr);
}

void JITFrameManager::deregisterEHFrames() {
  return deregisterEHFramesImpl();
}

void JITFrameManager::deregisterEHFramesImpl() {
  lock_guard guard(kRegistrationMutex);
  for (auto it = registered_frames_.rbegin(); it != registered_frames_.rend(); ++it) {
    __deregister_frame_log (*it, log_frame_event);
  }
  registered_frames_.clear();
}

} // namespace codegen
} // namespace kudu

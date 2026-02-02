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
#include <optional>

#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <gflags/gflags.h>
#include <kudu/util/logging.h>
#include <kudu/util/flag_tags.h>

DEFINE_bool(codegen_split_eh_frames,
            true,
            "Split EH frames into separate allocations to avoid overlap issues.");
TAG_FLAG(codegen_split_eh_frames, runtime);

// External symbols from libgcc/libunwind.
extern "C" void __register_frame(void*);    // NOLINT(bugprone-reserved-identifier)
extern "C" void __deregister_frame(void*);  // NOLINT(bugprone-reserved-identifier)

using llvm::SectionMemoryManager;
using llvm::StringRef;
using std::lock_guard;

namespace kudu {
namespace codegen {

// Initialize the static mutex
std::mutex JITFrameManager::kRegistrationMutex;

namespace {

uint64_t read_uleb(uint8_t** addr) {
  uint32_t shift = 0;
  uint8_t byte;
  uint64_t result;
  result = 0;
  do {
    byte = **addr;
    (*addr)++;
    result |= (byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  return result;
}

int64_t read_sleb(uint8_t** addr) {
  uint32_t shift = 0;
  uint8_t byte;
  int64_t result;
  result = 0;
  do {
    byte = **addr;
    (*addr)++;
    result |= (byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);

  if (shift < 8 * sizeof(result) && (byte & 0x40) != 0) result |= -(((uint64_t)1L) << shift);
  return result;
}

enum PointerEncoding : int {
  abs = 0x00,
  uleb128 = 0x01,
  fixed2 = 0x02,
  fixed4 = 0x03,
  fixed8 = 0x04,
  sleb128 = 0x09,
  sdata2 = 0x0A,
  sdata4 = 0x0B,
  sdata8 = 0x0C,
  relative = 0x10
};

class FDECopy {
 public:
  static std::unique_ptr<uint8_t[]> create_copy(uint8_t* src_cie_start, uint8_t* src_fde_start) {
    return FDECopy(src_cie_start, src_fde_start).do_copy();
  }

 private:
  FDECopy(uint8_t* src_cie_start, uint8_t* src_fde_start)
      : src_cie_start_(src_cie_start), src_fde_start_(src_fde_start) {};
  // Used:
  // https://refspecs.linuxfoundation.org/LSB_3.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
  // Relative pointers needs to be fixed up to be relative to the new location.
  // LLVM seems to always use 0x1c (relative 64 bit) for everything
  // but we convert others just in case (and convert then to 0x1c).
  std::unique_ptr<uint8_t[]> do_copy() {
    std::unique_ptr<uint8_t[]> dest_array;

    uint32_t src_cie_length = *reinterpret_cast<uint32_t*>(src_cie_start_);
    uint32_t src_fde_length = *reinterpret_cast<uint32_t*>(src_fde_start_);

    // personality pointer, lsda pointer, personality pointer might extend. + 2x padding
    // that is 5x7 bytes extra worst case.
    dest_array = std::make_unique<uint8_t[]>(src_cie_length + src_fde_length + 5 * 7);
    read_pos = src_cie_start_;
    write_pos = dest_array.get();
    copy_bytes(8);  // length, will be fixed later, cie remains 0 offset.
    auto cie_version = read_type<uint8_t>();
    if (cie_version != 1) {
      KLOG_EVERY_N_SECS(ERROR, 5) << "Unsupported CIE version " << static_cast<int>(cie_version)
                                  << " in EH frames";
      return nullptr;
    }
    write_type<uint8_t>(cie_version);
    auto aug_string = std::string(reinterpret_cast<char*>(read_pos));
    copy_bytes(aug_string.size() + 1);  // aug string + null terminator
    auto read_pos_new = read_pos;
    read_uleb(&read_pos_new);             // Code Alignment Factor
    read_sleb(&read_pos_new);             // Data Alignment Factor
    read_uleb(&read_pos_new);             // Return Address Register
    read_uleb(&read_pos_new);             // Augmentation Length
    copy_bytes(read_pos_new - read_pos);  // up to aug length
    auto aug_length_pos = write_pos - 1;      // always 1 byte for supported versions
    if (aug_string[0] != 'z') {
      KLOG_EVERY_N_SECS(ERROR, 5) << "Unsupported CIE augmentation in EH frames: " << aug_string;
      return nullptr;
    }
    uint8_t encoding = 0;
    std::optional<uint8_t> lsda_encoding = std::nullopt;
    for (auto c = aug_string.begin() + 1; c != aug_string.end(); ++c) {
      if (*c == 'R') {
        encoding = read_type<uint8_t>();
        write_type<uint8_t>(0x1c);  // set to relative 64 bit
      } else if (*c == 'P') {
        uint8_t penc = read_type<uint8_t>();
        write_type<uint8_t>(0x1c);  // set to relative 64 bit
        auto field_address = reinterpret_cast<int64_t>(write_pos);
        auto personality_ptr = read_pointer(penc);
        if (!personality_ptr.has_value()) {
          return nullptr;
        }
        write_type<int64_t>(personality_ptr.value() - field_address);
      } else if (*c == 'L') {
        lsda_encoding = read_type<uint8_t>();
        write_type<uint8_t>(0x1c);  // set to relative 64 bit
      } else {
        // Unknown augmentation
        KLOG_EVERY_N_SECS(ERROR, 5) << "Unsupported CIE augmentation in EH frames: " << aug_string;
        return nullptr;
      }
    }

    auto new_aug_length = write_pos - (aug_length_pos + 1);
    *aug_length_pos = static_cast<uint8_t>(new_aug_length);

    copy_bytes(src_cie_length + src_cie_start_ - read_pos);  // rest of CIE
    while (reinterpret_cast<uintptr_t>(write_pos) % 8 != 0) {
      write_type<uint8_t>(0);  // padding
    }

    auto cie_new_length = write_pos - dest_array.get() - 4;
    *reinterpret_cast<uint32_t*>(dest_array.get()) = cie_new_length;

    // cie is fine, now fde

    auto fde_new_start_pos = write_pos;
    read_pos = src_fde_start_;
    copy_bytes(4);          // length, will be fixed later
    read_type<uint32_t>();  // CIE Offset
    auto cie_offset = 8 + cie_new_length;
    write_type<uint32_t>(cie_offset);  // CIE Offset

    auto pc_field = read_pointer(encoding);
    if (!pc_field.has_value()) {
      return nullptr;
    }
    auto pc_field_address = reinterpret_cast<int64_t>(write_pos);
    write_type<int64_t>(pc_field.value() - pc_field_address);  // PC Begin

    // Copy range. It is always an absolute value.
    auto range = read_pointer(encoding & 0x0f);
    if (!range.has_value()) {
      return nullptr;
    }
    write_type<int64_t>(range.value());  // PC Range

    auto fde_aug_length = read_uleb(&read_pos);  // Augmentation Length
    auto fde_aug_len_read_pos = read_pos;
    if (lsda_encoding.has_value()) {
      auto field_address = reinterpret_cast<int64_t>(write_pos);
      auto lsda_ptr = read_pointer(lsda_encoding.value());
      if (!lsda_ptr.has_value()) {
        return nullptr;
      }
      write_type<int8_t>(sizeof(int64_t)); // only member of augmentation.
      write_type<int64_t>(lsda_ptr.value() - field_address);
    } else {
      write_type<uint8_t>(0);  // no LSDA
    }
    if (read_pos - fde_aug_len_read_pos != fde_aug_length) {
      KLOG_EVERY_N_SECS(ERROR, 5) << "FDE augmentation length mismatch in EH frames";
      return nullptr;
    }

    copy_bytes(src_fde_length + src_fde_start_ - read_pos);  // rest of FDE
    while (reinterpret_cast<uintptr_t>(write_pos) % 8 != 0) {
      write_type<uint8_t>(0);  // padding
    }

    auto fde_new_length = write_pos - fde_new_start_pos - 4;
    *reinterpret_cast<uint32_t*>(fde_new_start_pos) = fde_new_length;

    return dest_array;
  };

  std::optional<int64_t> read_pointer(uint8_t encoding) {
    int64_t shift = 0;
    if (encoding > 0x20) {
      KLOG_EVERY_N_SECS(ERROR, 5) << "Unsupported pointer encoding in EH frames: "
                                  << static_cast<int>(encoding);
      return std::nullopt;
    }
    if (encoding & PointerEncoding::relative) {
      shift = reinterpret_cast<int64_t>(read_pos);
    }
    switch (encoding & 0x0f) {
      case PointerEncoding::abs:  // absolute pointer
        return (uint64_t)read_type<void*>();
      case PointerEncoding::uleb128:  // uleb128
        return shift + read_uleb(&read_pos);
      case PointerEncoding::sleb128:  // sleb128
        return shift + read_sleb(&read_pos);
      case PointerEncoding::fixed2:  // fixed 2 bytes
        return shift + read_type<uint16_t>();
      case PointerEncoding::fixed4:  // fixed 4 bytes
        return shift + read_type<uint32_t>();
      case PointerEncoding::fixed8:  // fixed 8 bytes
        return shift + read_type<uint64_t>();
      case PointerEncoding::sdata2:  // signed 2 bytes
        return shift + read_type<int16_t>();
      case PointerEncoding::sdata4:  // signed 4 bytes
        return shift + read_type<int32_t>();
      case PointerEncoding::sdata8:  // signed 8 bytes
        return shift + read_type<int64_t>();
      default:
        KLOG_EVERY_N_SECS(ERROR, 5)
            << "Unsupported pointer encoding in EH frames: " << static_cast<int>(encoding);
        return std::nullopt;
    }
  }

  void copy_bytes(std::size_t length) {
    std::memcpy(write_pos, read_pos, length);
    read_pos += length;
    write_pos += length;
  }

  template <typename T>
  T read_type() {
    T val = *reinterpret_cast<T*>(read_pos);
    read_pos += sizeof(T);
    return val;
  }

  template <typename T>
  void write_type(const T& val) {
    *reinterpret_cast<T*>(write_pos) = val;
    write_pos += sizeof(T);
  }

  uint8_t* src_cie_start_;
  uint8_t* src_fde_start_;

  uint8_t* read_pos;
  uint8_t* write_pos;
};

// Its better not to register anything that overlaps with existing registered areas.
// In one case we will fail to unwind a crash, in another we may crash Kudu.
// So if we detect am unexpected encoding/aug_string, we just skip the FDE,
// and log an error.
std::vector<std::unique_ptr<std::uint8_t[]> > create_single_fdes(uint8_t* orig_address) {
  auto split_fdes = std::vector<std::unique_ptr<std::uint8_t[]> >();

  uint8_t* addr_iter = orig_address;

  for (uint32_t current_length = 0;; addr_iter += current_length) {
    current_length = reinterpret_cast<uint32_t*>(addr_iter)[0];
    addr_iter += 4;

    if (current_length == 0) {
      break;  // terminator
    }

    uint32_t cie_offset = reinterpret_cast<uint32_t*>(addr_iter)[0];

    if (cie_offset == 0) {  // CIE will be checked on copy.
      continue;
    }

    uint8_t* cie_to_use = addr_iter - cie_offset;
    uint8_t* fde_start = addr_iter - 4;  // include length

    auto res = FDECopy::create_copy(cie_to_use, fde_start);
    if (res == nullptr) {
      KLOG_EVERY_N_SECS(ERROR, 5) << "Failed to copy FDE, stopping EH frame splitting.";
      continue;
    }
    split_fdes.emplace_back(std::move(res));
  }
  return split_fdes;
}

}  // namespace

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
  return SectionMemoryManager::allocateCodeSection(size, alignment, section_id, section_name);
}

void JITFrameManager::registerEHFrames(uint8_t* addr, uint64_t /*load_addr*/, size_t size) {
  lock_guard guard(kRegistrationMutex);
  auto* terminator = reinterpret_cast<uint32_t*>(addr + size);
  *terminator = 0;

  if (FLAGS_codegen_split_eh_frames == false) {
    __register_frame(addr);
    registered_frames_.push_back(addr);
  } else {
    auto table = create_single_fdes(addr);
    registered_frame_array_.emplace_back(std::move(table));
    for (const auto& rec : registered_frame_array_.back()) {
      __register_frame((void*)rec.get());
    }
  }

}

void JITFrameManager::deregisterEHFrames() { return deregisterEHFramesImpl(); }

void JITFrameManager::deregisterEHFramesImpl() {
  lock_guard guard(kRegistrationMutex);

  // registered_frames_ or registered_frame_array_ is empty depending on
  // FLAGS_codegen_split_eh_frames, unless it was set differently during
  // the current compilation, so both can contain data.
  for (auto it = registered_frames_.rbegin(); it != registered_frames_.rend(); ++it) {
    __deregister_frame(*it);
  }
  registered_frames_.clear();

  for (auto it = registered_frame_array_.rbegin(); it != registered_frame_array_.rend(); ++it) {
    for (auto it2 = it->rbegin(); it2 != it->rend(); ++it2) {
      __deregister_frame((void*)it2->get());
    }
  }
  registered_frame_array_.clear();
}

}  // namespace codegen
}  // namespace kudu

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_file.h"

#include <atomic>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/vfs/devices/disc_image_entry.h"
DEFINE_string(trace_disc_reads, "",
              "Log every read of disc files whose name contains this text.",
              "Logging");

namespace xe {
namespace vfs {

static std::atomic<DiscReadObserver> disc_read_observer{nullptr};

void SetDiscReadObserver(DiscReadObserver observer) {
  disc_read_observer.store(observer, std::memory_order_relaxed);
}

DiscImageFile::DiscImageFile(uint32_t file_access, DiscImageEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscImageFile::~DiscImageFile() = default;

void DiscImageFile::Destroy() { delete this; }

X_STATUS DiscImageFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }

  if (entry_->data_offset() >= entry_->mmap()->size()) {
    xe::FatalError("This ISO image is corrupted and cannot be played.");
    return X_STATUS_END_OF_FILE;
  }

  size_t real_offset = entry_->data_offset() + byte_offset;
  size_t real_length =
      std::min(buffer.size(), entry_->data_size() - byte_offset);
  std::memcpy(buffer.data(), entry_->mmap()->data() + real_offset, real_length);
  *out_bytes_read = real_length;
  if (DiscReadObserver observer =
          disc_read_observer.load(std::memory_order_relaxed)) {
    observer(entry_->name(), byte_offset, real_length);
  }
  if (!cvars::trace_disc_reads.empty() &&
      entry_->name().find(cvars::trace_disc_reads) != std::string::npos) {
    XELOGI("DiscRead {} off={:08X} len={:X} got={:X} tid={} uptime={}",
           entry_->name(), byte_offset, buffer.size(), real_length,
           threading::current_thread_id(), Clock::QueryGuestUptimeMillis());
  }
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe

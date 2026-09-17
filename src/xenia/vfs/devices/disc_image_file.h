/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_IMAGE_FILE_H_
#define XENIA_VFS_DEVICES_DISC_IMAGE_FILE_H_

#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

// Called on every read of a disc image file with the file's name, the offset
// in the file and the bytes read. Set by the emulator; null for none.
using DiscReadObserver = void (*)(const std::string& name, uint64_t offset,
                                  uint64_t length);
void SetDiscReadObserver(DiscReadObserver observer);

class DiscImageEntry;

class DiscImageFile : public File {
 public:
  DiscImageFile(uint32_t file_access, DiscImageEntry* entry);
  ~DiscImageFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override {
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t length) override { return X_STATUS_ACCESS_DENIED; }

 private:
  DiscImageEntry* entry_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_IMAGE_FILE_H_

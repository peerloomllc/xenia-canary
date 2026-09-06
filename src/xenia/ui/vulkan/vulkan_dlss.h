/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_VULKAN_DLSS_H_
#define XENIA_UI_VULKAN_VULKAN_DLSS_H_

#include <cstdint>
#include <memory>

#include "xenia/ui/vulkan/vulkan_device.h"

namespace xe {
namespace ui {
namespace vulkan {

// NVIDIA DLAA through NGX, used by the presenter to anti-alias the guest
// output at its own resolution. The emulator has no motion vectors or jitter
// for the guest frame, so the evaluation feeds zero motion vectors and a
// flat depth buffer; that rules out the detail-reconstructing super
// resolution modes (measured to add nothing over bilinear here), while
// anti-aliasing an already-rendered frame still works.
class VulkanDlss {
 public:
  // Returns nullptr when the SDK is not compiled in, the device lacks the NGX
  // extensions, NGX fails to initialize (non-NVIDIA GPU or old driver) or
  // DLSS is not available.
  static std::unique_ptr<VulkanDlss> TryCreate(const VulkanDevice* device);

  ~VulkanDlss();

  // (Re)creates the DLSS feature and the dummy depth and motion vector images
  // if the sizes changed, recording initialization commands into the command
  // buffer (must be outside a render pass). The caller must have awaited
  // completion of any submissions still using the previous sizes (the
  // presenter already does this when intermediate images are resized).
  // Returns false on failure; the caller must fall back to another effect.
  bool EnsureFeature(VkCommandBuffer command_buffer, uint32_t input_width,
                     uint32_t input_height, uint32_t output_width,
                     uint32_t output_height);

  // Records a DLSS evaluation into the command buffer (outside a render
  // pass). The input image must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
  // and the output image in VK_IMAGE_LAYOUT_GENERAL. `reset` requests a
  // history reset (new guest output version, size change).
  bool Evaluate(VkCommandBuffer command_buffer, VkImage input_image,
                VkImageView input_view, VkImage output_image,
                VkImageView output_view, VkFormat format, bool reset);

  uint32_t input_width() const { return input_width_; }
  uint32_t input_height() const { return input_height_; }
  uint32_t output_width() const { return output_width_; }
  uint32_t output_height() const { return output_height_; }

 private:
  explicit VulkanDlss(const VulkanDevice* device) : device_(device) {}

  struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool CreateOwnedImage(VkFormat format, uint32_t width, uint32_t height,
                        OwnedImage& out);
  void DestroyOwnedImage(OwnedImage& image);
  void ReleaseFeature();

  const VulkanDevice* device_;

  // NVSDK_NGX_Parameter*, kept as void* so the header needs no SDK includes.
  void* ngx_parameters_ = nullptr;
  // NVSDK_NGX_Handle*.
  void* ngx_feature_ = nullptr;
  bool ngx_initialized_ = false;

  OwnedImage depth_;
  OwnedImage motion_vectors_;
  bool dummies_initialized_ = false;

  uint32_t input_width_ = 0;
  uint32_t input_height_ = 0;
  uint32_t output_width_ = 0;
  uint32_t output_height_ = 0;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_DLSS_H_

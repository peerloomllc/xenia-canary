/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_dlss.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_util.h"

#if XE_VULKAN_HAS_DLSS
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"
#endif  // XE_VULKAN_HAS_DLSS

namespace xe {
namespace ui {
namespace vulkan {

#if !XE_VULKAN_HAS_DLSS

std::unique_ptr<VulkanDlss> VulkanDlss::TryCreate(const VulkanDevice* device) {
  return nullptr;
}
VulkanDlss::~VulkanDlss() = default;
bool VulkanDlss::EnsureFeature(VkCommandBuffer command_buffer,
                               uint32_t input_width, uint32_t input_height,
                               uint32_t output_width, uint32_t output_height) {
  return false;
}
bool VulkanDlss::Evaluate(VkCommandBuffer command_buffer, VkImage input_image,
                          VkImageView input_view, VkImage output_image,
                          VkImageView output_view, VkFormat format,
                          bool reset) {
  return false;
}
bool VulkanDlss::CreateOwnedImage(VkFormat format, uint32_t width,
                                  uint32_t height, OwnedImage& out) {
  return false;
}
void VulkanDlss::DestroyOwnedImage(OwnedImage& image) {}
void VulkanDlss::ReleaseFeature() {}

#else  // XE_VULKAN_HAS_DLSS

namespace {
// Arbitrary but stable identifier passed to NGX for this application.
constexpr unsigned long long kNgxApplicationId = 0x0058454E4941ull;  // "XENIA"
}  // namespace

std::unique_ptr<VulkanDlss> VulkanDlss::TryCreate(const VulkanDevice* device) {
  const VulkanDevice::Extensions& extensions = device->extensions();
  if (!extensions.ext_NVX_binary_import ||
      !extensions.ext_NVX_image_view_handle ||
      !extensions.ext_KHR_push_descriptor ||
      !device->properties().bufferDeviceAddress) {
    XELOGI("VulkanDlss: NGX device extensions not available");
    return nullptr;
  }

  auto dlss = std::unique_ptr<VulkanDlss>(new VulkanDlss(device));

  // The DLSS runtime library (libnvidia-ngx-dlss.so.*) is searched next to
  // the executable.
  const std::wstring executable_folder =
      xe::filesystem::GetExecutableFolder().wstring();
  const wchar_t* feature_paths[] = {executable_folder.c_str()};
  NVSDK_NGX_FeatureCommonInfo feature_common_info = {};
  feature_common_info.PathListInfo.Path =
      const_cast<wchar_t**>(feature_paths);
  feature_common_info.PathListInfo.Length = 1;

  const VulkanInstance* instance = device->vulkan_instance();
  const NVSDK_NGX_Result init_result = NVSDK_NGX_VULKAN_Init(
      kNgxApplicationId, executable_folder.c_str(), instance->instance(),
      device->physical_device(), device->device(),
      instance->functions().vkGetInstanceProcAddr,
      instance->functions().vkGetDeviceProcAddr, &feature_common_info);
  if (NVSDK_NGX_FAILED(init_result)) {
    XELOGI("VulkanDlss: NGX initialization failed with 0x{:08X}",
           uint32_t(init_result));
    return nullptr;
  }
  dlss->ngx_initialized_ = true;

  NVSDK_NGX_Parameter* parameters = nullptr;
  if (NVSDK_NGX_FAILED(
          NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters))) {
    XELOGI("VulkanDlss: failed to get the NGX capability parameters");
    return nullptr;
  }
  dlss->ngx_parameters_ = parameters;

  int supersampling_available = 0;
  parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available,
                  &supersampling_available);
  if (!supersampling_available) {
    int needs_updated_driver = 0;
    parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                    &needs_updated_driver);
    XELOGI("VulkanDlss: DLSS not available on this GPU or driver{}",
           needs_updated_driver ? " (a driver update would provide it)" : "");
    return nullptr;
  }

  XELOGI("VulkanDlss: DLSS is available");
  return dlss;
}

VulkanDlss::~VulkanDlss() {
  ReleaseFeature();
  DestroyOwnedImage(motion_vectors_);
  DestroyOwnedImage(depth_);
  if (ngx_parameters_) {
    NVSDK_NGX_VULKAN_DestroyParameters(
        static_cast<NVSDK_NGX_Parameter*>(ngx_parameters_));
  }
  if (ngx_initialized_) {
    NVSDK_NGX_VULKAN_Shutdown1(device_->device());
  }
}

void VulkanDlss::ReleaseFeature() {
  if (ngx_feature_) {
    NVSDK_NGX_VULKAN_ReleaseFeature(
        static_cast<NVSDK_NGX_Handle*>(ngx_feature_));
    ngx_feature_ = nullptr;
  }
}

bool VulkanDlss::CreateOwnedImage(VkFormat format, uint32_t width,
                                  uint32_t height, OwnedImage& out) {
  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = format;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(device_, image_create_info,
                                            util::MemoryPurpose::kDeviceLocal,
                                            out.image, out.memory)) {
    return false;
  }
  const VulkanDevice::Functions& dfn = device_->functions();
  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = out.image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (dfn.vkCreateImageView(device_->device(), &view_create_info, nullptr,
                            &out.view) != VK_SUCCESS) {
    DestroyOwnedImage(out);
    return false;
  }
  return true;
}

void VulkanDlss::DestroyOwnedImage(OwnedImage& image) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  if (image.view != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, image.view, nullptr);
    image.view = VK_NULL_HANDLE;
  }
  if (image.image != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device, image.image, nullptr);
    image.image = VK_NULL_HANDLE;
  }
  if (image.memory != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, image.memory, nullptr);
    image.memory = VK_NULL_HANDLE;
  }
}

bool VulkanDlss::EnsureFeature(VkCommandBuffer command_buffer,
                               uint32_t input_width, uint32_t input_height,
                               uint32_t output_width, uint32_t output_height) {
  if (ngx_feature_ && input_width == input_width_ &&
      input_height == input_height_ && output_width == output_width_ &&
      output_height == output_height_) {
    return true;
  }

  ReleaseFeature();
  if (input_width != input_width_ || input_height != input_height_) {
    DestroyOwnedImage(motion_vectors_);
    DestroyOwnedImage(depth_);
    dummies_initialized_ = false;
  }
  input_width_ = input_width;
  input_height_ = input_height;
  output_width_ = output_width;
  output_height_ = output_height;

  if (depth_.image == VK_NULL_HANDLE &&
      !CreateOwnedImage(VK_FORMAT_R32_SFLOAT, input_width, input_height,
                        depth_)) {
    XELOGE("VulkanDlss: failed to create the dummy depth image");
    return false;
  }
  if (motion_vectors_.image == VK_NULL_HANDLE &&
      !CreateOwnedImage(VK_FORMAT_R16G16_SFLOAT, input_width, input_height,
                        motion_vectors_)) {
    XELOGE("VulkanDlss: failed to create the dummy motion vector image");
    return false;
  }

  const VulkanDevice::Functions& dfn = device_->functions();

  if (!dummies_initialized_) {
    // Clear the dummy depth to a constant and the motion vectors to zero,
    // then keep both in SHADER_READ_ONLY_OPTIMAL forever.
    VkImageMemoryBarrier barriers[2];
    for (size_t i = 0; i < 2; ++i) {
      barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barriers[i].pNext = nullptr;
      barriers[i].srcAccessMask = 0;
      barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    barriers[0].image = depth_.image;
    barriers[1].image = motion_vectors_.image;
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, barriers);
    VkImageSubresourceRange clear_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                           1};
    VkClearColorValue depth_clear = {};
    depth_clear.float32[0] = 0.5f;
    dfn.vkCmdClearColorImage(command_buffer, depth_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth_clear,
                             1, &clear_range);
    VkClearColorValue mv_clear = {};
    dfn.vkCmdClearColorImage(command_buffer, motion_vectors_.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mv_clear, 1,
                             &clear_range);
    for (size_t i = 0; i < 2; ++i) {
      barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 2, barriers);
    dummies_initialized_ = true;
  }

  // Pick the DLSS quality mode closest to the scaling factor.
  const float scale =
      std::max(float(output_width) / float(input_width),
               float(output_height) / float(input_height));
  NVSDK_NGX_PerfQuality_Value quality;
  if (scale < 1.05f) {
    quality = NVSDK_NGX_PerfQuality_Value_DLAA;
  } else if (scale < 1.6f) {
    quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;
  } else if (scale < 1.85f) {
    quality = NVSDK_NGX_PerfQuality_Value_Balanced;
  } else if (scale < 2.5f) {
    quality = NVSDK_NGX_PerfQuality_Value_MaxPerf;
  } else {
    quality = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
  }

  NVSDK_NGX_DLSS_Create_Params create_params = {};
  create_params.Feature.InWidth = input_width;
  create_params.Feature.InHeight = input_height;
  create_params.Feature.InTargetWidth = output_width;
  create_params.Feature.InTargetHeight = output_height;
  create_params.Feature.InPerfQualityValue = quality;
  create_params.InFeatureCreateFlags =
      NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
  NVSDK_NGX_Handle* feature = nullptr;
  const NVSDK_NGX_Result create_result = NGX_VULKAN_CREATE_DLSS_EXT1(
      device_->device(), command_buffer, 1, 1, &feature,
      static_cast<NVSDK_NGX_Parameter*>(ngx_parameters_), &create_params);
  if (NVSDK_NGX_FAILED(create_result)) {
    XELOGE("VulkanDlss: feature creation ({}x{} -> {}x{}) failed with 0x{:08X}",
           input_width, input_height, output_width, output_height,
           uint32_t(create_result));
    return false;
  }
  ngx_feature_ = feature;
  XELOGI("VulkanDlss: created a DLSS feature, {}x{} -> {}x{}, quality mode {}",
         input_width, input_height, output_width, output_height,
         int32_t(quality));
  return true;
}

bool VulkanDlss::Evaluate(VkCommandBuffer command_buffer, VkImage input_image,
                          VkImageView input_view, VkImage output_image,
                          VkImageView output_view, VkFormat format,
                          bool reset) {
  if (!ngx_feature_) {
    return false;
  }
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                         1};
  NVSDK_NGX_Resource_VK color_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
      input_view, input_image, range, format, input_width_, input_height_,
      false);
  NVSDK_NGX_Resource_VK depth_resource = NVSDK_NGX_Create_ImageView_Resource_VK(
      depth_.view, depth_.image, range, VK_FORMAT_R32_SFLOAT, input_width_,
      input_height_, false);
  NVSDK_NGX_Resource_VK motion_vectors_resource =
      NVSDK_NGX_Create_ImageView_Resource_VK(
          motion_vectors_.view, motion_vectors_.image, range,
          VK_FORMAT_R16G16_SFLOAT, input_width_, input_height_, false);
  NVSDK_NGX_Resource_VK output_resource =
      NVSDK_NGX_Create_ImageView_Resource_VK(output_view, output_image, range,
                                             format, output_width_,
                                             output_height_, true);
  NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
  eval_params.Feature.pInColor = &color_resource;
  eval_params.Feature.pInOutput = &output_resource;
  eval_params.pInDepth = &depth_resource;
  eval_params.pInMotionVectors = &motion_vectors_resource;
  eval_params.InRenderSubrectDimensions = {input_width_, input_height_};
  eval_params.InReset = reset ? 1 : 0;
  const NVSDK_NGX_Result evaluate_result = NGX_VULKAN_EVALUATE_DLSS_EXT(
      command_buffer, static_cast<NVSDK_NGX_Handle*>(ngx_feature_),
      static_cast<NVSDK_NGX_Parameter*>(ngx_parameters_), &eval_params);
  if (NVSDK_NGX_FAILED(evaluate_result)) {
    XELOGE("VulkanDlss: evaluation failed with 0x{:08X}",
           uint32_t(evaluate_result));
    return false;
  }
  return true;
}

#endif  // XE_VULKAN_HAS_DLSS

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

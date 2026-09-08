/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_VULKAN_RESHADE_H_
#define XENIA_UI_VULKAN_VULKAN_RESHADE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace ui {
namespace vulkan {

// Native ReShade-style post-processing: compiles ReShade FX (.fx) shaders to
// SPIR-V with the vendored reshade-fx compiler and (eventually) runs their
// techniques as post-process passes on the guest output, with an ImGui overlay
// for per-effect toggles and settings.
//
// This is being built in stages (notes/72). Right now it compiles a shader and
// exposes its reflected module (techniques, passes, uniforms, textures); the
// Vulkan runtime that executes the passes comes next.
class VulkanReShade {
 public:
  // A control surfaced in the UI, one per shader uniform, from its ReShade
  // annotations.
  struct Uniform {
    std::string name;
    std::string ui_label;
    std::string ui_type;  // "drag", "slider", "combo", "checkbox", ...
    uint32_t offset = 0;
    uint32_t size = 0;
    float ui_min = 0.0f;
    float ui_max = 1.0f;
    std::vector<uint8_t> default_value;
  };

  struct Pass {
    std::string name;
    std::string vs_entry_point;
    std::string ps_entry_point;
    std::vector<uint32_t> vs_spirv;
    std::vector<uint32_t> ps_spirv;
    // Number of combined image samplers this pass's pixel shader references
    // (all bound to the effect input for now - only BackBuffer is supported).
    uint32_t sampler_count = 0;
    // Vulkan runtime objects (created by CreateRuntime).
    VkShaderModule vs_module = VK_NULL_HANDLE;
    VkShaderModule ps_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet sampler_descriptor_set = VK_NULL_HANDLE;
  };

  struct Effect {
    std::string name;
    std::string path;
    bool enabled = false;
    std::vector<Uniform> uniforms;
    std::vector<Pass> passes;
    uint32_t uniform_size = 0;

    // Vulkan runtime objects shared by the passes (created by CreateRuntime).
    VkDescriptorSetLayout set_layout_ubo = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_samplers = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkBuffer uniform_buffer = VK_NULL_HANDLE;
    VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
    void* uniform_mapped = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Framebuffer from the most recent Render; destroyed on the next Render or
    // teardown (the presenter awaits prior submissions before reusing images).
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    bool runtime_ready = false;
  };

  explicit VulkanReShade(const VulkanDevice* device) : device_(device) {}
  ~VulkanReShade();

  // Compiles a .fx file for the given output size. Returns the effect (with
  // per-entry-point SPIR-V and reflected uniforms/passes) or nullptr on
  // failure, logging the compiler errors.
  std::unique_ptr<Effect> CompileEffect(const std::string& path,
                                        uint32_t width, uint32_t height);

  // Builds the Vulkan pipelines/descriptors for the effect, targeting the
  // given output format, and uploads the uniform defaults. `sampler` is used
  // for the effect's image inputs. Returns false (and logs) on failure.
  bool CreateRuntime(Effect& effect, VkFormat format, VkSampler sampler);

  // Records the effect's passes: samples `input_view` (guest output, in
  // SHADER_READ_ONLY_OPTIMAL) and writes `output` (in COLOR_ATTACHMENT via a
  // framebuffer, left in SHADER_READ_ONLY_OPTIMAL). A transient framebuffer is
  // created and destroyed around the recording.
  bool Render(VkCommandBuffer command_buffer, Effect& effect,
              VkImageView input_view, VkImage output_image,
              VkImageView output_view, VkExtent2D extent);

  void DestroyRuntime(Effect& effect);

 private:
  const VulkanDevice* device_;
  VkSampler runtime_sampler_ = VK_NULL_HANDLE;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_RESHADE_H_

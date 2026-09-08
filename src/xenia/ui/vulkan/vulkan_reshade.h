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

#include <chrono>
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
    // ReShade "source" annotation (e.g. "timer", "frametime", "framecount");
    // empty for a normal user-adjustable uniform. Special uniforms are filled
    // by the runtime each frame and are hidden from the settings UI.
    std::string source;
  };

  struct Pass {
    std::string name;
    std::string vs_entry_point;
    std::string ps_entry_point;
    std::vector<uint32_t> vs_spirv;
    std::vector<uint32_t> ps_spirv;
    // Number of combined image samplers this pass's pixel shader references.
    uint32_t sampler_count = 0;
    // The texture (unique) name each sampler slot references, in slot order.
    std::vector<std::string> sampler_texture_names;
    // The effect-owned textures this pass renders to, in attachment order.
    // Empty = the pass writes the backbuffer (the effect's output chain).
    std::vector<std::string> render_target_names;
    bool clear_render_targets = false;
    // Fixed-function state reflected from the FX pass (attachment 0's blend
    // is applied to every attachment slot below).
    bool blend_enable = false;
    VkBlendFactor src_color_factor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_color_factor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD;
    VkBlendFactor src_alpha_factor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dst_alpha_factor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags color_write_mask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t num_vertices = 3;
    // Render area for a render-target pass (the targets' size); 0 = the
    // effect output extent (backbuffer passes).
    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;
    // Vulkan runtime objects (created by CreateRuntime).
    VkShaderModule vs_module = VK_NULL_HANDLE;
    VkShaderModule ps_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    // Stable framebuffer for a render-target pass (its attachments are the
    // effect's own images); backbuffer passes use per-frame transient ones.
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet sampler_descriptor_set = VK_NULL_HANDLE;
  };

  // An effect-owned image resource. Backbuffer/depth are external (the
  // presenter's guest output); file textures are loaded from disk.
  struct Texture {
    std::string name;         // unique name the samplers reference
    std::string source_file;  // absolute path if loaded from a file
    bool is_backbuffer = false;
    bool is_depth = false;
    // A pass renders into this texture (created as a color attachment that
    // later passes sample).
    bool is_render_target = false;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  struct Effect {
    std::string name;
    std::string path;
    bool enabled = false;
    // Output size the effect was compiled for (BUFFER_WIDTH/HEIGHT are baked
    // into the shader, and the render-target textures are sized from them).
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Uniform> uniforms;
    std::vector<Pass> passes;
    std::vector<Texture> textures;
    uint32_t uniform_size = 0;

    // Vulkan runtime objects shared by the passes (created by CreateRuntime).
    VkDescriptorSetLayout set_layout_ubo = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_samplers = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkBuffer uniform_buffer = VK_NULL_HANDLE;
    VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
    void* uniform_mapped = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Ping-pong partner for the presenter's output image when more than one
    // pass writes the backbuffer: passes alternate between the two so each
    // one can sample the previous result, with the last landing on the
    // presenter's output.
    VkImage chain_image = VK_NULL_HANDLE;
    VkDeviceMemory chain_memory = VK_NULL_HANDLE;
    VkImageView chain_view = VK_NULL_HANDLE;
    // Backbuffer-pass framebuffers from the most recent Render; destroyed on
    // the next Render or teardown (the presenter awaits prior submissions
    // before reusing images).
    std::vector<VkFramebuffer> transient_framebuffers;
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
  // framebuffer, left in SHADER_READ_ONLY_OPTIMAL). Render-target passes
  // write the effect's own textures; backbuffer passes ping-pong between the
  // output and the chain image, the last one landing on the output.
  bool Render(VkCommandBuffer command_buffer, Effect& effect,
              VkImageView input_view, VkImage output_image,
              VkImageView output_view, VkExtent2D extent,
              VkImageView depth_view = VK_NULL_HANDLE);

  void DestroyRuntime(Effect& effect);

  // Writes the effect's built-in (source-annotated) uniforms - timer,
  // frametime, framecount - into the mapped uniform buffer. Call once per
  // frame on the paint thread before Render.
  void UpdateSystemUniforms(Effect& effect);

  // Sets the guest depth-buffer convention applied to the ReShade depth
  // macros when compiling an effect (RESHADE_DEPTH_INPUT_IS_REVERSED /
  // _UPSIDE_DOWN). Call before CompileEffect.
  void SetDepthConvention(bool reversed, bool upside_down) {
    depth_reversed_ = reversed;
    depth_upside_down_ = upside_down;
  }

 private:
  const VulkanDevice* device_;
  bool depth_reversed_ = true;
  bool depth_upside_down_ = false;
  VkSampler runtime_sampler_ = VK_NULL_HANDLE;
  // Timing for the built-in uniforms.
  bool timing_started_ = false;
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_frame_time_;
  uint32_t frame_count_ = 0;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_RESHADE_H_

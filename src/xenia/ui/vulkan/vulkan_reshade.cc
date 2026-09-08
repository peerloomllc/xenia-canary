/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_reshade.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_util.h"

#include "third_party/stb/stb_image.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

namespace xe {
namespace ui {
namespace vulkan {

namespace {

// Finds a named annotation in a list, if present.
const reshadefx::annotation* FindAnnotationIn(
    const std::vector<reshadefx::annotation>& annotations, const char* name) {
  for (const auto& a : annotations) {
    if (a.name == name) {
      return &a;
    }
  }
  return nullptr;
}
const reshadefx::annotation* FindAnnotation(const reshadefx::uniform& u,
                                            const char* name) {
  return FindAnnotationIn(u.annotations, name);
}

std::vector<uint32_t> ToWords(const std::string& bytes) {
  std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
  if (!words.empty()) {
    std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
  }
  return words;
}

}  // namespace

std::unique_ptr<VulkanReShade::Effect> VulkanReShade::CompileEffect(
    const std::string& path, uint32_t width, uint32_t height) {
  reshadefx::preprocessor pp;
  pp.add_macro_definition("__RESHADE__", "60000");
  pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
  pp.add_macro_definition("__RENDERER__", "0x20000");  // Vulkan
  pp.add_macro_definition("BUFFER_WIDTH", std::to_string(width));
  pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(height));
  pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
  pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
  pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");

  std::filesystem::path fs_path(path);
  pp.add_include_path(fs_path.parent_path());

  if (!pp.append_file(fs_path)) {
    XELOGE("VulkanReShade: preprocess of '{}' failed: {}", path, pp.errors());
    return nullptr;
  }

  std::unique_ptr<reshadefx::codegen> backend(reshadefx::create_codegen_spirv(
      /*vulkan_semantics=*/true, /*debug_info=*/false, /*spec_constants=*/false,
      /*invert_y_axis=*/false));

  reshadefx::parser parser;
  if (!parser.parse(pp.output(), backend.get())) {
    XELOGE("VulkanReShade: parse of '{}' failed: {}{}", path, pp.errors(),
           parser.errors());
    return nullptr;
  }

  const reshadefx::effect_module& module = backend->module();

  auto effect = std::make_unique<Effect>();
  effect->path = path;
  effect->name = fs_path.stem().string();
  effect->uniform_size = module.total_uniform_size;

  for (const auto& u : module.uniforms) {
    Uniform uniform;
    uniform.name = u.name;
    uniform.offset = u.offset;
    uniform.size = u.size;
    if (const auto* a = FindAnnotation(u, "source")) {
      uniform.source = a->value.string_data;
    }
    uniform.default_value.assign(
        reinterpret_cast<const uint8_t*>(u.initializer_value.as_float),
        reinterpret_cast<const uint8_t*>(u.initializer_value.as_float) +
            std::min<uint32_t>(u.size, uint32_t(sizeof(u.initializer_value.as_float))));
    if (const auto* a = FindAnnotation(u, "ui_label")) {
      uniform.ui_label = a->value.string_data;
    }
    if (uniform.ui_label.empty()) {
      uniform.ui_label = u.name;
    }
    if (const auto* a = FindAnnotation(u, "ui_type")) {
      uniform.ui_type = a->value.string_data;
    }
    if (const auto* a = FindAnnotation(u, "ui_min")) {
      uniform.ui_min = a->value.as_float[0];
    }
    if (const auto* a = FindAnnotation(u, "ui_max")) {
      uniform.ui_max = a->value.as_float[0];
    }
    effect->uniforms.push_back(std::move(uniform));
  }

  // Textures the effect references. Backbuffer/depth are provided by the
  // presenter (the guest output); file textures are loaded in CreateRuntime.
  const std::filesystem::path shader_dir = fs_path.parent_path();
  for (const auto& tex : module.textures) {
    Texture texture;
    texture.name = tex.unique_name;
    texture.is_backbuffer =
        tex.semantic == "COLOR" || tex.semantic == "SV_TARGET";
    texture.is_depth = tex.semantic == "DEPTH";
    if (const auto* a = FindAnnotationIn(tex.annotations, "source")) {
      const std::string& src = a->value.string_data;
      if (!src.empty()) {
        // Resolve against the shader dir and the usual texture locations
        // (ReShade shaders name a bare file and rely on a texture path).
        const std::filesystem::path candidates[] = {
            shader_dir / src,
            shader_dir / "Textures" / src,
            shader_dir.parent_path() / "Textures" / src,
        };
        std::error_code ec;
        for (const auto& candidate : candidates) {
          if (std::filesystem::exists(candidate, ec)) {
            texture.source_file = candidate.string();
            break;
          }
        }
        if (texture.source_file.empty()) {
          texture.source_file = (shader_dir / src).string();  // report attempt
        }
      }
    }
    effect->textures.push_back(std::move(texture));
  }

  // Assemble per-entry-point SPIR-V once, keyed by entry point name.
  auto spirv_for = [&](const std::string& entry_point) -> std::vector<uint32_t> {
    if (entry_point.empty()) {
      return {};
    }
    std::string spirv, assembly, errors;
    if (!backend->assemble_code_for_entry_point(entry_point, spirv, assembly,
                                                errors)) {
      XELOGE("VulkanReShade: SPIR-V assembly for '{}' failed: {}", entry_point,
             errors);
      return {};
    }
    return ToWords(spirv);
  };

  for (const auto& technique : module.techniques) {
    for (const auto& pass : technique.passes) {
      if (pass.cs_entry_point.size()) {
        // Compute passes are not handled in this first runtime.
        continue;
      }
      Pass p;
      p.name = pass.name.empty() ? technique.name : pass.name;
      p.vs_entry_point = pass.vs_entry_point;
      p.ps_entry_point = pass.ps_entry_point;
      p.vs_spirv = spirv_for(pass.vs_entry_point);
      p.ps_spirv = spirv_for(pass.ps_entry_point);
      p.sampler_count = uint32_t(pass.sampler_bindings.size());
      for (const auto& sb : pass.sampler_bindings) {
        std::string texture_name;
        if (sb.index < module.samplers.size()) {
          texture_name = module.samplers[sb.index].texture_name;
        }
        p.sampler_texture_names.push_back(texture_name);
      }
      if (p.vs_spirv.empty() || p.ps_spirv.empty()) {
        XELOGE("VulkanReShade: '{}' pass '{}' missing SPIR-V, skipping effect",
               effect->name, p.name);
        return nullptr;
      }
      effect->passes.push_back(std::move(p));
    }
  }

  XELOGI(
      "VulkanReShade: compiled '{}' - {} uniform(s) ({} bytes), {} pass(es)",
      effect->name, effect->uniforms.size(), effect->uniform_size,
      effect->passes.size());
  return effect;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

namespace xe {
namespace ui {
namespace vulkan {

VulkanReShade::~VulkanReShade() {}

void VulkanReShade::DestroyRuntime(Effect& effect) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (Pass& pass : effect.passes) {
    if (pass.pipeline != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pass.pipeline, nullptr);
      pass.pipeline = VK_NULL_HANDLE;
    }
    if (pass.vs_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, pass.vs_module, nullptr);
      pass.vs_module = VK_NULL_HANDLE;
    }
    if (pass.ps_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, pass.ps_module, nullptr);
      pass.ps_module = VK_NULL_HANDLE;
    }
    pass.descriptor_set = VK_NULL_HANDLE;
  }
  if (effect.uniform_mapped) {
    dfn.vkUnmapMemory(device, effect.uniform_memory);
    effect.uniform_mapped = nullptr;
  }
  if (effect.uniform_buffer != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, effect.uniform_buffer, nullptr);
    effect.uniform_buffer = VK_NULL_HANDLE;
  }
  if (effect.uniform_memory != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, effect.uniform_memory, nullptr);
    effect.uniform_memory = VK_NULL_HANDLE;
  }
  if (effect.descriptor_pool != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorPool(device, effect.descriptor_pool, nullptr);
    effect.descriptor_pool = VK_NULL_HANDLE;
  }
  if (effect.pipeline_layout != VK_NULL_HANDLE) {
    dfn.vkDestroyPipelineLayout(device, effect.pipeline_layout, nullptr);
    effect.pipeline_layout = VK_NULL_HANDLE;
  }
  if (effect.set_layout_ubo != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, effect.set_layout_ubo, nullptr);
    effect.set_layout_ubo = VK_NULL_HANDLE;
  }
  if (effect.set_layout_samplers != VK_NULL_HANDLE) {
    dfn.vkDestroyDescriptorSetLayout(device, effect.set_layout_samplers,
                                     nullptr);
    effect.set_layout_samplers = VK_NULL_HANDLE;
  }
  if (effect.framebuffer != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, effect.framebuffer, nullptr);
    effect.framebuffer = VK_NULL_HANDLE;
  }
  if (effect.render_pass != VK_NULL_HANDLE) {
    dfn.vkDestroyRenderPass(device, effect.render_pass, nullptr);
    effect.render_pass = VK_NULL_HANDLE;
  }
  for (Texture& texture : effect.textures) {
    if (texture.view != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, texture.view, nullptr);
      texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, texture.image, nullptr);
      texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, texture.memory, nullptr);
      texture.memory = VK_NULL_HANDLE;
    }
  }
  effect.runtime_ready = false;
}

bool VulkanReShade::CreateRuntime(Effect& effect, VkFormat format,
                                  VkSampler sampler) {
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();
  runtime_sampler_ = sampler;
  effect.format = format;

  uint32_t max_samplers = 0;
  for (const Pass& pass : effect.passes) {
    max_samplers = std::max(max_samplers, pass.sampler_count);
  }

  // Descriptor set layouts: set 0 = uniform buffer, set 1 = combined image
  // samplers (ReShade's Vulkan SPIR-V uses set 0 binding 0 for the UBO and
  // set 1 for sampled images).
  {
    VkDescriptorSetLayoutBinding ubo_binding = {};
    ubo_binding.binding = 0;
    ubo_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo_binding.descriptorCount = 1;
    ubo_binding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &ubo_binding;
    if (dfn.vkCreateDescriptorSetLayout(device, &ci, nullptr,
                                        &effect.set_layout_ubo) != VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to create the UBO set layout");
      DestroyRuntime(effect);
      return false;
    }
  }
  {
    std::vector<VkDescriptorSetLayoutBinding> bindings(max_samplers);
    for (uint32_t i = 0; i < max_samplers; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = uint32_t(bindings.size());
    ci.pBindings = bindings.empty() ? nullptr : bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(device, &ci, nullptr,
                                        &effect.set_layout_samplers) !=
        VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to create the sampler set layout");
      DestroyRuntime(effect);
      return false;
    }
  }
  {
    VkDescriptorSetLayout set_layouts[2] = {effect.set_layout_ubo,
                                            effect.set_layout_samplers};
    VkPipelineLayoutCreateInfo ci = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = set_layouts;
    if (dfn.vkCreatePipelineLayout(device, &ci, nullptr,
                                   &effect.pipeline_layout) != VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to create the pipeline layout");
      DestroyRuntime(effect);
      return false;
    }
  }

  // Render pass writing the effect output, left ready to sample.
  {
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color_ref = {0,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &attachment;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    if (dfn.vkCreateRenderPass(device, &ci, nullptr, &effect.render_pass) !=
        VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to create the render pass");
      DestroyRuntime(effect);
      return false;
    }
  }

  // Uniform buffer (host visible), filled with the reflected defaults.
  const VkDeviceSize ubo_size = std::max<VkDeviceSize>(effect.uniform_size, 16);
  if (!util::CreateDedicatedAllocationBuffer(
          device_, ubo_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          util::MemoryPurpose::kUpload, effect.uniform_buffer,
          effect.uniform_memory) ||
      dfn.vkMapMemory(device, effect.uniform_memory, 0, VK_WHOLE_SIZE, 0,
                      &effect.uniform_mapped) != VK_SUCCESS) {
    XELOGE("VulkanReShade: failed to create the uniform buffer");
    DestroyRuntime(effect);
    return false;
  }
  std::memset(effect.uniform_mapped, 0, size_t(ubo_size));
  for (const Uniform& u : effect.uniforms) {
    if (!u.default_value.empty() && u.offset + u.default_value.size() <=
                                        size_t(ubo_size)) {
      std::memcpy(static_cast<uint8_t*>(effect.uniform_mapped) + u.offset,
                  u.default_value.data(), u.default_value.size());
    }
  }

  // Descriptor pool and one (ubo, samplers) set pair per pass.
  {
    const uint32_t pass_count = uint32_t(effect.passes.size());
    VkDescriptorPoolSize sizes[2];
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = std::max(1u, pass_count);
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = std::max(1u, pass_count * std::max(1u, max_samplers));
    VkDescriptorPoolCreateInfo ci = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = std::max(1u, pass_count * 2);
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(device, &ci, nullptr,
                                   &effect.descriptor_pool) != VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to create the descriptor pool");
      DestroyRuntime(effect);
      return false;
    }
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  for (Pass& pass : effect.passes) {
    pass.vs_module = util::CreateShaderModule(device_, pass.vs_spirv.data(),
                                              pass.vs_spirv.size() * 4);
    pass.ps_module = util::CreateShaderModule(device_, pass.ps_spirv.data(),
                                              pass.ps_spirv.size() * 4);
    if (pass.vs_module == VK_NULL_HANDLE || pass.ps_module == VK_NULL_HANDLE) {
      XELOGE("VulkanReShade: failed to create shader modules for pass '{}'",
             pass.name);
      DestroyRuntime(effect);
      return false;
    }
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = pass.vs_module;
    stages[0].pName = pass.vs_entry_point.c_str();
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pass.ps_module;
    stages[1].pName = pass.ps_entry_point.c_str();

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_assembly;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pColorBlendState = &color_blend;
    ci.pDynamicState = &dynamic_state;
    ci.layout = effect.pipeline_layout;
    ci.renderPass = effect.render_pass;
    if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                      &pass.pipeline) != VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to create the pipeline for pass '{}'",
             pass.name);
      DestroyRuntime(effect);
      return false;
    }

    // Allocate and write the descriptor sets for this pass.
    VkDescriptorSetLayout set_layouts[2] = {effect.set_layout_ubo,
                                            effect.set_layout_samplers};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = effect.descriptor_pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = set_layouts;
    if (dfn.vkAllocateDescriptorSets(device, &ai, sets) != VK_SUCCESS) {
      XELOGE("VulkanReShade: failed to allocate descriptor sets");
      DestroyRuntime(effect);
      return false;
    }
    pass.descriptor_set = sets[0];
    pass.sampler_descriptor_set = sets[1];

    VkDescriptorBufferInfo buffer_info = {};
    buffer_info.buffer = effect.uniform_buffer;
    buffer_info.range = ubo_size;
    VkWriteDescriptorSet ubo_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    ubo_write.dstSet = sets[0];
    ubo_write.dstBinding = 0;
    ubo_write.descriptorCount = 1;
    ubo_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo_write.pBufferInfo = &buffer_info;
    dfn.vkUpdateDescriptorSets(device, 1, &ubo_write, 0, nullptr);
    // The sampler descriptors are written at render time (they reference the
    // input image, which varies per frame).
  }

  // Load file-sourced textures (e.g. LUTs) and upload them via a one-shot
  // transfer. Backbuffer/depth textures are provided by the presenter.
  for (Texture& texture : effect.textures) {
    if (texture.source_file.empty()) {
      continue;
    }
    int tw = 0, th = 0, tc = 0;
    stbi_uc* pixels = stbi_load(texture.source_file.c_str(), &tw, &th, &tc, 4);
    if (!pixels) {
      XELOGW("VulkanReShade: could not load texture '{}'",
             texture.source_file);
      continue;
    }
    const VkDeviceSize data_size = VkDeviceSize(tw) * th * 4;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {uint32_t(tw), uint32_t(th), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(
            device_, ici, util::MemoryPurpose::kDeviceLocal, texture.image,
            texture.memory)) {
      stbi_image_free(pixels);
      continue;
    }
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = texture.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &vci, nullptr, &texture.view) !=
        VK_SUCCESS) {
      stbi_image_free(pixels);
      continue;
    }
    // Staging buffer.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void* staging_mapped = nullptr;
    if (util::CreateDedicatedAllocationBuffer(
            device_, data_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            util::MemoryPurpose::kUpload, staging, staging_memory) &&
        dfn.vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0,
                        &staging_mapped) == VK_SUCCESS) {
      std::memcpy(staging_mapped, pixels, size_t(data_size));
      dfn.vkUnmapMemory(device, staging_memory);
      // One-shot upload command buffer.
      VkCommandPoolCreateInfo cpci = {
          VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      cpci.queueFamilyIndex = device_->queue_family_graphics_compute();
      VkCommandPool upload_pool = VK_NULL_HANDLE;
      dfn.vkCreateCommandPool(device, &cpci, nullptr, &upload_pool);
      VkCommandBufferAllocateInfo cbai = {
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      cbai.commandPool = upload_pool;
      cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cbai.commandBufferCount = 1;
      VkCommandBuffer cb = VK_NULL_HANDLE;
      dfn.vkAllocateCommandBuffers(device, &cbai, &cb);
      VkCommandBufferBeginInfo bi = {
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      dfn.vkBeginCommandBuffer(cb, &bi);
      VkImageMemoryBarrier to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      to_dst.srcAccessMask = 0;
      to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_dst.image = texture.image;
      to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &to_dst);
      VkBufferImageCopy copy = {};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {uint32_t(tw), uint32_t(th), 1};
      dfn.vkCmdCopyBufferToImage(cb, staging, texture.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &copy);
      VkImageMemoryBarrier to_read = to_dst;
      to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &to_read);
      dfn.vkEndCommandBuffer(cb);
      VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkFence fence = VK_NULL_HANDLE;
      dfn.vkCreateFence(device, &fci, nullptr, &fence);
      VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      {
        auto q = device_->AcquireQueue(
            device_->queue_family_graphics_compute(), 0);
        dfn.vkQueueSubmit(q.queue(), 1, &si, fence);
      }
      dfn.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
      dfn.vkDestroyFence(device, fence, nullptr);
      dfn.vkDestroyCommandPool(device, upload_pool, nullptr);
      XELOGI("VulkanReShade: loaded texture '{}' ({}x{})", texture.name, tw,
             th);
    }
    if (staging != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, staging, nullptr);
    }
    if (staging_memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, staging_memory, nullptr);
    }
    stbi_image_free(pixels);
  }

  effect.runtime_ready = true;
  XELOGI("VulkanReShade: runtime built for '{}' ({} pass(es), {} sampler(s))",
         effect.name, effect.passes.size(), max_samplers);
  return true;
}

bool VulkanReShade::Render(VkCommandBuffer command_buffer, Effect& effect,
                           VkImageView input_view, VkImage output_image,
                           VkImageView output_view, VkExtent2D extent) {
  if (!effect.runtime_ready) {
    return false;
  }
  const VulkanDevice::Functions& dfn = device_->functions();
  const VkDevice device = device_->device();

  // The previous frame's framebuffer is done (the presenter awaits prior
  // submissions before reusing the output image); destroy it and make a new
  // one for this frame's output view.
  if (effect.framebuffer != VK_NULL_HANDLE) {
    dfn.vkDestroyFramebuffer(device, effect.framebuffer, nullptr);
    effect.framebuffer = VK_NULL_HANDLE;
  }
  VkFramebufferCreateInfo fb_ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb_ci.renderPass = effect.render_pass;
  fb_ci.attachmentCount = 1;
  fb_ci.pAttachments = &output_view;
  fb_ci.width = extent.width;
  fb_ci.height = extent.height;
  fb_ci.layers = 1;
  if (dfn.vkCreateFramebuffer(device, &fb_ci, nullptr, &effect.framebuffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkFramebuffer framebuffer = effect.framebuffer;

  VkRenderPassBeginInfo rp_bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp_bi.renderPass = effect.render_pass;
  rp_bi.framebuffer = framebuffer;
  rp_bi.renderArea.extent = extent;
  dfn.vkCmdBeginRenderPass(command_buffer, &rp_bi,
                           VK_SUBPASS_CONTENTS_INLINE);
  // Flip Y with a negative-height viewport: ReShade's fullscreen vertex shader
  // targets Direct3D clip space, so without this the output is vertically
  // mirrored under Vulkan (confirmed with a UV probe).
  VkViewport viewport = {0.0f, float(extent.height), float(extent.width),
                         -float(extent.height), 0.0f, 1.0f};
  VkRect2D scissor = {{0, 0}, extent};
  dfn.vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  dfn.vkCmdSetScissor(command_buffer, 0, 1, &scissor);

  for (Pass& pass : effect.passes) {
    VkDescriptorSet sampler_set = pass.sampler_descriptor_set;
    if (pass.sampler_count) {
      // Bind the input image to every sampler slot (only BackBuffer supported).
      std::vector<VkDescriptorImageInfo> image_infos(pass.sampler_count);
      std::vector<VkWriteDescriptorSet> writes(pass.sampler_count);
      for (uint32_t i = 0; i < pass.sampler_count; ++i) {
        // Resolve the slot's texture: the backbuffer (guest output) binds the
        // presenter's input; a loaded file texture binds its own image;
        // anything else falls back to the input.
        VkImageView slot_view = input_view;
        if (i < pass.sampler_texture_names.size()) {
          const std::string& tex_name = pass.sampler_texture_names[i];
          for (const Texture& texture : effect.textures) {
            if (texture.name == tex_name) {
              if (!texture.is_backbuffer && texture.view != VK_NULL_HANDLE) {
                slot_view = texture.view;
              }
              break;
            }
          }
        }
        image_infos[i].sampler = runtime_sampler_;
        image_infos[i].imageView = slot_view;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = sampler_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
      }
      dfn.vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(),
                                 0, nullptr);
    }
    dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pass.pipeline);
    VkDescriptorSet bind_sets[2] = {pass.descriptor_set, sampler_set};
    dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                effect.pipeline_layout, 0, 2, bind_sets, 0,
                                nullptr);
    dfn.vkCmdDraw(command_buffer, 3, 1, 0, 0);
  }

  dfn.vkCmdEndRenderPass(command_buffer);
  (void)framebuffer;  // Kept on the effect, destroyed on the next Render.
  return true;
}

void VulkanReShade::UpdateSystemUniforms(Effect& effect) {
  if (!effect.uniform_mapped) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!timing_started_) {
    start_time_ = now;
    last_frame_time_ = now;
    timing_started_ = true;
  }
  const float timer_ms =
      std::chrono::duration<float, std::milli>(now - start_time_).count();
  const float frame_ms =
      std::chrono::duration<float, std::milli>(now - last_frame_time_).count();
  last_frame_time_ = now;
  ++frame_count_;
  auto* base = static_cast<uint8_t*>(effect.uniform_mapped);
  for (const Uniform& u : effect.uniforms) {
    if (u.source.empty()) {
      continue;
    }
    if (u.source == "timer") {
      std::memcpy(base + u.offset, &timer_ms, sizeof(float));
    } else if (u.source == "frametime") {
      std::memcpy(base + u.offset, &frame_ms, sizeof(float));
    } else if (u.source == "framecount") {
      const int32_t fc = int32_t(frame_count_);
      std::memcpy(base + u.offset, &fc, sizeof(int32_t));
    }
    // Other sources (date, pingpong, mouse, key) are left at defaults for now.
  }
}

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

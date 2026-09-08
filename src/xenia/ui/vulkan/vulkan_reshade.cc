/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/vulkan/vulkan_reshade.h"

#include <cstring>
#include <filesystem>

#include "xenia/base/logging.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

namespace xe {
namespace ui {
namespace vulkan {

namespace {

// Finds a named string/float annotation on a uniform, if present.
const reshadefx::annotation* FindAnnotation(const reshadefx::uniform& u,
                                            const char* name) {
  for (const auto& a : u.annotations) {
    if (a.name == name) {
      return &a;
    }
  }
  return nullptr;
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

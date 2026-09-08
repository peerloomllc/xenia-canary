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
    float ui_min = 0.0f;
    float ui_max = 1.0f;
  };

  struct Pass {
    std::string name;
    std::string vs_entry_point;
    std::string ps_entry_point;
    std::vector<uint32_t> vs_spirv;
    std::vector<uint32_t> ps_spirv;
  };

  struct Effect {
    std::string name;
    std::string path;
    bool enabled = false;
    std::vector<Uniform> uniforms;
    std::vector<Pass> passes;
    uint32_t uniform_size = 0;
  };

  explicit VulkanReShade(const VulkanDevice* device) : device_(device) {}

  // Compiles a .fx file for the given output size. Returns the effect (with
  // per-entry-point SPIR-V and reflected uniforms/passes) or nullptr on
  // failure, logging the compiler errors.
  std::unique_ptr<Effect> CompileEffect(const std::string& path,
                                        uint32_t width, uint32_t height);

 private:
  const VulkanDevice* device_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_VULKAN_RESHADE_H_

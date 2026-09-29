/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Host render target path: copies the alpha between a k_2_10_10_10_FLOAT
// render target, emulated as R16G16B16A16_SFLOAT, and its A2B10G10R10_UNORM
// companion, into which guest draws blend the same alpha with 2-bit rounding.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XePushConstants {
  uvec2 xe_origin;
  uvec2 xe_extent;
  // 1: companion -> render target, 0: render target -> companion.
  uint xe_to_target;
};

layout(set = 0, binding = 0, rgba16f) uniform image2DMS xe_target;
layout(set = 1, binding = 0, rgb10_a2) uniform image2DMS xe_companion;

void main() {
  if (any(greaterThanEqual(gl_GlobalInvocationID.xy, xe_extent))) {
    return;
  }
  ivec2 xy = ivec2(xe_origin + gl_GlobalInvocationID.xy);
  int sample_count = imageSamples(xe_target);
  for (int i = 0; i < sample_count; ++i) {
    vec4 color = imageLoad(xe_target, xy, i);
    float companion_alpha = imageLoad(xe_companion, xy, i).a;
    if (xe_to_target != 0u) {
      if (companion_alpha != color.a) {
        color.a = companion_alpha;
        imageStore(xe_target, xy, i, color);
      }
    } else {
      // Stored with the 2-bit rounding of the format.
      imageStore(xe_companion, xy, i, vec4(0.0, 0.0, 0.0, color.a));
    }
  }
}

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Host render target path: rounds the alpha of a k_2_10_10_10_FLOAT render
// target, emulated as R16G16B16A16_SFLOAT, to the 2 bits the Xenos keeps,
// after a blended draw, so that repeated blending over it behaves as on the
// console instead of decaying in the higher host precision.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XePushConstants {
  uvec2 xe_origin;
  uvec2 xe_extent;
};

layout(set = 0, binding = 0, rgba16f) uniform image2DMS xe_target;

float Round2BitAlpha(float alpha) {
  // Same as packing into the guest format: clamp, then round to nearest.
  return floor(clamp(alpha, 0.0, 1.0) * 3.0 + 0.5) * (1.0 / 3.0);
}

void main() {
  if (any(greaterThanEqual(gl_GlobalInvocationID.xy, xe_extent))) {
    return;
  }
  ivec2 xy = ivec2(xe_origin + gl_GlobalInvocationID.xy);
  int sample_count = imageSamples(xe_target);
  for (int i = 0; i < sample_count; ++i) {
    vec4 color = imageLoad(xe_target, xy, i);
    float alpha = Round2BitAlpha(color.a);
    // Most samples already hold a 2-bit value; don't spend bandwidth on them.
    if (alpha != color.a) {
      color.a = alpha;
      imageStore(xe_target, xy, i, color);
    }
  }
}

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
// target (R16G16B16A16_SFLOAT on the host) to the 2 bits the Xenos keeps, in
// the pixels a guest draw covered. Drawn with the draw's own vertex shader and
// geometry in a pass without attachments, right after the draw, so only those
// pixels are touched. Pixels whose alpha is already a 2-bit value are left
// alone, so overlapping primitives rounding the same pixel are harmless.
// Set 4 follows the 4 descriptor sets of the guest pipeline layout.

layout(set = 4, binding = 0, rgba16f) uniform image2DMS xe_target;

float Round2BitAlpha(float alpha) {
  // Same as packing into the guest format: clamp, then round to nearest.
  return floor(clamp(alpha, 0.0, 1.0) * 3.0 + 0.5) * (1.0 / 3.0);
}

void main() {
  ivec2 xy = ivec2(gl_FragCoord.xy);
  int sample_count = imageSamples(xe_target);
  for (int i = 0; i < sample_count; ++i) {
    vec4 color = imageLoad(xe_target, xy, i);
    float alpha = Round2BitAlpha(color.a);
    if (alpha != color.a) {
      color.a = alpha;
      imageStore(xe_target, xy, i, color);
    }
  }
}

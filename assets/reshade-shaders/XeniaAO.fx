// Screen-space ambient occlusion from the guest depth buffer (needs
// --reshade_depth). Spiral-tap depth AO with a per-pixel rotation, a
// depth-aware two-pass blur and a distance fade, composited by darkening
// the image in creases and contact areas. Tuned for Xbox 360 titles fed
// through Xenia's depth resolve; every value is a slider in the overlay.
#include "ReShade.fxh"

uniform float Strength <
    ui_type = "slider"; ui_label = "Strength";
    ui_min = 0.0; ui_max = 2.0;
> = 1.0;

uniform float SampleRadius <
    ui_type = "slider"; ui_label = "Sample radius";
    ui_min = 0.2; ui_max = 4.0;
> = 1.0;

uniform float DepthRange <
    ui_type = "slider"; ui_label = "Occlusion depth range";
    ui_min = 0.5; ui_max = 8.0;
> = 2.0;

uniform float FadeDistance <
    ui_type = "slider"; ui_label = "Distance fade";
    ui_min = 0.05; ui_max = 1.0;
> = 0.35;

uniform bool DebugAO <
    ui_label = "Show AO only";
> = false;

texture XeniaAOTexA { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
texture XeniaAOTexB { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler XeniaAOSamplerA { Texture = XeniaAOTexA; };
sampler XeniaAOSamplerB { Texture = XeniaAOTexB; };

// Interleaved gradient noise - a stable per-pixel rotation for the spiral.
float IGN(float2 pixel) {
  return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// AO term + linear center depth, for the depth-aware blur that follows.
float4 PS_AOGen(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  float depth = ReShade::GetLinearizedDepth(texcoord);
  // Sky / far plane: leave it untouched.
  if (depth >= 0.98) {
    return float4(1.0, depth, 0.0, 1.0);
  }
  // Screen-space radius shrinks with distance for a perspective-consistent
  // world-ish radius; clamped so near geometry does not sample the whole
  // screen.
  float radius_uv = min(SampleRadius * 0.01 / max(depth * 20.0, 0.05), 0.05);
  float rotation = IGN(vpos.xy) * 6.2831853;
  float occlusion = 0.0;
  float total_weight = 0.0;
  for (int i = 0; i < 10; i++) {
    // Golden-angle spiral.
    float angle = rotation + float(i) * 2.3999632;
    float dist = sqrt((float(i) + 0.5) / 10.0);
    float2 offset = float2(cos(angle), sin(angle)) * dist * radius_uv;
    offset.y *= BUFFER_ASPECT_RATIO;
    float tap_depth = ReShade::GetLinearizedDepth(texcoord + offset);
    // Positive when the tap is closer to the camera than the center:
    // geometry in front that occludes this pixel.
    float diff = depth - tap_depth;
    // Bias rejects self-occlusion on flat surfaces; the range term fades
    // out occluders too far in front (a character should not darken the
    // wall far behind them).
    float range = DepthRange * 0.001 * (0.5 + depth * 10.0);
    float bias = 0.0004 + depth * 0.0015;
    float ao = step(bias, diff) * (1.0 - smoothstep(range * 0.5, range * 4.0, diff));
    // Closer taps in the spiral count more.
    float weight = 1.0 - dist * 0.5;
    occlusion += ao * weight;
    total_weight += weight;
  }
  occlusion /= max(total_weight, 0.001);
  // Fade the effect out with distance so the far scene stays clean.
  occlusion *= 1.0 - smoothstep(FadeDistance * 0.5, FadeDistance, depth);
  return float4(saturate(1.0 - occlusion), depth, 0.0, 1.0);
}

// Depth-aware separable blur: smooths the spiral noise without bleeding AO
// across depth edges (silhouettes stay crisp).
float BlurAO(sampler s, float2 texcoord, float2 axis) {
  float4 center = tex2D(s, texcoord);
  float center_depth = center.y;
  float sum = center.x;
  float total = 1.0;
  for (int i = 1; i <= 4; i++) {
    float w = exp(-float(i * i) * 0.18);
    float2 offset = axis * float(i);
    float4 a = tex2D(s, texcoord + offset);
    float4 b = tex2D(s, texcoord - offset);
    float wa = w * saturate(1.0 - abs(a.y - center_depth) * 400.0);
    float wb = w * saturate(1.0 - abs(b.y - center_depth) * 400.0);
    sum += a.x * wa + b.x * wb;
    total += wa + wb;
  }
  return sum / total;
}

float4 PS_BlurH(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  float blurred = BlurAO(XeniaAOSamplerA, texcoord, float2(BUFFER_RCP_WIDTH, 0.0));
  float depth = tex2D(XeniaAOSamplerA, texcoord).y;
  return float4(blurred, depth, 0.0, 1.0);
}

float3 PS_Apply(float4 vpos : SV_Position, float2 texcoord : TEXCOORD) : SV_Target {
  float ao = BlurAO(XeniaAOSamplerB, texcoord, float2(0.0, BUFFER_RCP_HEIGHT));
  ao = saturate(1.0 - Strength * (1.0 - ao));
  if (DebugAO) {
    return float3(ao, ao, ao);
  }
  float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
  return color * ao;
}

technique XeniaAO {
  pass AOGen {
    VertexShader = PostProcessVS;
    PixelShader = PS_AOGen;
    RenderTarget = XeniaAOTexA;
  }
  pass BlurH {
    VertexShader = PostProcessVS;
    PixelShader = PS_BlurH;
    RenderTarget = XeniaAOTexB;
  }
  pass Apply {
    VertexShader = PostProcessVS;
    PixelShader = PS_Apply;
  }
}

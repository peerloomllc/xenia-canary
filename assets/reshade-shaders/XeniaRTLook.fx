// A "ray-traced look" for bright titles like Lost Odyssey, in one shader:
// tames the highlights with a filmic roll-off, deepens light separation
// with local contrast, bleeds real highlights softly (bloom), adds a light
// vignette and finishes with an edge-aware sharpen. Tuned defaults for
// Lost Odyssey; every value is a slider in the overlay.
#include "ReShade.fxh"

uniform float Exposure <
    ui_type = "slider"; ui_label = "Exposure";
    ui_min = 0.5; ui_max = 1.5;
> = 0.88;

uniform float HighlightTame <
    ui_type = "slider"; ui_label = "Highlight taming";
    ui_min = 0.0; ui_max = 1.0;
> = 0.7;

uniform float ShadowDepth <
    ui_type = "slider"; ui_label = "Shadow depth";
    ui_min = 0.0; ui_max = 1.0;
> = 0.35;

uniform float Clarity <
    ui_type = "slider"; ui_label = "Light separation (clarity)";
    ui_min = 0.0; ui_max = 1.0;
> = 0.4;

uniform float BloomThreshold <
    ui_type = "slider"; ui_label = "Bloom threshold";
    ui_min = 0.0; ui_max = 1.0;
> = 0.72;

uniform float BloomAmount <
    ui_type = "slider"; ui_label = "Bloom amount";
    ui_min = 0.0; ui_max = 1.5;
> = 0.4;

uniform float Vibrance <
    ui_type = "slider"; ui_label = "Vibrance";
    ui_min = 0.0; ui_max = 0.6;
> = 0.18;

uniform float VignetteAmount <
    ui_type = "slider"; ui_label = "Vignette";
    ui_min = 0.0; ui_max = 0.6;
> = 0.22;

uniform float SharpenAmount <
    ui_type = "slider"; ui_label = "Sharpen";
    ui_min = 0.0; ui_max = 1.0;
> = 0.5;

texture XeniaRTLookTexA { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
texture XeniaRTLookTexB { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
sampler XeniaRTLookSamplerA { Texture = XeniaRTLookTexA; };
sampler XeniaRTLookSamplerB { Texture = XeniaRTLookTexB; };

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float3 PS_Downsample(float4 vpos : SV_Position, float2 texcoord : TEXCOORD)
    : SV_Target {
  return tex2D(ReShade::BackBuffer, texcoord).rgb;
}

float3 Blur(sampler s, float2 texcoord, float2 direction) {
  float2 step = direction * 2.0 / float2(BUFFER_WIDTH, BUFFER_HEIGHT);
  float3 color = tex2D(s, texcoord).rgb * 0.2270270270;
  color += tex2D(s, texcoord + step * 1.3846153846).rgb * 0.3162162162;
  color += tex2D(s, texcoord - step * 1.3846153846).rgb * 0.3162162162;
  color += tex2D(s, texcoord + step * 3.2307692308).rgb * 0.0702702703;
  color += tex2D(s, texcoord - step * 3.2307692308).rgb * 0.0702702703;
  return color;
}

float3 PS_BlurH(float4 vpos : SV_Position, float2 texcoord : TEXCOORD)
    : SV_Target {
  return Blur(XeniaRTLookSamplerA, texcoord, float2(1.0, 0.0));
}

float3 PS_BlurV(float4 vpos : SV_Position, float2 texcoord : TEXCOORD)
    : SV_Target {
  return Blur(XeniaRTLookSamplerB, texcoord, float2(0.0, 1.0));
}

float3 PS_Composite(float4 vpos : SV_Position, float2 texcoord : TEXCOORD)
    : SV_Target {
  float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb * Exposure;
  float3 blurred = tex2D(XeniaRTLookSamplerA, texcoord).rgb * Exposure;

  // Filmic-style highlight roll-off: linear below the knee, compressed
  // above it, so the sky and bright stone stop clipping toward white.
  float3 over = max(color - 0.65, 0.0);
  float3 rolled = color - over + over / (1.0 + 2.2 * over);
  color = lerp(color, rolled, HighlightTame);

  // Deepen the shadows a touch (a gamma-like toe below mid grey).
  float luma = dot(color, kLumaWeights);
  float toe = smoothstep(0.45, 0.0, luma);
  color *= 1.0 - ShadowDepth * 0.35 * toe;

  // Local contrast against the blurred copy: light areas separate from
  // shade the way bounced-light rendering reads.
  float blurred_luma = dot(blurred, kLumaWeights);
  float detail = dot(color, kLumaWeights) - blurred_luma;
  color *= 1.0 + Clarity * detail * 2.0;

  // Soft bleed around genuine highlights.
  float3 bright = max(blurred - BloomThreshold, 0.0);
  color += bright * BloomAmount;

  // Vibrance: boost muted colours more than already-saturated ones.
  float maxc = max(color.r, max(color.g, color.b));
  float minc = min(color.r, min(color.g, color.b));
  float sat = maxc - minc;
  color = lerp(color, lerp(dot(color, kLumaWeights).xxx, color, 1.0 + Vibrance),
               1.0 - sat);

  // Light corner fall-off.
  float2 offset = texcoord - 0.5;
  float falloff = smoothstep(0.25, 0.7, dot(offset, offset) * 2.2);
  color *= 1.0 - VignetteAmount * falloff;

  return saturate(color);
}

float3 PS_Sharpen(float4 vpos : SV_Position, float2 texcoord : TEXCOORD)
    : SV_Target {
  float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
  float3 center = tex2D(ReShade::BackBuffer, texcoord).rgb;
  float3 ring = tex2D(ReShade::BackBuffer, texcoord + float2(px.x, 0)).rgb +
                tex2D(ReShade::BackBuffer, texcoord - float2(px.x, 0)).rgb +
                tex2D(ReShade::BackBuffer, texcoord + float2(0, px.y)).rgb +
                tex2D(ReShade::BackBuffer, texcoord - float2(0, px.y)).rgb;
  float3 detail = center - ring * 0.25;
  // Clamp the added detail so edges sharpen without halos.
  detail = clamp(detail, -0.06, 0.06);
  return saturate(center + detail * SharpenAmount * 2.0);
}

technique XeniaRTLook {
  pass Downsample {
    VertexShader = PostProcessVS;
    PixelShader = PS_Downsample;
    RenderTarget = XeniaRTLookTexA;
  }
  pass BlurH {
    VertexShader = PostProcessVS;
    PixelShader = PS_BlurH;
    RenderTarget = XeniaRTLookTexB;
  }
  pass BlurV {
    VertexShader = PostProcessVS;
    PixelShader = PS_BlurV;
    RenderTarget = XeniaRTLookTexA;
  }
  pass Composite {
    VertexShader = PostProcessVS;
    PixelShader = PS_Composite;
  }
  pass Sharpen {
    VertexShader = PostProcessVS;
    PixelShader = PS_Sharpen;
  }
}

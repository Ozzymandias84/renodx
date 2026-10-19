#include "./shared.h"
#include "./tonemap.hlsli"

// Original shader: tonemap + colour grade (draw 259 of the late-frame chain).
// Reads the RGBA16F scene, the bloom buffer, a 256x1 tone curve table and a 16x16x16 grading LUT.
// Writes an RGBA8 image with luma in alpha for the FXAA pass that follows.

SamplerState SourceTexSampler_sampler_s : register(s0);
SamplerState BloomTexSampler_sampler_s : register(s2);
SamplerState ToneMapTableSampler_sampler_s : register(s3);
SamplerState LUTTexSampler_sampler_s : register(s5);
Texture2D<float4> SourceTexSampler_texture : register(t0);
Texture2D<float4> BloomTexSampler_texture : register(t2);
Texture2D<float4> ToneMapTableSampler_texture : register(t3);
Texture3D<float4> LUTTexSampler_texture : register(t5);

float4 main(float4 v0 : SV_Position0, float2 v1 : TEXCOORD0) : SV_Target0 {
  float3 bloom = BloomTexSampler_texture.Sample(BloomTexSampler_sampler_s, v1.xy).xyz;
  float3 scene = SourceTexSampler_texture.Sample(SourceTexSampler_sampler_s, v1.xy).xyz;

  float3 curve_input = scene / (scene + 0.1f);

  float3 curve_output;
  curve_output.x = ToneMapTableSampler_texture.Sample(ToneMapTableSampler_sampler_s, float2(curve_input.x, 0)).x;
  curve_output.y = ToneMapTableSampler_texture.Sample(ToneMapTableSampler_sampler_s, float2(curve_input.y, 0)).y;
  curve_output.z = ToneMapTableSampler_texture.Sample(ToneMapTableSampler_sampler_s, float2(curve_input.z, 0)).z;

  // Bloom slider (100% = the game). The bloom buffer already contains each bright pixel's own light,
  // so scaling it everywhere would also dim or brighten the objects themselves. The gain therefore fades out
  // as the pixel's own curve value gets bright, and only the halo around objects is scaled.
  float own_brightness = max(curve_output.x, max(curve_output.y, curve_output.z)) * 2.f;
  bloom *= lerp(CUSTOM_BLOOM, 1.f, smoothstep(0.3f, 0.8f, own_brightness));

  // Display-encoded colour the vanilla shader feeds to the grading LUT.
  // Values above 1.0 are clamped by the LUT sampler, which is where highlights are lost.
  float3 lut_input = curve_output * 2.f + bloom;

  float3 graded_encoded = LUTTexSampler_texture.Sample(LUTTexSampler_sampler_s, lut_input * 0.9375f + 0.03125f).xyz;

  float3 output_color;
  if (RENODX_TONE_MAP_TYPE == 0.f) {
    output_color = renodx::draw::RenderIntermediatePass(renodx::color::srgb::DecodeSafe(graded_encoded));
  } else {
    // The tone table squashes the float scene above its own SDR white point into a few percent of output range.
    // Find the curve input where vanilla output reaches SDR white (2 * table == 1), per channel.
    float3 search_low = 0.f;
    float3 search_high = 1.f;
    [unroll] for (uint step_index = 0; step_index < 10u; step_index++) {
      float3 search_mid = (search_low + search_high) * 0.5f;
      float3 table_mid;
      table_mid.x = ToneMapTableSampler_texture.SampleLevel(ToneMapTableSampler_sampler_s, float2(search_mid.x, 0), 0).x;
      table_mid.y = ToneMapTableSampler_texture.SampleLevel(ToneMapTableSampler_sampler_s, float2(search_mid.y, 0), 0).y;
      table_mid.z = ToneMapTableSampler_texture.SampleLevel(ToneMapTableSampler_sampler_s, float2(search_mid.z, 0), 0).z;
      float3 below_white = (float3)(table_mid * 2.f < 1.f);
      search_low = lerp(search_low, search_mid, below_white);
      search_high = lerp(search_mid, search_high, below_white);
    }
    float3 white_curve_input = clamp((search_low + search_high) * 0.5f, 0.01f, 0.999f);
    float3 scene_white = 0.1f * white_curve_input / (1.f - white_curve_input);  // inverse of scene / (scene + 0.1)

    // neutral_sdr / graded_sdr: what vanilla would have displayed without / with the grade.
    float3 neutral_sdr = renodx::color::srgb::DecodeSafe(saturate(lut_input));
    float3 graded_sdr = renodx::color::srgb::DecodeSafe(graded_encoded);

    // untonemapped: the larger of the curve + bloom value before the LUT clamp, and the float scene
    // normalised so the table's SDR white is 1.0. Never below neutral_sdr, so the grade upgrade only adds range.
    float3 curve_linear = renodx::color::srgb::DecodeSafe(max(0.f, lut_input));
    float3 scene_linear = max(0.f, scene) / scene_white + renodx::color::srgb::DecodeSafe(bloom);
    float3 untonemapped = max(neutral_sdr, max(curve_linear, scene_linear));

    float peak = RENODX_PEAK_WHITE_NITS / RENODX_DIFFUSE_WHITE_NITS;
    if (RENODX_GAMMA_CORRECTION != 0.f) {
      peak = renodx::color::correct::Gamma(
          peak, RENODX_GAMMA_CORRECTION > 0.f, abs(RENODX_GAMMA_CORRECTION) == 1.f ? 2.2f : 2.4f);
    }

    // Vanilla+: the extended signal carries the game's grade, then user grading with the per-channel hue shift / blowout
    // emulation, then a Hermite spline roll-off of the brightest channel to the display peak.
    float3 extended_graded = renodx::draw::ComputeUntonemappedGraded(untonemapped, graded_sdr, neutral_sdr);
    // Hue shift / blowout reference: the game's real per-channel clipped image at the extended pixel's luminance, so only its
    // colour (hue, bleaching) is compared.
    float3 per_channel_reference = graded_sdr * renodx::math::DivideSafe(
                                                    renodx::color::y::from::BT709(extended_graded),
                                                    renodx::color::y::from::BT709(graded_sdr),
                                                    1.f);
    float3 graded = ApplyVanillaPlusGrading(extended_graded, per_channel_reference);

    float3 tonemapped;
    if (peak > 1.f) {
      tonemapped = ApplyHermiteSplineByMaxChannel(graded, RENODX_DIFFUSE_WHITE_NITS, peak * RENODX_DIFFUSE_WHITE_NITS);
    } else {
      float graded_max_channel = max(graded.r, max(graded.g, graded.b));  // SDR output: plain clip like the game
      tonemapped = graded * renodx::math::DivideSafe(min(graded_max_channel, 1.f), graded_max_channel, 1.f);
    }
    output_color = renodx::draw::RenderIntermediatePass(tonemapped);
  }

  return float4(output_color, renodx::color::luma::from::BT601(max(0, output_color)));
}

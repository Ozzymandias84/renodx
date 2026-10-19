#include "./shared.h"

// Vanilla+ helpers, ported from the Dark Souls Remastered mod (src/games/darksouls/tonemap.hlsli).
// The signal is the game's own tone curve extended past SDR white (1.0 = SDR white), with the game's colour grade carried over.

// The game's tone curve works on each channel separately, so bright colours bend toward a different hue and bleach toward white.
// Hue Shift and Per Channel Blowout pull the colour toward `per_channel_reference`, the game's real clipped SDR image.

float3 ApplyVanillaPlusGrading(float3 ungraded, float3 per_channel_reference) {
  renodx::color::grade::Config grade_config = renodx::color::grade::config::Create();
  grade_config.exposure = RENODX_TONE_MAP_EXPOSURE;
  grade_config.highlights = RENODX_TONE_MAP_HIGHLIGHTS;
  grade_config.shadows = RENODX_TONE_MAP_SHADOWS;
  grade_config.contrast = RENODX_TONE_MAP_CONTRAST;
  grade_config.flare = 0.10f * pow(RENODX_TONE_MAP_FLARE, 10.f);
  grade_config.saturation = RENODX_TONE_MAP_SATURATION;
  grade_config.dechroma = RENODX_TONE_MAP_BLOWOUT;
  grade_config.hue_correction_strength = RENODX_TONE_MAP_HUE_SHIFT;
  grade_config.blowout = -1.f * (RENODX_TONE_MAP_HIGHLIGHT_SATURATION - 1.f);

  const float mid_gray = 0.18f;
  const float y = renodx::color::y::from::BT709(ungraded);
  float3 color = ungraded;

  if (grade_config.exposure != 1.f || grade_config.shadows != 1.f || grade_config.highlights != 1.f
      || grade_config.contrast != 1.f || grade_config.flare != 0.f) {
    color *= grade_config.exposure;

    const float y_normalized = y / mid_gray;
    const float flare = renodx::math::DivideSafe(y_normalized + grade_config.flare, y_normalized, 1.f);
    const float y_contrasted = pow(y_normalized, grade_config.contrast * flare) * mid_gray;

    float y_highlighted = y_contrasted;
    if (grade_config.highlights > 1.f) {
      y_highlighted = max(y_contrasted, lerp(y_contrasted, mid_gray * pow(y_contrasted / mid_gray, grade_config.highlights), min(y_contrasted, 1.f)));
    } else if (grade_config.highlights < 1.f) {
      const float highlights_input = y_contrasted / mid_gray;
      y_highlighted = lerp(highlights_input, pow(highlights_input, grade_config.highlights), step(1.f, highlights_input)) * mid_gray;
    }

    float y_shadowed = y_highlighted;
    if (grade_config.shadows != 1.f) {
      const float shadows_ratio = max(renodx::math::DivideSafe(y_highlighted, mid_gray, 0.f), 0.f);
      const float shadows_base = y_highlighted * mid_gray;
      const float shadows_reference_scale = renodx::math::DivideSafe(shadows_base, shadows_ratio, 0.f);
      if (grade_config.shadows > 1.f) {
        const float raised = y_highlighted * (1.f + renodx::math::DivideSafe(shadows_base, pow(shadows_ratio, grade_config.shadows), 0.f));
        const float reference = y_highlighted * (1.f + shadows_reference_scale);
        y_shadowed = max(y_highlighted, y_highlighted + (raised - reference));
      } else {
        const float lowered = y_highlighted * (1.f - renodx::math::DivideSafe(shadows_base, pow(shadows_ratio, 2.f - grade_config.shadows), 0.f));
        const float reference = y_highlighted * (1.f - shadows_reference_scale);
        y_shadowed = clamp(y_highlighted + (lowered - reference), 0.f, y_highlighted);
      }
    }

    color = renodx::color::correct::Luminance(color, y, y_shadowed);
  }

  if (grade_config.saturation != 1.f || grade_config.dechroma != 0.f || grade_config.hue_correction_strength != 0.f
      || grade_config.blowout != 0.f || RENODX_TONE_MAP_PER_CHANNEL_BLOWOUT != 0.f) {
    float3 perceptual_new = renodx::color::oklab::from::BT709(color);

    if (grade_config.hue_correction_strength != 0.f || RENODX_TONE_MAP_PER_CHANNEL_BLOWOUT != 0.f) {
      const float3 reference_oklab = renodx::color::oklab::from::BT709(
          per_channel_reference);

      float chrominance_current = length(perceptual_new.yz);
      float chrominance_ratio = 1.f;

      if (grade_config.hue_correction_strength != 0.f) {
        const float chrominance_pre = chrominance_current;
        perceptual_new.yz = lerp(perceptual_new.yz, reference_oklab.yz, grade_config.hue_correction_strength);
        chrominance_current = length(perceptual_new.yz);
        chrominance_ratio = renodx::math::SafeDivision(chrominance_pre, chrominance_current, 1);
      }

      if (RENODX_TONE_MAP_PER_CHANNEL_BLOWOUT != 0.f) {
        const float target_chrominance_ratio = renodx::math::SafeDivision(length(reference_oklab.yz), chrominance_current, 1);
        chrominance_ratio = lerp(chrominance_ratio, target_chrominance_ratio, RENODX_TONE_MAP_PER_CHANNEL_BLOWOUT);
      }
      perceptual_new.yz *= chrominance_ratio;
    }

    if (grade_config.dechroma != 0.f) {
      perceptual_new.yz *= lerp(1.f, 0.f, saturate(pow(y / (10000.f / 100.f), (1.f - grade_config.dechroma))));
    }

    if (grade_config.blowout != 0.f) {
      // positive = 1 to 0, negative = 1 to 2
      const float percent_max = saturate(y * 100.f / 10000.f);
      float blowout_change = pow(1.f - percent_max, 100.f * abs(grade_config.blowout));
      if (grade_config.blowout < 0.f) {
        blowout_change = (2.f - blowout_change);
      }
      perceptual_new.yz *= blowout_change;
    }

    perceptual_new.yz *= grade_config.saturation;

    color = renodx::color::bt709::from::OkLab(perceptual_new);
    color = renodx::color::bt709::clamp::AP1(color);
  }

  return color;
}

// Rolls the signal off to the display peak in PQ space by scaling all channels by the same factor, so hue is kept.
float3 ApplyHermiteSplineByMaxChannel(float3 input, float diffuse_nits, float peak_nits) {
  input = max(0, renodx::color::bt2020::from::BT709(input));

  const float peak_ratio = peak_nits / diffuse_nits;
  const float white_clip = max(100.f, peak_ratio * 1.5f);  // safeguard to prevent artifacts

  const float max_channel = renodx::math::Max(input.r, input.g, input.b);

  const float max_pq = renodx::color::pq::Encode(max_channel, diffuse_nits);
  const float target_white_pq = renodx::color::pq::Encode(peak_nits, 1.f);
  const float max_white_pq = renodx::color::pq::Encode(white_clip, diffuse_nits);
  const float target_black_pq = renodx::color::pq::Encode(0.0001f, 1.f);
  const float min_black_pq = renodx::color::pq::Encode(0.f, 1.f);

  const float scaled_pq = renodx::tonemap::HermiteSplineRolloff(max_pq, target_white_pq, max_white_pq, target_black_pq, min_black_pq);

  const float mapped_max = min(renodx::color::pq::Decode(scaled_pq, diffuse_nits), peak_ratio);

  const float scale = renodx::math::DivideSafe(mapped_max, max_channel, 0.f);
  return renodx::color::bt709::from::BT2020(input * scale);
}

// The present handler: the per-frame bookkeeping, and the latch of the render scale and the jitter for the next frame.
#pragma once

#include "ds2_developer.hpp"

namespace ds2 {

constexpr int DEFAULT_JITTER_PHASES = 16;

// Latches the render scale and the jitter for the next frame (every pass of a frame sees the same scale).
inline void LatchRenderScale() {
  const int previous_mode = latched_mode;
  latched_mode = std::clamp(settings.mode, 0, UPSCALE_MODE_COUNT - 1);
  // A change between two scaled modes leaves the other mode's picture around the new render rectangle in the game's textures;
  // the effects that read across its border show it as a soft line at the right and bottom edge until a full-size frame
  // overwrites it. Two frames are therefore drawn at full size (without the upscaler) when such a switch happens.
  if (previous_mode != latched_mode && previous_mode > 0 && latched_mode > 0 && settings.enabled) upscale_hold_frames = 2;
  frame_holding = (upscale_hold_frames > 0);
  if (frame_holding) upscale_hold_frames--;
  scaling_active = (settings.enabled && !frame_holding && latched_mode > 0 && back_buffer_width != 0 && back_buffer_height != 0);
  int jitter_phase_count = DEFAULT_JITTER_PHASES;
  if (scaling_active) {
    const float ratio = UPSCALE_MODES[latched_mode].ratio;
    render_width = std::max(16u, static_cast<uint32_t>(std::lround(static_cast<float>(back_buffer_width) * ratio)));
    render_height = std::max(16u, static_cast<uint32_t>(std::lround(static_cast<float>(back_buffer_height) * ratio)));
    scale_x = static_cast<float>(render_width) / static_cast<float>(back_buffer_width);
    scale_y = static_cast<float>(render_height) / static_cast<float>(back_buffer_height);
    // NVIDIA recommends 8 * (output / render)^2 phases so the jitter covers the output pixels.
    jitter_phase_count = std::clamp(static_cast<int>(std::ceil(8.f / (ratio * ratio))), 8, 64);
  } else {
    render_width = back_buffer_width;
    render_height = back_buffer_height;
    scale_x = 1.f;
    scale_y = 1.f;
  }
  uv_scale_values = {scale_x, scale_y};
  frame_scaled = (scaling_active && scene_frame_hint);
  if (previous_mode != latched_mode) reset_history_request = true;
  mip_bias = (scaling_active ? std::log2(UPSCALE_MODES[latched_mode].ratio) + (settings.upscaler == 1 ? tuning.fsr_mip_bias_offset : tuning.mip_bias_offset)
                             : 0.f);

  frame_number++;
  // Offsets in [-0.5, 0.5] pixels from the Halton (2, 3) sequence, applied to the next frame's scene passes.
  const uint32_t sample_index = (frame_number % static_cast<uint32_t>(jitter_phase_count)) + 1;
  jitter_phases = jitter_phase_count;
  jitter_x = Halton(sample_index, 2) - 0.5f;
  jitter_y = Halton(sample_index, 3) - 0.5f;
}

inline void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain,
                      const reshade::api::rect* source_rect, const reshade::api::rect* dest_rect,
                      uint32_t dirty_rect_count, const reshade::api::rect* dirty_rects) {
#if DS2_PROFILE
  ProfileFrame();
#endif
  DS2_LOCK_OR_RETURN();
  FlushDumps(queue);
  if (frame_number % 30u == 0u) {
    PollConfigFile();
    WriteStatusFile();
  }

  const auto back_buffer_desc = queue->get_device()->get_resource_desc(swapchain->get_current_back_buffer());
  back_buffer_width = back_buffer_desc.texture.width;
  back_buffer_height = back_buffer_desc.texture.height;

  static const char* last_logged_status = "";
  if (frame_scaled && last_upscaled_frame != frame_number && draws_this_frame > 0 && status_text != last_logged_status) {
    last_logged_status = status_text;
    LogMessage(reshade::log::level::warning, std::string("scaled frame without upscaling: ") + status_text);
  }
  if (draws_this_frame > 0) scene_frame_hint = camera_captured_this_frame;
  camera_captured_this_frame = false;
  upscaled_this_frame = false;
  // The status line must follow what this frame did: a disabled add-on, or a frame without a 3D scene (menu, loading).
  if (!settings.enabled) {
    status_text = "Upscaler disabled";
    status_running = false;
  } else if (status_running && last_upscaled_frame != frame_number) {
    status_text = "Waiting for a 3D scene";
    status_running = false;
  }
  hash_occurrences.Clear();
  HudEndFrame();
  scaled_written_resources.clear();
  last_draw_ps_hash = 0;
  last_draw_ordinal = 0;
  draws_this_frame = 0;
  LatchRenderScale();
}

}  // namespace ds2

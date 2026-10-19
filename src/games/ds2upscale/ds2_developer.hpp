// Developer remote control, for builds with DS2_DEVELOPER = 1: ds2upscale.cfg next to the game executable (key=value lines, re-read
// when it changes) overrides the settings and the tuning values, and ds2upscale_status.txt is rewritten every 30 frames. Release
// builds get empty stand-ins, so they never touch the disk from the render thread.
#pragma once

#include "ds2_events.hpp"

namespace ds2 {

#if !DS2_DEVELOPER

inline void PollConfigFile() {}
inline void WriteStatusFile() {}

#else

// key=value lines. Keys that are absent keep their value; reset_defaults=1 (first line) puts everything back first.
inline void PollConfigFile() {
  static const std::filesystem::path path = ExeDirectory() / "ds2upscale.cfg";
  static std::filesystem::file_time_type last_write = {};
  std::error_code error;
  const auto write_time = std::filesystem::last_write_time(path, error);
  if (error || write_time == last_write) return;
  last_write = write_time;

  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    const auto equals = line.find('=');
    if (equals == std::string::npos || line[0] == '#') continue;
    const std::string key = line.substr(0, equals);
    const float value = std::strtof(line.c_str() + equals + 1, nullptr);
    const bool flag = (value != 0.f);
    if (key == "reset_defaults") {
      if (flag) {
        settings = Settings{};
        tuning = Tuning{};
      }
    }
    else if (key == "enabled") settings.enabled = flag;
    else if (key == "mode") settings.mode = std::clamp(static_cast<int>(value), 0, UPSCALE_MODE_COUNT - 1);
    else if (key == "upscaler") settings.upscaler = std::clamp(static_cast<int>(value), 0, 1);
    else if (key == "preset") settings.preset = static_cast<int>(value);
    else if (key == "motion_blur_off") settings.motion_blur_off = flag;
    else if (key == "disable_fxaa") settings.disable_fxaa = flag;
    else if (key == "character_sharpening") settings.character_sharpening = flag;
    else if (key == "image_sharpening") settings.image_sharpening = flag;
    else if (key == "image_sharpening_strength") settings.image_sharpening_strength = std::clamp(value, 0.f, 100.f);
    else if (key == "hud_scale") settings.hud_scale = std::max(value, 0.f);
    else if (key == "stabilize_weight") tuning.stabilize_weight = std::clamp(value, 0.f, 0.95f);
    else if (key == "mip_bias_offset") tuning.mip_bias_offset = value;
    else if (key == "fsr_mip_bias_offset") tuning.fsr_mip_bias_offset = value;
    else if (key == "character_bias") tuning.character_bias = std::clamp(value, 0.f, 1.f);
    else if (key == "fsr_sharpness") tuning.fsr_sharpness = std::clamp(value, 0.f, 1.f);
    else if (key == "fsr_character_reactive") tuning.fsr_character_reactive = std::clamp(value, 0.f, 1.f);
    else if (key == "fsr_velocity_factor") tuning.fsr_velocity_factor = std::clamp(value, 0.f, 1.f);
    else if (key == "fsr_reactiveness") tuning.fsr_reactiveness = std::max(value, 0.f);
    else if (key == "fsr_shading_change") tuning.fsr_shading_change = std::max(value, 0.f);
    else if (key == "fsr_accumulation") tuning.fsr_accumulation = std::clamp(value, 0.f, 1.f);
    else if (key == "fsr_min_disocclusion") tuning.fsr_min_disocclusion = std::clamp(value, -1.f, 1.f);
    else if (key == "fsr_foliage_reactive") tuning.fsr_foliage_reactive = std::clamp(value, 0.f, 1.f);
    else if (key == "exposure") tuning.exposure = value;
    else if (key == "camera_cut_distance") tuning.camera_cut_distance = value;
    else if (key == "hud_zone") tuning.hud_zone = std::clamp(value, 0.f, 1.f);
    else if (key == "hud_zone_y") tuning.hud_zone_y = std::clamp(value, 0.f, 1.f);
    else if (key == "dump_frames" && value > 0.f) dump_frames_requested = static_cast<int>(value);
    else if (key == "reset_history" && flag) reset_history_request = true;
  }
  LogMessage(reshade::log::level::info, "ds2upscale.cfg applied");
}

inline void WriteStatusFile() {
  static const std::filesystem::path path = ExeDirectory() / "ds2upscale_status.txt";
  std::ofstream file(path, std::ios::trunc);
  if (!file) return;
  file << "frame=" << frame_number << "\n";
  file << "status=" << status_text << "\n";
  file << "last_ngx_message=" << last_ngx_message << "\n";
  file << "frames_upscaled=" << frames_upscaled << " last_upscaled_frame=" << last_upscaled_frame
       << " evaluate_failures=" << evaluate_failures << " lock_timeouts=" << lock_timeouts.load() << "\n";
  file << "gpu_ms: total=" << gpu_timer.averages[0] << " prep=" << gpu_timer.averages[1] << " backend=" << gpu_timer.averages[2]
       << " post=" << gpu_timer.averages[3] << " window=" << gpu_timer.samples << "/120\n";
#if DS2_PROFILE
  if (!profile_text.empty()) file << profile_text << "\n";
#endif
  file << "upscaler=" << (settings.upscaler == 1 ? "FSR 3.1" : "DLSS") << " fsr_resources=" << fsr.created
       << " fsr_memory_mb=" << (fsr.memory_bytes >> 20) << " fsr_message=" << fsr.message << "\n";
  file << "dlss: sdk_initialized=" << dlss.sdk_initialized << " feature=" << (dlss.feature != nullptr)
       << " size=" << dlss.feature_width << "x" << dlss.feature_height << " failed_attempts=" << dlss.failed_attempts << "\n";
  file << "back_buffer=" << back_buffer_width << "x" << back_buffer_height << "\n";
  file << "mode=" << settings.mode << " (" << ModeName(latched_mode) << ") scaling_active=" << scaling_active
       << " render=" << render_width << "x" << render_height << " scale=" << scale_x << "," << scale_y
       << " scene_hint=" << scene_frame_hint << " frame_scaled=" << frame_scaled << "\n";
  file << "jitter_offset=" << jitter_x << " " << jitter_y << " camera_motion_px=" << camera_motion_pixels << "\n";
  file << "settings: enabled=" << settings.enabled << " mode=" << settings.mode << " preset=" << settings.preset
       << " motion_blur_off=" << settings.motion_blur_off << " disable_fxaa=" << settings.disable_fxaa
       << " character_sharpening=" << settings.character_sharpening << " engine_velocity=" << (engine_velocity_resource.handle != 0)
       << " image_sharpening=" << settings.image_sharpening << " strength=" << settings.image_sharpening_strength << "\n";
  file << "tuning: stabilize_weight=" << tuning.stabilize_weight << " mip_bias_offset=" << tuning.mip_bias_offset
       << " character_bias=" << tuning.character_bias << " exposure=" << tuning.exposure
       << " camera_cut=" << tuning.camera_cut_distance << "\n";
  file << "hud: scale=" << settings.hud_scale << " zone=" << tuning.hud_zone << " clusters=" << hud_cluster_count << "\n";
  file << "mip_bias=" << mip_bias << " twins=" << sampler_twins.size() << " known_samplers=" << sampler_descs.size() << "\n";
  file << "camera_valid=" << camera_current.valid << " camera_pos=" << camera_current.position[0] << " "
       << camera_current.position[1] << " " << camera_current.position[2] << "\n";
}

#endif  // DS2_DEVELOPER

}  // namespace ds2

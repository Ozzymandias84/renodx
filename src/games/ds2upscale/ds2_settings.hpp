// Settings. `Settings` is what the overlay shows and what is saved in ReShade.ini ([DS2Upscale]); `Tuning` holds the
// behaviour that is not in the overlay, with the values the project settled on (a developer build can change them live
// through ds2upscale.cfg, a release build compiles them in as constants).
#pragma once

#include "ds2_constants.hpp"

namespace ds2 {

// The defaults are what a first-time user gets: FSR 3.1 at native resolution (FSR runs on every GPU; DLSS needs an RTX card).
struct Settings {
  bool enabled = true;                     // Enable the upscaler (DLSS or FSR)
  int mode = 0;                            // index into UPSCALE_MODES (0 = native resolution with the upscaler's anti-aliasing)
  int upscaler = 1;                        // 0 = DLSS, 1 = FSR 3.1
  int preset = 13;                         // NVSDK_NGX_DLSS_Hint_Render_Preset: 0 default, 10 J, 11 K, 12 L, 13 M
  bool motion_blur_off = true;             // replace the game's motion blur by a passthrough
  bool disable_fxaa = true;                // skip the game's FXAA pass while the upscaler is on (it would blur the upscaled image)
  bool character_sharpening = false;       // texture sharpening (negative mip bias) on characters too: may ghost
  bool image_sharpening = false;           // sharpening of the upscaled image (CAS for DLSS, RCAS for FSR)
  float image_sharpening_strength = 20.f;  // 0 .. 100
  float hud_scale = 1.f;                   // HUD size: 1 = game default, >= 0 (typed values above the slider range work)
};

struct Tuning {
  float stabilize_weight = 0.7f;        // weight of the previous frame in the temporal smoothing of the depth of field and sun ray maps
  float mip_bias_offset = -1.f;         // DLSS: texture LOD bias while upscaling = log2(render / output) + offset (NVIDIA recommends -1)
  float fsr_mip_bias_offset = 0.f;      // the same for FSR (softer textures alias less; AMD suggests -1)
  float character_bias = 0.5f;          // DLSS "bias current colour" strength on moving objects (0 = off)
  float fsr_sharpness = 0.1f;           // FSR: RCAS sharpness 0..1 when the overlay image sharpening is off (0 = no sharpening pass)
  float fsr_character_reactive = 0.8f;  // FSR: reactive mask value on moving objects (stencil bit): less history, less ghosting
  float fsr_velocity_factor = 1.f;
  float fsr_reactiveness = 1.f;
  float fsr_shading_change = 1.f;
  float fsr_accumulation = 1.f / 3.f;   // accumulation added per frame (lower = less ghosting, may flicker thin detail)
  float fsr_min_disocclusion = 1.f;     // minimum accumulation around disocclusions (AMD default -1/3): 1.0 = least flicker, no ghosting seen
  float fsr_foliage_reactive = 0.9f;    // FSR: reactive mask value on wind-animated foliage
  float exposure = 0.5f;                // fixed exposure handed to DLSS (presets L and M use their own auto exposure)
  float camera_cut_distance = 5.f;      // camera jump (world units) that resets the upscaler history
  float hud_zone = 0.3f;                // HUD scaling: an element whose centre is further than this from the screen centre (NDC) keeps its edge
  float hud_zone_y = 0.15f;             // same, vertically (the floor message dialog sits at y -0.39 and belongs to the bottom edge)
};

inline Settings settings;
// The first mode is native resolution with the upscaler's own anti-aliasing: DLAA for DLSS.
inline const char* ModeName(int mode) { return (mode == 0 && settings.upscaler == 1) ? "Native (FSR 3.1 AA)" : UPSCALE_MODES[mode].name; }
#if DS2_DEVELOPER
inline Tuning tuning;
#else
inline constexpr Tuning tuning{};
#endif

constexpr const char* CONFIG_SECTION = "DS2Upscale";

inline void LoadSettings() {
  Settings loaded;
  reshade::get_config_value(nullptr, CONFIG_SECTION, "Enabled", loaded.enabled);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "Mode", loaded.mode);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "Upscaler", loaded.upscaler);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "Preset", loaded.preset);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "MotionBlurOff", loaded.motion_blur_off);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "DisableFxaa", loaded.disable_fxaa);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "CharacterSharpening", loaded.character_sharpening);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "ImageSharpening", loaded.image_sharpening);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "ImageSharpeningStrength", loaded.image_sharpening_strength);
  reshade::get_config_value(nullptr, CONFIG_SECTION, "HudScale", loaded.hud_scale);
  loaded.mode = std::clamp(loaded.mode, 0, UPSCALE_MODE_COUNT - 1);
  loaded.upscaler = std::clamp(loaded.upscaler, 0, 1);
  loaded.image_sharpening_strength = std::clamp(loaded.image_sharpening_strength, 0.f, 100.f);
  if (!(loaded.hud_scale >= 0.f)) loaded.hud_scale = Settings{}.hud_scale;  // also rejects NaN
  bool known_preset = false;
  for (const int value : DLSS_PRESET_VALUES) known_preset |= (value == loaded.preset);
  if (!known_preset) loaded.preset = Settings{}.preset;
  settings = loaded;
}

inline void SaveSettings() {
  reshade::set_config_value(nullptr, CONFIG_SECTION, "Enabled", settings.enabled);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "Mode", settings.mode);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "Upscaler", settings.upscaler);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "Preset", settings.preset);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "MotionBlurOff", settings.motion_blur_off);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "DisableFxaa", settings.disable_fxaa);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "CharacterSharpening", settings.character_sharpening);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "ImageSharpening", settings.image_sharpening);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "ImageSharpeningStrength", settings.image_sharpening_strength);
  reshade::set_config_value(nullptr, CONFIG_SECTION, "HudScale", settings.hud_scale);
}

}  // namespace ds2

/*
 * Copyright (C) 2024 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include <optional>
#include <string>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/date.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

constexpr float OUTPUT_MODE_SDR = 0.f;
constexpr float OUTPUT_MODE_HDR10 = 1.f;

renodx::mods::shader::CustomShaders custom_shaders = {__ALL_CUSTOM_SHADERS};

ShaderInjectData shader_injection;

renodx::utils::settings::Setting* output_mode_setting = nullptr;
renodx::utils::settings::Setting* peak_nits_setting = nullptr;
renodx::utils::settings::Setting* game_nits_setting = nullptr;
renodx::utils::settings::Setting* ui_nits_setting = nullptr;
renodx::utils::settings::Setting* gamma_correction_setting = nullptr;

reshade::api::swapchain* tracked_swapchain = nullptr;
std::optional<reshade::api::color_space> next_color_space = std::nullopt;

// SDR mode keeps the upgraded pipeline but collapses every nit setting to 1 so the output is plain sRGB.
void HandleOutputModeChange() {
  if (output_mode_setting->GetValue() == OUTPUT_MODE_SDR) {
    next_color_space = reshade::api::color_space::srgb_nonlinear;
    shader_injection.swap_chain_output_preset = 0.f;
    shader_injection.peak_white_nits = 1.f;
    shader_injection.diffuse_white_nits = 1.f;
    shader_injection.graphics_white_nits = 1.f;
    shader_injection.gamma_correction = 0.f;
  } else {
    next_color_space = reshade::api::color_space::hdr10_st2084;
    shader_injection.swap_chain_output_preset = 1.f;
    shader_injection.peak_white_nits = peak_nits_setting->GetValue();
    shader_injection.diffuse_white_nits = game_nits_setting->GetValue();
    shader_injection.graphics_white_nits = ui_nits_setting->GetValue();
    shader_injection.gamma_correction = gamma_correction_setting->GetValue();
  }
}

bool IsHDREnabled() { return shader_injection.swap_chain_output_preset > 0.f; }

bool IsTonemapEnabled() { return shader_injection.tone_map_type > 0.f; }


renodx::utils::settings::Settings settings = {
    output_mode_setting = new renodx::utils::settings::Setting{
        .key = "OutputMode",
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = OUTPUT_MODE_HDR10,
        .can_reset = false,
        .label = "Output Mode",
        .tooltip = "SDR keeps the game's original output. HDR enables HDR10 output.",
        .labels = {"SDR", "HDR10"},
        .on_change_value = [](float previous, float current) { HandleOutputModeChange(); },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Vanilla keeps the game's own clip. Vanilla+ extends the game's own tone mapper past white and rolls it "
                   "off to your peak with a Hermite spline, with Hue Shift and Per Channel Blowout matched to the game's "
                   "real per-channel clipped image.",
        .labels = {"Vanilla", "Vanilla+ (Default)"},
        .is_enabled = &IsHDREnabled,
    },
    peak_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 4000.f,
        .is_enabled = &IsHDREnabled,
    },
    game_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = &IsHDREnabled,
    },
    ui_nits_setting = new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
        .is_enabled = &IsHDREnabled,
    },
    gamma_correction_setting = new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .binding = &shader_injection.gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "SDR EOTF Emulation",
        .section = "Tone Mapping",
        .tooltip = "Emulates output decoding used on SDR displays.",
        .labels = {"None", "2.2", "BT.1886"},
        .is_enabled = &IsHDREnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueShift",
        .binding = &shader_injection.tone_map_hue_shift,
        .default_value = 100.f,
        .label = "Hue Shift",
        .section = "Tone Mapping",
        .tooltip = "Emulates the hue shifting of the game's per-channel tone mapping.",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapPerChannelBlowout",
        .binding = &shader_injection.tone_map_per_channel_blowout,
        .default_value = 50.f,
        .label = "Per Channel Blowout",
        .section = "Tone Mapping",
        .tooltip = "Emulates the highlight blowout of the game's per-channel tone mapping.",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_enabled = &IsTonemapEnabled,
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlightSaturation",
        .binding = &shader_injection.tone_map_highlight_saturation,
        .default_value = 50.f,
        .label = "Highlight Saturation",
        .section = "Color Grading",
        .tooltip = "Adds or removes highlight color.",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare Compensation.",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeScene",
        .binding = &shader_injection.color_grade_strength,
        .default_value = 100.f,
        .label = "Scene Grading",
        .section = "Color Grading",
        .tooltip = "Scene grading as applied by the game",
        .max = 100.f,
        .is_enabled = &IsTonemapEnabled,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "FxBloom",
        .binding = &shader_injection.custom_bloom,
        .default_value = 100.f,
        .label = "Bloom",
        .section = "Effects",
        .tooltip = "Scales the game's bloom, the soft glow around the sun and bright lights. "
                   "100% matches the game.",
        .max = 200.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .value_type = renodx::utils::settings::SettingValueType::TEXT,
        .label = std::string("Build: ") + renodx::utils::date::ISO_DATE_TIME,
        .section = "About",
    },
};

void OnPresetOff() {
  renodx::utils::settings::UpdateSettings({
      {"ToneMapType", 0.f},
      {"ToneMapPeakNits", 203.f},
      {"ToneMapGameNits", 203.f},
      {"ToneMapUINits", 203.f},
      {"ToneMapHueShift", 100.f},
      {"ToneMapPerChannelBlowout", 50.f},
      {"ColorGradeExposure", 1.f},
      {"ColorGradeHighlights", 50.f},
      {"ColorGradeShadows", 50.f},
      {"ColorGradeContrast", 50.f},
      {"ColorGradeSaturation", 50.f},
      {"ColorGradeHighlightSaturation", 50.f},
      {"ColorGradeBlowout", 0.f},
      {"ColorGradeFlare", 0.f},
      {"ColorGradeScene", 100.f},
      {"FxBloom", 100.f},
  });
}

void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  auto peak = renodx::utils::swapchain::GetPeakNits(swapchain);
  if (!peak.has_value()) return;
  peak_nits_setting->default_value = peak.value();
  peak_nits_setting->can_reset = true;
}

void OnPresent(reshade::api::command_queue* queue,
               reshade::api::swapchain* swapchain,
               const reshade::api::rect* source_rect,
               const reshade::api::rect* dest_rect,
               uint32_t dirty_rect_count,
               const reshade::api::rect* dirty_rects) {
  auto* device = queue->get_device();

  auto* data = renodx::utils::data::Get<renodx::mods::swapchain::DeviceData>(device);
  if (data == nullptr) return;
  if (!data->upgraded_swapchains.contains(swapchain)) return;

  if (tracked_swapchain != swapchain) {
    tracked_swapchain = swapchain;
    HandleOutputModeChange();
  } else if (next_color_space.has_value()) {
    renodx::utils::swapchain::ChangeColorSpace(tracked_swapchain, next_color_space.value());
    next_color_space = std::nullopt;
  }
}

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX for Dark Souls II: Scholar of the First Sin";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      renodx::mods::shader::expected_constant_buffer_index = 13;

      renodx::mods::swapchain::use_resource_cloning = true;
      renodx::mods::swapchain::prevent_full_screen = true;
      // Enables RenoDX's command-list state tracking in this (first loaded) add-on so other passes can restore state.
      renodx::mods::swapchain::swapchain_proxy_revert_state = true;
      renodx::mods::swapchain::swap_chain_proxy_vertex_shader = __swap_chain_proxy_vertex_shader;
      renodx::mods::swapchain::swap_chain_proxy_pixel_shader = __swap_chain_proxy_pixel_shader;
      renodx::mods::swapchain::SetUseHDR10();

      // The tonemap, FXAA and HUD passes all render into screen-sized RGBA8 buffers.
      renodx::mods::swapchain::resource_upgrade_infos.push_back({
          .old_format = reshade::api::format::r8g8b8a8_unorm,
          .new_format = reshade::api::format::r16g16b16a16_float,
          .use_resource_view_cloning = true,
          .usage_include = reshade::api::resource_usage::render_target,
      });

      renodx::utils::settings::on_preset_changed_callbacks.emplace_back(&HandleOutputModeChange);

      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::present>(OnPresent);

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}

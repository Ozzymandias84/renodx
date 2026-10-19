/*
 * DS2 Upscaler: NVIDIA DLSS and AMD FSR 3.1 for Dark Souls II: Scholar of the First Sin (Direct3D 11), a ReShade add-on.
 * Works next to (but independently of) the RenoDX HDR add-on: it jitters the scene passes, builds motion vectors from the
 * game's depth buffer, camera matrices and velocity map, runs DLSS or FSR 3.1 on the scene before depth of field / motion blur /
 * bloom, and copies the result back into the game's scene texture. Below output resolution the whole scene is drawn into the
 * top-left sub-rectangle of the game's render targets and the passes that read those textures are corrected.
 *
 * Layout (one translation unit, headers in dependency order):
 *   ds2_build.hpp      build switches (developer tools, CPU probes)
 *   ds2_common.hpp     includes, NGX library
 *   ds2_constants.hpp  shader hashes, upscale modes
 *   ds2_profile.hpp    CPU probes (developer builds)
 *   ds2_util.hpp       logging, hashing, jitter sequence, state lock
 *   ds2_settings.hpp   overlay settings (saved in ReShade.ini) and fixed tuning values
 *   ds2_state.hpp      what the game bound, camera, frame state, add-on GPU objects
 *   ds2_dlss.hpp       NGX lifetime and DLSS feature creation
 *   ds2_dump.hpp       frame dumps for offline validation (developer builds)
 *   ds2_timing.hpp     GPU timers (developer builds)
 *   ds2_resources.hpp  textures, replacement pipelines, camera / DoF constant readers
 *   ds2_fsr.hpp        FSR 3.1 backend: AMD's FidelityFX compute shaders (fsr3/, see THIRD_PARTY_NOTICES.md) run with D3D11 calls
 *   ds2_hud.hpp        HUD scaling
 *   ds2_passes.hpp     depth rebuild, effect stabiliser, motion vectors + upscaler + sharpening
 *   ds2_events.hpp     ReShade event handlers and per-draw overrides
 *   ds2_developer.hpp  remote control file and status file (developer builds)
 *   ds2_present.hpp    present handler
 * SPDX-License-Identifier: MIT
 */

#include "ds2_present.hpp"

namespace ds2 {

namespace {

constexpr ImVec4 ERROR_COLOR = ImVec4(1.f, 0.4f, 0.4f, 1.f);

constexpr const char* PRESET_NAMES[DLSS_PRESET_COUNT] = {"Default", "Preset J", "Preset K", "Preset L", "Preset M"};

}  // namespace

void DrawOverlay(reshade::api::effect_runtime* runtime) {
  bool changed = false;

  changed |= ImGui::Checkbox("Enable Upscaler", &settings.enabled);

  ImGui::BeginDisabled(!settings.enabled);
  constexpr const char* UPSCALER_NAMES[] = {"DLSS", "FSR 3.1"};
  if (ImGui::BeginCombo("Upscaler", UPSCALER_NAMES[std::clamp(settings.upscaler, 0, 1)])) {
    for (int i = 0; i < 2; i++) {
      if (ImGui::Selectable(UPSCALER_NAMES[i], settings.upscaler == i)) {
        settings.upscaler = i;
        reset_history_request = true;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("FSR 3.1 works on every graphics card.\n"
                      "DLSS needs an NVIDIA RTX card and nvngx_dlss.dll next to the game executable.");
  }
  if (!fsr.message.empty()) ImGui::TextColored(ERROR_COLOR, "FSR: %s", fsr.message.c_str());
  if (ImGui::BeginCombo("Upscaling mode", ModeName(std::clamp(settings.mode, 0, UPSCALE_MODE_COUNT - 1)))) {
    for (int i = 0; i < UPSCALE_MODE_COUNT; i++) {
      if (ImGui::Selectable(ModeName(i), settings.mode == i)) {
        settings.mode = i;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  if (scaling_active && render_width != 0) {
    ImGui::TextDisabled("Renders at %ux%u, outputs %ux%u", render_width, render_height, back_buffer_width,
                        back_buffer_height);
  }

  ImGui::BeginDisabled(settings.upscaler != 0);
  int preset_index = 0;
  for (int i = 0; i < DLSS_PRESET_COUNT; i++) {
    if (DLSS_PRESET_VALUES[i] == settings.preset) preset_index = i;
  }
  if (ImGui::BeginCombo("DLSS preset", PRESET_NAMES[preset_index])) {
    for (int i = 0; i < DLSS_PRESET_COUNT; i++) {
      if (ImGui::Selectable(PRESET_NAMES[i], preset_index == i)) {
        settings.preset = DLSS_PRESET_VALUES[i];
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Default lets NVIDIA choose per mode. Presets L and M manage exposure themselves;\n"
                      "the other presets use a fixed exposure of 0.5. The feature is recreated when this changes.");
  }
  ImGui::EndDisabled();  // preset (DLSS only)
  ImGui::EndDisabled();  // enabled

  ImGui::Separator();
  changed |= ImGui::Checkbox("Disable motion blur", &settings.motion_blur_off);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Hides the blur only. Keep the game's own Motion Blur option ON in its settings: its velocity data\n"
                      "gives moving characters their real motion in DLSS (less ghosting). Without it they use the camera's motion.");
  }
  changed |= ImGui::Checkbox("Disable FXAA", &settings.disable_fxaa);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Skips the game's FXAA while the upscaler is on: it would blur the upscaled image a second time.\n"
                      "Has no effect when the upscaler is off.");
  }

  changed |= ImGui::Checkbox("Texture sharpening for characters (may induce ghosting or flicker)",
                             &settings.character_sharpening);

  ImGui::Separator();
  changed |= ImGui::Checkbox("Image sharpening", &settings.image_sharpening);
  ImGui::BeginDisabled(!settings.image_sharpening);
  changed |= ImGui::SliderFloat("Sharpening strength", &settings.image_sharpening_strength, 0.f, 100.f, "%.0f");
  ImGui::EndDisabled();

  ImGui::Separator();
  // No clamp on typed input: Ctrl+click accepts any value (5 still scales); negative values are cut to 0 below.
  changed |= ImGui::SliderFloat("HUD scale", &settings.hud_scale, 0.10f, 2.f, "%.2f");

  if (changed) {
    settings.image_sharpening_strength = std::clamp(settings.image_sharpening_strength, 0.f, 100.f);
    if (!(settings.hud_scale >= 0.f)) settings.hud_scale = 0.f;  // never below 0 (also catches NaN)
    SaveSettings();
  }

  ImGui::Separator();
  ImGui::TextDisabled("Status: %s", status_text);
  if (!last_ngx_message.empty()) ImGui::TextColored(ERROR_COLOR, "%s", last_ngx_message.c_str());
  ImGui::TextDisabled("DS2 Upscaler %s - uses NVIDIA DLSS and AMD FidelityFX Super Resolution 3.1", DS2_VERSION);
#if DS2_DEVELOPER
  ImGui::TextColored(ERROR_COLOR, "Developer build: remote control file and status file are active");
#endif
}

}  // namespace ds2

extern "C" __declspec(dllexport) constexpr const char* NAME = "DS2 Upscaler";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "Dark Souls II DLSS / FSR 3.1 upscaling for DirectX 11";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  using namespace ds2;
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      LoadSettings();
      {
        // One line per start, for support: the version and the settings in effect.
        char text[320];
        std::snprintf(text, sizeof(text),
                      "v%s%s, settings: enabled=%d upscaler=%d mode=%d preset=%d motion_blur_off=%d disable_fxaa=%d "
                      "character_sharpening=%d image_sharpening=%d strength=%.0f hud_scale=%.2f",
                      DS2_VERSION, DS2_DEVELOPER ? " (developer build)" : "", settings.enabled, settings.upscaler, settings.mode,
                      settings.preset, settings.motion_blur_off, settings.disable_fxaa, settings.character_sharpening,
                      settings.image_sharpening, settings.image_sharpening_strength, settings.hud_scale);
        LogMessage(reshade::log::level::info, text);
      }
      renodx::utils::state::use_snapshot = true;
      renodx::utils::resource::Use(fdw_reason);
      renodx::utils::state::Use(fdw_reason);
      reshade::register_event<reshade::addon_event::init_resource>(OnInitResource);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::register_event<reshade::addon_event::init_resource_view>(OnInitResourceView);
      reshade::register_event<reshade::addon_event::destroy_resource_view>(OnDestroyResourceView);
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::init_sampler>(OnInitSampler);
      reshade::register_event<reshade::addon_event::destroy_sampler>(OnDestroySampler);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::register_event<reshade::addon_event::map_buffer_region>(OnMapBufferRegion);
      reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnUnmapBufferRegion);
      reshade::register_event<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion);
      reshade::register_event<reshade::addon_event::update_buffer_region_command>(OnUpdateBufferRegionCommand);
      reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
      reshade::register_event<reshade::addon_event::bind_vertex_buffers>(OnBindVertexBuffers);
      reshade::register_event<reshade::addon_event::bind_scissor_rects>(OnBindScissorRects);
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::register_event<reshade::addon_event::draw>(OnDrawVertices);
      reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_overlay("DS2 Upscaler", DrawOverlay);
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_overlay("DS2 Upscaler", DrawOverlay);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
      reshade::unregister_event<reshade::addon_event::draw>(OnDrawVertices);
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
      reshade::unregister_event<reshade::addon_event::bind_scissor_rects>(OnBindScissorRects);
      reshade::unregister_event<reshade::addon_event::bind_vertex_buffers>(OnBindVertexBuffers);
      reshade::unregister_event<reshade::addon_event::bind_viewports>(OnBindViewports);
      reshade::unregister_event<reshade::addon_event::update_buffer_region_command>(OnUpdateBufferRegionCommand);
      reshade::unregister_event<reshade::addon_event::update_buffer_region>(OnUpdateBufferRegion);
      reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(OnUnmapBufferRegion);
      reshade::unregister_event<reshade::addon_event::map_buffer_region>(OnMapBufferRegion);
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
      reshade::unregister_event<reshade::addon_event::bind_pipeline>(OnBindPipeline);
      reshade::unregister_event<reshade::addon_event::init_pipeline>(OnInitPipeline);
      reshade::unregister_event<reshade::addon_event::destroy_sampler>(OnDestroySampler);
      reshade::unregister_event<reshade::addon_event::init_sampler>(OnInitSampler);
      reshade::unregister_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_resource_view>(OnDestroyResourceView);
      reshade::unregister_event<reshade::addon_event::init_resource_view>(OnInitResourceView);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
      reshade::unregister_event<reshade::addon_event::init_resource>(OnInitResource);
      renodx::utils::state::Use(fdw_reason);
      renodx::utils::resource::Use(fdw_reason);
      reshade::unregister_addon(h_module);
      break;
  }
  return TRUE;
}

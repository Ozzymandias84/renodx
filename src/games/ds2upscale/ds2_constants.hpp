// Shader hashes the add-on reacts to, the DLSS modes and a few fixed sizes.
#pragma once

#include "ds2_common.hpp"

namespace ds2 {

constexpr uint64_t SCENE_CAMERA_BUFFER_BYTES = 320;  // VS b0 of the scene passes: projection, view and camera position
constexpr uint32_t UV_SCALE_CONSTANT_SLOT = 11;      // b11: unused by the game's shaders, carries the add-on's constants
constexpr uint32_t UV_SCALE_CONSTANT_COUNT = 20;     // floats pushed to b11 (see uv_scale_values in ds2_state.hpp)
constexpr uint32_t STENCIL_MOVING_BIT = 2u;          // stencil bit the game sets on moving objects
constexpr uint32_t STENCIL_FOLIAGE_BIT = 4u;         // stencil bit this add-on writes on wind-animated foliage (FSR only)
constexpr uint32_t FOLIAGE_VERTEX_SHADER = 0x4A2EC780u;  // grass / leaves: vertex sway driven by sin/cos and per-material wind constants

// Plain texture blit that opens the post-processing chain: it occurs only three times per frame, so its first draw is the
// moment the scene is final and nothing has blurred it yet (upscaler trigger). The tonemap draw is the fallback anchor.
constexpr uint32_t POST_CHAIN_BLIT_PIXEL_SHADER = 0xBEE6AC7Eu;
constexpr uint32_t TONEMAP_PIXEL_SHADER = 0x8DE1836Du;
constexpr uint32_t FXAA_PIXEL_SHADER = 0xDA4A34C6u;  // the game's FXAA (console variant): reads the luma-alpha copy and writes the picture the HUD is drawn on

// Render scale: the scene is drawn into the top-left sub-rectangle of the game's full-size render targets. Full-screen
// quads take their UVs from the vertex data, so these vertex shaders are replaced while the scene is scaled.
constexpr uint32_t QUAD_VERTEX_SHADER = 0x449A78BCu;
constexpr uint32_t SHADOW_QUAD_VERTEX_SHADER = 0x3C0AB793u;
constexpr uint32_t COC_PIXEL_SHADER = 0x895709DFu;  // depth of field circle of confusion: reads the low-res depth chain
// Motion blur / depth of field combine (and its masked variant): replaced by a passthrough that keeps the CoC alpha.
constexpr uint32_t MOTION_BLUR_PIXEL_SHADER = 0x52355C56u;
constexpr uint32_t MASKED_MOTION_BLUR_PIXEL_SHADER = 0x89CD9946u;
constexpr uint32_t WATER_PIXEL_SHADER = 0x938B36DAu;        // water surface: mirrored reflection lookup + refraction offsets
constexpr uint32_t LIGHT_SHAFT_PIXEL_SHADER = 0xF036E70Eu;  // sun ray mask: tests the half-resolution depth for sky
// SSAO (only present while the game's SSAO option is on): the AO pass and its history copy assume the whole texture.
constexpr uint32_t SSAO_PIXEL_SHADER = 0xACABD0A4u;
constexpr uint32_t SSAO_HISTORY_COPY_VERTEX_SHADER = 0x04FB3A5Cu;
constexpr uint32_t SSAO_HISTORY_COPY_PIXEL_SHADER = 0xE5206B32u;
// Depth of field blur (8 taps on one ring) and combine: replaced while the upscaler runs.
constexpr uint32_t DOF_BLUR_PIXEL_SHADER = 0x5EE74652u;
constexpr uint32_t DOF_COMBINE_PIXEL_SHADER = 0xD817AC63u;
// Skinned mesh vertex shaders (characters, NPCs) of the G-buffer and material passes.
constexpr uint32_t SKINNED_VERTEX_SHADER_A = 0x1095D461u;
constexpr uint32_t SKINNED_VERTEX_SHADER_B = 0x8A0F7AF9u;

struct UpscaleMode {
  const char* name;
  float ratio;  // render size / output size per axis
  NVSDK_NGX_PerfQuality_Value quality;
};
constexpr UpscaleMode UPSCALE_MODES[] = {
    {"Native (DLAA)", 1.f, NVSDK_NGX_PerfQuality_Value_DLAA},
    {"Quality", 1.f / 1.5f, NVSDK_NGX_PerfQuality_Value_MaxQuality},
    {"Balanced", 0.58f, NVSDK_NGX_PerfQuality_Value_Balanced},
    {"Performance", 0.5f, NVSDK_NGX_PerfQuality_Value_MaxPerf},
    {"Ultra Performance", 1.f / 3.f, NVSDK_NGX_PerfQuality_Value_UltraPerformance},
};
constexpr int UPSCALE_MODE_COUNT = static_cast<int>(sizeof(UPSCALE_MODES) / sizeof(UPSCALE_MODES[0]));

// NVSDK_NGX_DLSS_Hint_Render_Preset values offered in the overlay (0 = the driver's default for the mode).
constexpr int DLSS_PRESET_VALUES[] = {0, 10, 11, 12, 13};
constexpr int DLSS_PRESET_COUNT = static_cast<int>(sizeof(DLSS_PRESET_VALUES) / sizeof(DLSS_PRESET_VALUES[0]));

}  // namespace ds2

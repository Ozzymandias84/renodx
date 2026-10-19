// NVIDIA NGX / DLSS lifetime: SDK init, feature creation per size + mode + preset, shutdown.
#pragma once

#include "ds2_state.hpp"
#include "ds2_util.hpp"

namespace ds2 {

inline std::string NgxResultText(NVSDK_NGX_Result result) {
  char text[96];
  std::snprintf(text, sizeof(text), "0x%08X", static_cast<uint32_t>(result));
  const wchar_t* wide = GetNGXResultAsString(result);
  std::string description;
  for (; wide != nullptr && *wide != 0; wide++) description.push_back(*wide < 128 ? static_cast<char>(*wide) : '?');
  return std::string(text) + " " + description;
}

inline void ReleaseDlssFeature() {
  if (dlss.feature != nullptr) {
    NVSDK_NGX_D3D11_ReleaseFeature(dlss.feature);
    dlss.feature = nullptr;
  }
  dlss.feature_width = 0;
  dlss.feature_height = 0;
}

inline void ShutdownDlss() {
  ReleaseDlssFeature();
  if (dlss.parameters != nullptr) {
    NVSDK_NGX_D3D11_DestroyParameters(dlss.parameters);
    dlss.parameters = nullptr;
  }
  if (dlss.sdk_initialized && dlss.device != nullptr) {
    NVSDK_NGX_D3D11_Shutdown1(dlss.device);
  }
  dlss.sdk_initialized = false;
  dlss.device = nullptr;
  dlss.warm_up_steps = 0;
  dlss.warm_up_frame = 0;
}

// Initialises NGX once per device. Returns false (and schedules a retry) on failure.
inline bool InitDlssSdk(ID3D11Device* native_device) {
  if (dlss.sdk_initialized) return true;
  const std::filesystem::path exe_dir = ExeDirectory();
  const std::wstring exe_dir_text = exe_dir.wstring();
  const std::wstring data_dir_text = (std::filesystem::temp_directory_path() / "ds2upscale").wstring();
  std::error_code ignored;
  std::filesystem::create_directories(data_dir_text, ignored);

  const wchar_t* search_paths[] = {exe_dir_text.c_str()};
  NVSDK_NGX_FeatureCommonInfo common_info = {};
  common_info.PathListInfo.Path = search_paths;
  common_info.PathListInfo.Length = 1;

  const NVSDK_NGX_Result init_result = NVSDK_NGX_D3D11_Init_with_ProjectID(
      "e8a6d4f3-5b1c-4c79-9a0d-2f6a73c1b905", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data_dir_text.c_str(), native_device,
      &common_info, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(init_result)) {
    last_ngx_message = "NGX init failed: " + NgxResultText(init_result);
    status_text = "NGX init failed (see ReShade.log)";
    LogMessage(reshade::log::level::error, last_ngx_message);
    dlss.failed_attempts++;
    dlss.next_attempt_frame = frame_number + 600;
    return false;
  }
  dlss.sdk_initialized = true;
  dlss.device = native_device;

  NVSDK_NGX_Parameter* capabilities = nullptr;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilities)) && capabilities != nullptr) {
    int available = 0;
    int needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(capabilities, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    LogMessage(reshade::log::level::info, "DLSS super sampling available=" + std::to_string(available)
                                              + " needs_updated_driver=" + std::to_string(needs_driver));
  }
  return true;
}

// Creates (or keeps) the DLSS feature for the given render size, output size, mode and preset.
inline bool EnsureDlssFeature(ID3D11Device* native_device, ID3D11DeviceContext* context, uint32_t width, uint32_t height,
                              uint32_t target_width, uint32_t target_height) {
  if (dlss.feature != nullptr && dlss.feature_width == width && dlss.feature_height == height
      && dlss.feature_target_width == target_width && dlss.feature_target_height == target_height
      && dlss.feature_mode == latched_mode && dlss.feature_preset == settings.preset) {
    const bool warm_up_due = (dlss.warm_up_steps < 2 && dlss.warm_up_frame != 0 && frame_number >= dlss.warm_up_frame);
    if (!warm_up_due) return true;
    dlss.warm_up_steps++;
    LogMessage(reshade::log::level::info, "recreating the DLSS feature (warm-up " + std::to_string(dlss.warm_up_steps) + ")");
  }
  if (frame_number < dlss.next_attempt_frame) return false;

  ReleaseDlssFeature();
  if (dlss.device != nullptr && dlss.device != native_device) ShutdownDlss();
  if (!InitDlssSdk(native_device)) return false;

  if (dlss.parameters == nullptr) {
    const NVSDK_NGX_Result parameters_result = NVSDK_NGX_D3D11_AllocateParameters(&dlss.parameters);
    if (NVSDK_NGX_FAILED(parameters_result) || dlss.parameters == nullptr) {
      last_ngx_message = "NGX AllocateParameters failed: " + NgxResultText(parameters_result);
      status_text = "NGX parameters failed";
      LogMessage(reshade::log::level::error, last_ngx_message);
      dlss.parameters = nullptr;
      dlss.next_attempt_frame = frame_number + 600;
      return false;
    }
  }

  // The preset hint is per quality mode: set it for all of them so every mode uses the selected preset.
  const auto preset = static_cast<unsigned int>(settings.preset);
  NVSDK_NGX_Parameter_SetUI(dlss.parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
  NVSDK_NGX_Parameter_SetUI(dlss.parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
  NVSDK_NGX_Parameter_SetUI(dlss.parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
  NVSDK_NGX_Parameter_SetUI(dlss.parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
  NVSDK_NGX_Parameter_SetUI(dlss.parameters, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = width;
  create.Feature.InHeight = height;
  create.Feature.InTargetWidth = target_width;
  create.Feature.InTargetHeight = target_height;
  create.Feature.InPerfQualityValue = UPSCALE_MODES[latched_mode].quality;
  // The motion vectors are full-size textures that hold data only in the render rectangle; exposure is fixed (tuning.exposure).
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
  create.InEnableOutputSubrects = false;

  const NVSDK_NGX_Result create_result = NGX_D3D11_CREATE_DLSS_EXT(context, &dlss.feature, dlss.parameters, &create);
  if (NVSDK_NGX_FAILED(create_result) || dlss.feature == nullptr) {
    last_ngx_message = "DLSS CreateFeature failed: " + NgxResultText(create_result);
    status_text = "DLSS feature creation failed (is nvngx_dlss.dll next to the game exe?)";
    LogMessage(reshade::log::level::error, last_ngx_message);
    dlss.feature = nullptr;
    dlss.failed_attempts++;
    dlss.next_attempt_frame = frame_number + 600;
    return false;
  }
  dlss.feature_width = width;
  dlss.feature_height = height;
  dlss.feature_target_width = target_width;
  dlss.feature_target_height = target_height;
  dlss.feature_mode = latched_mode;
  dlss.feature_preset = settings.preset;
  last_upscaled_frame = 0;
  if (dlss.warm_up_steps < 2) dlss.warm_up_frame = frame_number + (dlss.warm_up_steps == 0 ? 30 : 150);
  LogMessage(reshade::log::level::info,
             std::string("DLSS feature created (") + UPSCALE_MODES[latched_mode].name + " " + std::to_string(width) + "x"
                 + std::to_string(height) + " -> " + std::to_string(target_width) + "x" + std::to_string(target_height)
                 + ", preset " + std::to_string(settings.preset) + ")");
  return true;
}

}  // namespace ds2

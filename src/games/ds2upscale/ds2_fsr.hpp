// FSR 3.1 upscaler for DirectX 11: a port of AMD's FidelityFX SDK upscaler (MIT, see fsr3/LICENSE-AMD-FidelityFX-SDK.md).
// AMD's compute shaders (fsr3/amd, shader model 5.0 build) are run here with plain D3D11 calls; the resources, constants and the pass
// order follow AMD's ffx_fsr3upscaler.cpp (fsr3upscalerCreate / fsr3upscalerDispatch). The inputs are the same textures DLSS reads:
// the scene colour, the main depth buffer and the add-on's motion vectors, all full-size textures that hold the picture in the render
// rectangle (top-left). The internal textures are therefore sized like the back buffer ("max render size" in FSR's terms).
#pragma once

#include <wrl/client.h>

#include "ds2_state.hpp"
#include "ds2_util.hpp"

namespace ds2 {

using Microsoft::WRL::ComPtr;

// Mirror of AMD's Fsr3UpscalerConstants / cbFSR3Upscaler (160 bytes with the constant buffer padding).
struct FsrConstants {
  int32_t render_size[2];
  int32_t previous_render_size[2];
  int32_t upscale_size[2];
  int32_t previous_upscale_size[2];
  int32_t max_render_size[2];
  int32_t max_upscale_size[2];
  float device_to_view_depth[4];
  float jitter[2];
  float previous_jitter[2];
  float motion_vector_scale[2];
  float downscale_factor[2];
  float motion_vector_jitter_cancellation[2];
  float tan_half_fov;
  float jitter_phase_count;
  float delta_time;
  float delta_pre_exposure;
  float view_space_to_meters;
  float frame_index;
  float velocity_factor;
  float reactiveness_scale;
  float shading_change_scale;
  float accumulation_added_per_frame;
  float min_disocclusion_accumulation;
  float padding[3];
};
static_assert(sizeof(FsrConstants) == 160);

struct FsrSpdConstants {
  uint32_t mips;
  uint32_t work_groups;
  uint32_t work_group_offset[2];
  uint32_t render_size[2];
  uint32_t padding[2];
};

struct FsrTexture {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;
};

// Views of the game's textures that FSR reads, cached per resource.
struct FsrInputView {
  ComPtr<ID3D11ShaderResourceView> srv;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

constexpr uint32_t FSR_SPD_MIP_VIEWS = 6;

struct FsrState {
  bool created = false;
  bool failed = false;  // creation failed once for this size: not retried every frame
  uint32_t width = 0;   // output size (back buffer): the size of the textures that hold the upscaled picture
  uint32_t height = 0;
  uint32_t render_w = 0;  // render size of the mode the textures were made for ("max render size" in FSR terms)
  uint32_t render_h = 0;
  uint64_t memory_bytes = 0;

  ComPtr<ID3D11ComputeShader> prepare_inputs, luma_pyramid, shading_change_pyramid, shading_change, prepare_reactivity,
      luma_instability, accumulate, accumulate_plain, rcas;
  ComPtr<ID3D11Buffer> constants_buffer, spd_buffer, rcas_buffer;
  ComPtr<ID3D11SamplerState> linear_clamp;

  FsrTexture accumulation[2], luma[2], intermediate, shading_change_map, new_locks, upscaled[2], spd_mips, farthest_mip1,
      luma_history[2], spd_atomic, dilated_reactive, lanczos, default_reactive, frame_info, dilated_depth, dilated_motion,
      reconstructed_depth;
  std::array<ComPtr<ID3D11UnorderedAccessView>, FSR_SPD_MIP_VIEWS> spd_mip_uavs;

  FsrConstants constants = {};
  bool first_execution = true;
  uint32_t resource_frame = 0;
  int64_t last_tick = 0;
  std::unordered_map<ID3D11Resource*, FsrInputView> input_views;
  ComPtr<ID3D11UnorderedAccessView> output_uav;  // view of the add-on's upscaled texture (the output), made once
  ID3D11Resource* output_resource = nullptr;
  std::string message;
};
inline FsrState fsr;

inline void FsrReleaseResources() {
  fsr = FsrState{};
}

inline void FsrForgetResource(uint64_t handle) {
  fsr.input_views.erase(reinterpret_cast<ID3D11Resource*>(handle));
}

inline float FsrLanczos2(float value) {
  constexpr float PI = 3.14159265358979f;
  if (std::fabs(value) < 1e-6f) return 1.f;
  return (std::sin(PI * value) / (PI * value)) * (std::sin(0.5f * PI * value) / (0.5f * PI * value));
}

inline bool FsrCreateTexture(ID3D11Device* device, FsrTexture* out, uint32_t width, uint32_t height, DXGI_FORMAT format,
                             bool uav, uint32_t mip_levels = 1, const void* initial = nullptr, uint32_t row_pitch = 0) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = std::max(1u, width);
  desc.Height = std::max(1u, height);
  desc.MipLevels = mip_levels;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
  D3D11_SUBRESOURCE_DATA data = {initial, row_pitch, 0};
  if (FAILED(device->CreateTexture2D(&desc, initial != nullptr ? &data : nullptr, &out->texture))) return false;
  if (FAILED(device->CreateShaderResourceView(out->texture.Get(), nullptr, &out->srv))) return false;
  if (uav && FAILED(device->CreateUnorderedAccessView(out->texture.Get(), nullptr, &out->uav))) return false;
  {
    uint32_t bytes = 4;
    switch (format) {
      case DXGI_FORMAT_R8_UNORM: bytes = 1; break;
      case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_SNORM: bytes = 2; break;
      case DXGI_FORMAT_R16G16B16A16_FLOAT: bytes = 8; break;
      case DXGI_FORMAT_R32G32B32A32_FLOAT: bytes = 16; break;
      default: break;
    }
    fsr.memory_bytes += static_cast<uint64_t>(desc.Width) * desc.Height * bytes * (mip_levels == 1 ? 1.0 : 1.34);
  }
  return true;
}

inline bool FsrCreateBuffer(ID3D11Device* device, ComPtr<ID3D11Buffer>* out, uint32_t size) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  return SUCCEEDED(device->CreateBuffer(&desc, nullptr, out->GetAddressOf()));
}

// Typed UAV loads of these formats are used by the passes (they are optional in D3D11.0, standard on current GPUs).
inline bool FsrCheckTypedLoads(ID3D11Device* device) {
  const DXGI_FORMAT formats[] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8_UNORM,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM};
  for (const DXGI_FORMAT format : formats) {
    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(format, &support))) return false;
    UINT support2 = 0;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 query = {format, 0};
    if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &query, sizeof(query)))) return false;
    support2 = query.OutFormatSupport2;
    if ((support2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0 || (support2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) return false;
  }
  return true;
}

inline bool FsrEnsureResources(ID3D11Device* device, uint32_t width, uint32_t height, uint32_t render_w, uint32_t render_h) {
  if (fsr.created && fsr.width == width && fsr.height == height && fsr.render_w == render_w && fsr.render_h == render_h) return true;
  if (fsr.failed && fsr.width == width && fsr.height == height && fsr.render_w == render_w && fsr.render_h == render_h) return false;
  FsrReleaseResources();
  fsr.width = width;
  fsr.height = height;
  fsr.render_w = render_w;
  fsr.render_h = render_h;
  fsr.failed = true;  // cleared below when everything was created

  if (!FsrCheckTypedLoads(device)) {
    fsr.message = "the GPU lacks typed UAV loads/stores for the formats FSR needs";
    return false;
  }
  const auto make_shader = [&](ComPtr<ID3D11ComputeShader>* out, const std::span<const uint8_t>& code) {
    return SUCCEEDED(device->CreateComputeShader(code.data(), code.size(), nullptr, out->GetAddressOf()));
  };
  if (!make_shader(&fsr.prepare_inputs, __fsr3_prepare_inputs) || !make_shader(&fsr.luma_pyramid, __fsr3_luma_pyramid)
      || !make_shader(&fsr.shading_change_pyramid, __fsr3_shading_change_pyramid)
      || !make_shader(&fsr.shading_change, __fsr3_shading_change) || !make_shader(&fsr.prepare_reactivity, __fsr3_prepare_reactivity)
      || !make_shader(&fsr.luma_instability, __fsr3_luma_instability) || !make_shader(&fsr.accumulate, __fsr3_accumulate)
      || !make_shader(&fsr.accumulate_plain, __fsr3_accumulate_plain) || !make_shader(&fsr.rcas, __fsr3_rcas)) {
    fsr.message = "FSR compute shaders could not be created";
    return false;
  }
  if (!FsrCreateBuffer(device, &fsr.constants_buffer, sizeof(FsrConstants)) || !FsrCreateBuffer(device, &fsr.spd_buffer, sizeof(FsrSpdConstants))
      || !FsrCreateBuffer(device, &fsr.rcas_buffer, 16)) {
    fsr.message = "FSR constant buffers could not be created";
    return false;
  }
  D3D11_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sampler, &fsr.linear_clamp))) {
    fsr.message = "FSR sampler could not be created";
    return false;
  }

  const uint32_t half_w = std::max(1u, render_w / 2);
  const uint32_t half_h = std::max(1u, render_h / 2);
  bool ok = true;
  for (int i = 0; i < 2; i++) {
    ok &= FsrCreateTexture(device, &fsr.accumulation[i], render_w, render_h, DXGI_FORMAT_R8_UNORM, true);
    ok &= FsrCreateTexture(device, &fsr.luma[i], render_w, render_h, DXGI_FORMAT_R16_FLOAT, true);
    ok &= FsrCreateTexture(device, &fsr.upscaled[i], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    ok &= FsrCreateTexture(device, &fsr.luma_history[i], render_w, render_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
  }
  ok &= FsrCreateTexture(device, &fsr.intermediate, render_w, render_h, DXGI_FORMAT_R16_FLOAT, true);
  ok &= FsrCreateTexture(device, &fsr.shading_change_map, half_w, half_h, DXGI_FORMAT_R8_UNORM, true);
  ok &= FsrCreateTexture(device, &fsr.new_locks, width, height, DXGI_FORMAT_R8_UNORM, true);
  ok &= FsrCreateTexture(device, &fsr.spd_mips, half_w, half_h, DXGI_FORMAT_R16G16_FLOAT, true, 0);
  ok &= FsrCreateTexture(device, &fsr.farthest_mip1, half_w, half_h, DXGI_FORMAT_R16_FLOAT, true);
  ok &= FsrCreateTexture(device, &fsr.spd_atomic, 1, 1, DXGI_FORMAT_R32_UINT, true);
  ok &= FsrCreateTexture(device, &fsr.dilated_reactive, render_w, render_h, DXGI_FORMAT_R8G8B8A8_UNORM, true);
  ok &= FsrCreateTexture(device, &fsr.frame_info, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
  ok &= FsrCreateTexture(device, &fsr.dilated_depth, render_w, render_h, DXGI_FORMAT_R32_FLOAT, true);
  ok &= FsrCreateTexture(device, &fsr.dilated_motion, render_w, render_h, DXGI_FORMAT_R16G16_FLOAT, true);
  ok &= FsrCreateTexture(device, &fsr.reconstructed_depth, render_w, render_h, DXGI_FORMAT_R32_UINT, true);
  const uint8_t zero_reactive = 0;
  ok &= FsrCreateTexture(device, &fsr.default_reactive, 1, 1, DXGI_FORMAT_R8_UNORM, false, 1, &zero_reactive, 1);
  std::array<int16_t, 128> lanczos_weights = {};
  for (uint32_t i = 0; i < lanczos_weights.size(); i++) {
    const float x = 2.f * static_cast<float>(i) / static_cast<float>(lanczos_weights.size() - 1);
    lanczos_weights[i] = static_cast<int16_t>(std::lround(FsrLanczos2(x) * 32767.f));
  }
  ok &= FsrCreateTexture(device, &fsr.lanczos, static_cast<uint32_t>(lanczos_weights.size()), 1, DXGI_FORMAT_R16_SNORM, false, 1,
                         lanczos_weights.data(), static_cast<uint32_t>(sizeof(lanczos_weights)));
  if (ok) {
    for (uint32_t mip = 0; mip < FSR_SPD_MIP_VIEWS; mip++) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
      desc.Format = DXGI_FORMAT_R16G16_FLOAT;
      desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipSlice = mip;
      ok &= SUCCEEDED(device->CreateUnorderedAccessView(fsr.spd_mips.texture.Get(), &desc, fsr.spd_mip_uavs[mip].GetAddressOf()));
    }
  }
  if (!ok) {
    fsr.message = "FSR textures could not be created";
    return false;
  }
  fsr.failed = false;
  fsr.created = true;
  fsr.first_execution = true;
  fsr.message.clear();
  return true;
}

// A view of one of the game's textures (colour, depth, motion vectors), cached per resource.
inline ID3D11ShaderResourceView* FsrInputSrv(ID3D11Device* device, ID3D11Resource* resource, DXGI_FORMAT format) {
  auto& entry = fsr.input_views[resource];
  if (entry.srv != nullptr && entry.format == format) return entry.srv.Get();
  entry.srv.Reset();
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = 1;
  if (FAILED(device->CreateShaderResourceView(resource, &desc, entry.srv.GetAddressOf()))) return nullptr;
  entry.format = format;
  return entry.srv.Get();
}

struct FsrSavedState {
  ID3D11ComputeShader* shader = nullptr;
  ID3D11ShaderResourceView* srvs[9] = {};
  ID3D11UnorderedAccessView* uavs[8] = {};
  ID3D11Buffer* buffers[2] = {};
  ID3D11SamplerState* sampler = nullptr;
};

inline void FsrSaveState(ID3D11DeviceContext* context, FsrSavedState* state) {
  context->CSGetShader(&state->shader, nullptr, nullptr);
  context->CSGetShaderResources(0, 9, state->srvs);
  context->CSGetUnorderedAccessViews(0, 8, state->uavs);
  context->CSGetConstantBuffers(0, 2, state->buffers);
  context->CSGetSamplers(1, 1, &state->sampler);
}

inline void FsrRestoreState(ID3D11DeviceContext* context, FsrSavedState* state) {
  context->CSSetShader(state->shader, nullptr, 0);
  context->CSSetShaderResources(0, 9, state->srvs);
  const UINT keep_counts[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  context->CSSetUnorderedAccessViews(0, 8, state->uavs, keep_counts);
  context->CSSetConstantBuffers(0, 2, state->buffers);
  context->CSSetSamplers(1, 1, &state->sampler);
  if (state->shader != nullptr) state->shader->Release();
  for (auto* view : state->srvs) {
    if (view != nullptr) view->Release();
  }
  for (auto* view : state->uavs) {
    if (view != nullptr) view->Release();
  }
  for (auto* buffer : state->buffers) {
    if (buffer != nullptr) buffer->Release();
  }
  if (state->sampler != nullptr) state->sampler->Release();
}

// One compute pass: shader, the shader resources (t0..), the unordered access views (u0..) and the dispatch size.
inline void FsrDispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                        std::initializer_list<ID3D11UnorderedAccessView*> uavs, uint32_t groups_x, uint32_t groups_y) {
  std::array<ID3D11ShaderResourceView*, 9> srv_slots = {};
  std::array<ID3D11UnorderedAccessView*, 8> uav_slots = {};
  std::copy(srvs.begin(), srvs.end(), srv_slots.begin());
  std::copy(uavs.begin(), uavs.end(), uav_slots.begin());
  const UINT keep_counts[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
  context->CSSetShader(shader, nullptr, 0);
  context->CSSetShaderResources(0, 9, srv_slots.data());
  context->CSSetUnorderedAccessViews(0, 8, uav_slots.data(), keep_counts);
  context->Dispatch(groups_x, groups_y, 1);
  std::array<ID3D11ShaderResourceView*, 9> no_srvs = {};
  std::array<ID3D11UnorderedAccessView*, 8> no_uavs = {};
  context->CSSetShaderResources(0, 9, no_srvs.data());
  context->CSSetUnorderedAccessViews(0, 8, no_uavs.data(), keep_counts);
}

// Runs the FSR 3.1 passes. `scene` is the jittered RGBA16F colour, `depth` the main depth buffer (r24g8), `motion` the add-on's motion
// vector texture (render pixels, "pixel + vector = previous position"), `reactive` the optional reactive mask (R8). The result is written
// to `output` (RGBA16F, UAV capable).
inline bool RunFsr(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* scene, ID3D11Resource* depth, ID3D11Resource* motion,
                   ID3D11Resource* output, ID3D11Resource* reactive, uint32_t render_w, uint32_t render_h, uint32_t full_w, uint32_t full_h,
                   bool reset, float sharpness) {
  if (!FsrEnsureResources(device, full_w, full_h, render_w, render_h)) return false;
  if (fsr.created && output == nullptr) return false;

  auto* color_srv = FsrInputSrv(device, scene, DXGI_FORMAT_R16G16B16A16_FLOAT);
  auto* depth_srv = FsrInputSrv(device, depth, DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
  auto* motion_srv = FsrInputSrv(device, motion, DXGI_FORMAT_R16G16_FLOAT);
  if (color_srv == nullptr || depth_srv == nullptr || motion_srv == nullptr) {
    fsr.message = "FSR input views could not be created";
    return false;
  }
  ID3D11ShaderResourceView* reactive_srv = (reactive != nullptr ? FsrInputSrv(device, reactive, DXGI_FORMAT_R8_UNORM) : nullptr);
  if (reactive_srv == nullptr) reactive_srv = fsr.default_reactive.srv.Get();
  if (fsr.output_uav == nullptr || fsr.output_resource != output) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    fsr.output_uav.Reset();
    fsr.output_resource = nullptr;
    if (FAILED(device->CreateUnorderedAccessView(output, &desc, &fsr.output_uav))) {
      fsr.message = "FSR output view could not be created";
      return false;
    }
    fsr.output_resource = output;
  }
  ID3D11UnorderedAccessView* const output_uav = fsr.output_uav.Get();

  const bool reset_accumulation = reset || fsr.first_execution;
  const bool odd = (fsr.resource_frame & 1u) != 0;
  FsrTexture& accumulation_read = fsr.accumulation[odd ? 1 : 0];
  FsrTexture& accumulation_write = fsr.accumulation[odd ? 0 : 1];
  FsrTexture& upscaled_read = fsr.upscaled[odd ? 1 : 0];
  FsrTexture& upscaled_write = fsr.upscaled[odd ? 0 : 1];
  FsrTexture& history_read = fsr.luma_history[odd ? 1 : 0];
  FsrTexture& history_write = fsr.luma_history[odd ? 0 : 1];
  FsrTexture& luma_current = fsr.luma[odd ? 1 : 0];
  FsrTexture& luma_previous = fsr.luma[odd ? 0 : 1];

  // ---- constants (AMD: fsr3upscalerDispatch) ----
  FsrConstants& c = fsr.constants;
  std::memcpy(c.previous_jitter, c.jitter, sizeof(c.jitter));
  std::memcpy(c.previous_render_size, c.render_size, sizeof(c.render_size));
  std::memcpy(c.previous_upscale_size, c.upscale_size, sizeof(c.upscale_size));
  c.jitter[0] = jitter_x;
  c.jitter[1] = jitter_y;
  c.render_size[0] = static_cast<int32_t>(render_w);
  c.render_size[1] = static_cast<int32_t>(render_h);
  c.upscale_size[0] = static_cast<int32_t>(full_w);
  c.upscale_size[1] = static_cast<int32_t>(full_h);
  c.max_render_size[0] = static_cast<int32_t>(render_w);  // the internal render-size textures are made for exactly this size
  c.max_upscale_size[0] = static_cast<int32_t>(full_w);
  c.max_render_size[1] = static_cast<int32_t>(render_h);
  c.max_upscale_size[1] = static_cast<int32_t>(full_h);
  if (reset_accumulation) {
    std::memcpy(c.previous_jitter, c.jitter, sizeof(c.jitter));
    std::memcpy(c.previous_render_size, c.render_size, sizeof(c.render_size));
    std::memcpy(c.previous_upscale_size, c.upscale_size, sizeof(c.upscale_size));
  }
  // depth = A - B / z in the add-on's camera capture; FSR wants z = factor1 / (depth - factor0)
  c.device_to_view_depth[0] = camera_current.depth_a;
  c.device_to_view_depth[1] = -camera_current.depth_b;
  c.device_to_view_depth[2] = 1.f / camera_current.proj_x;
  c.device_to_view_depth[3] = 1.f / camera_current.proj_y;
  c.motion_vector_scale[0] = 1.f / static_cast<float>(render_w);
  c.motion_vector_scale[1] = 1.f / static_cast<float>(render_h);
  c.downscale_factor[0] = static_cast<float>(render_w) / static_cast<float>(full_w);
  c.downscale_factor[1] = static_cast<float>(render_h) / static_cast<float>(full_h);
  c.motion_vector_jitter_cancellation[0] = c.motion_vector_jitter_cancellation[1] = 0.f;
  c.tan_half_fov = 1.f / camera_current.proj_x;
  c.jitter_phase_count = static_cast<float>(jitter_phases);
  static const double ticks_to_ms = [] {
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    return 1000.0 / static_cast<double>(frequency.QuadPart);
  }();
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  const float frame_ms = (fsr.last_tick == 0) ? 16.6f : static_cast<float>(static_cast<double>(now.QuadPart - fsr.last_tick) * ticks_to_ms);
  fsr.last_tick = now.QuadPart;
  c.delta_time = std::clamp(frame_ms / 1000.f, 0.f, 1.f);
  c.delta_pre_exposure = 1.f;
  c.view_space_to_meters = 1.f;
  c.frame_index = reset_accumulation ? 0.f : c.frame_index + 1.f;
  c.velocity_factor = tuning.fsr_velocity_factor;
  c.reactiveness_scale = tuning.fsr_reactiveness;
  c.shading_change_scale = tuning.fsr_shading_change;
  c.accumulation_added_per_frame = tuning.fsr_accumulation;
  c.min_disocclusion_accumulation = tuning.fsr_min_disocclusion;

  // SPD (single pass downsampler) set-up for the render rectangle: AMD's ffxSpdSetup with rect {0, 0, w, h}.
  const uint32_t end_x = (render_w - 1) / 64;
  const uint32_t end_y = (render_h - 1) / 64;
  const uint32_t spd_groups_x = end_x + 1;
  const uint32_t spd_groups_y = end_y + 1;
  FsrSpdConstants spd = {};
  spd.work_groups = spd_groups_x * spd_groups_y;
  spd.mips = static_cast<uint32_t>(std::min(std::floor(std::log2(static_cast<float>(std::max(render_w, render_h)))), 12.f));
  spd.render_size[0] = render_w;
  spd.render_size[1] = render_h;

  // RCAS: sharpness 0..1 -> stops (AMD: (-2 * sharpness) + 2), then FsrRcasCon.
  const float stops = (-2.f * std::clamp(sharpness, 0.f, 2.f)) + 2.f;  // above 1 the filter is pushed past AMD's nominal maximum
  const float rcas_sharpness = std::exp2(-stops);
  uint32_t rcas_config[4] = {};
  std::memcpy(&rcas_config[0], &rcas_sharpness, sizeof(float));
  {
    // two copies of the sharpness as half floats
    const auto to_half = [](float value) {
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      const uint32_t sign = (bits >> 16) & 0x8000u;
      int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
      uint32_t mantissa = (bits >> 13) & 0x3FFu;
      if (exponent <= 0) return static_cast<uint32_t>(sign);
      if (exponent >= 31) return static_cast<uint32_t>(sign | 0x7C00u);
      return static_cast<uint32_t>(sign | (static_cast<uint32_t>(exponent) << 10) | mantissa);
    };
    const uint32_t half = to_half(rcas_sharpness);
    rcas_config[1] = half | (half << 16);
  }

  FsrSavedState saved;
  FsrSaveState(context, &saved);

  // ---- clears ----
  const float zero_color[4] = {0.f, 0.f, 0.f, 0.f};
  const UINT zero_uint[4] = {0, 0, 0, 0};
  if (fsr.first_execution) {
    for (int i = 0; i < 2; i++) {
      context->ClearUnorderedAccessViewFloat(fsr.accumulation[i].uav.Get(), zero_color);
      context->ClearUnorderedAccessViewFloat(fsr.luma[i].uav.Get(), zero_color);
    }
  }
  if (reset_accumulation) {
    context->ClearUnorderedAccessViewFloat(accumulation_read.uav.Get(), zero_color);
    for (auto& view : fsr.spd_mip_uavs) context->ClearUnorderedAccessViewFloat(view.Get(), zero_color);
    const float frame_info_reset[4] = {-1.f, 1.f, 0.f, 0.f};
    context->ClearUnorderedAccessViewFloat(fsr.frame_info.uav.Get(), frame_info_reset);
  }
  const UINT far_depth_bits[4] = {0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u};  // 1.0f: the game's depth is not reversed
  context->ClearUnorderedAccessViewUint(fsr.reconstructed_depth.uav.Get(), far_depth_bits);
  context->ClearUnorderedAccessViewUint(fsr.spd_atomic.uav.Get(), zero_uint);

  context->UpdateSubresource(fsr.constants_buffer.Get(), 0, nullptr, &c, sizeof(c), 0);
  context->UpdateSubresource(fsr.spd_buffer.Get(), 0, nullptr, &spd, sizeof(spd), 0);
  context->UpdateSubresource(fsr.rcas_buffer.Get(), 0, nullptr, rcas_config, sizeof(rcas_config), 0);
  ID3D11Buffer* constant_buffers[2] = {fsr.constants_buffer.Get(), fsr.spd_buffer.Get()};
  context->CSSetConstantBuffers(0, 2, constant_buffers);
  ID3D11SamplerState* samplers[1] = {fsr.linear_clamp.Get()};
  context->CSSetSamplers(1, 1, samplers);

  const uint32_t src_x = (render_w + 7) / 8;
  const uint32_t src_y = (render_h + 7) / 8;
  const uint32_t dst_x = (full_w + 7) / 8;
  const uint32_t dst_y = (full_h + 7) / 8;
  const uint32_t half_x = ((render_w / 2) + 7) / 8;
  const uint32_t half_y = ((render_h / 2) + 7) / 8;
  ID3D11ShaderResourceView* const exposure = fsr.frame_info.srv.Get();  // auto exposure: the luma pyramid writes it

  FsrDispatch(context, fsr.prepare_inputs.Get(), {motion_srv, depth_srv, color_srv},
              {fsr.dilated_motion.uav.Get(), fsr.dilated_depth.uav.Get(), fsr.reconstructed_depth.uav.Get(), fsr.intermediate.uav.Get(),
               luma_current.uav.Get()},
              src_x, src_y);
  FsrDispatch(context, fsr.luma_pyramid.Get(), {luma_current.srv.Get(), fsr.intermediate.srv.Get()},
              {fsr.spd_atomic.uav.Get(), fsr.frame_info.uav.Get(), fsr.spd_mip_uavs[5].Get(), fsr.farthest_mip1.uav.Get()}, spd_groups_x,
              spd_groups_y);
  FsrDispatch(context, fsr.shading_change_pyramid.Get(), {luma_current.srv.Get(), luma_previous.srv.Get(), fsr.dilated_motion.srv.Get(), exposure},
              {fsr.spd_atomic.uav.Get(), fsr.spd_mip_uavs[0].Get(), fsr.spd_mip_uavs[1].Get(), fsr.spd_mip_uavs[2].Get(),
               fsr.spd_mip_uavs[3].Get(), fsr.spd_mip_uavs[4].Get(), fsr.spd_mip_uavs[5].Get()},
              spd_groups_x, spd_groups_y);
  FsrDispatch(context, fsr.shading_change.Get(), {fsr.spd_mips.srv.Get()}, {fsr.shading_change_map.uav.Get()}, half_x, half_y);
  FsrDispatch(context, fsr.prepare_reactivity.Get(),
              {fsr.reconstructed_depth.srv.Get(), fsr.dilated_motion.srv.Get(), fsr.dilated_depth.srv.Get(), reactive_srv,
               fsr.default_reactive.srv.Get() /* transparency & composition mask: not used */, accumulation_read.srv.Get(),
               fsr.shading_change_map.srv.Get(), luma_current.srv.Get(), exposure},
              {fsr.dilated_reactive.uav.Get(), fsr.new_locks.uav.Get(), accumulation_write.uav.Get()}, src_x, src_y);
  FsrDispatch(context, fsr.luma_instability.Get(),
              {exposure, fsr.dilated_reactive.srv.Get(), fsr.dilated_motion.srv.Get(), nullptr, history_read.srv.Get(), nullptr,
               luma_current.srv.Get()},
              {history_write.uav.Get(), fsr.intermediate.uav.Get()}, src_x, src_y);
  const bool sharpen = sharpness > 0.f;
  FsrDispatch(context, (sharpen ? fsr.accumulate : fsr.accumulate_plain).Get(),
              {exposure, fsr.dilated_reactive.srv.Get(), fsr.dilated_motion.srv.Get(), upscaled_read.srv.Get(), fsr.lanczos.srv.Get(),
               fsr.farthest_mip1.srv.Get(), luma_current.srv.Get(), fsr.intermediate.srv.Get(), color_srv},
              {upscaled_write.uav.Get(), (sharpen ? nullptr : output_uav), fsr.new_locks.uav.Get()}, dst_x, dst_y);
  if (sharpen) {
    ID3D11Buffer* rcas_buffers[2] = {fsr.constants_buffer.Get(), fsr.rcas_buffer.Get()};
    context->CSSetConstantBuffers(0, 2, rcas_buffers);
    FsrDispatch(context, fsr.rcas.Get(), {exposure, upscaled_write.srv.Get()}, {output_uav}, (full_w + 15) / 16, (full_h + 15) / 16);
  }

  FsrRestoreState(context, &saved);
  fsr.first_execution = false;
  fsr.resource_frame = (fsr.resource_frame + 1) % 16u;
  return true;
}

}  // namespace ds2

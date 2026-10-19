// The add-on's GPU resources: upscaler textures, replacement shader pipelines, plus the readers of the game's camera and depth
// of field constants.
#pragma once

#include "ds2_state.hpp"
#include "ds2_util.hpp"

namespace ds2 {

inline void ResetPassSlot(reshade::api::device* device, PassSlot* slot) {
  slot->pass.DestroyAll(device);
  *slot = {};
}

// Creates the textures DLSS needs once per output size: motion vectors (valid in the render rectangle) and the output.
inline bool EnsureTextures(reshade::api::device* device, uint32_t width, uint32_t height) {
  if (constants_texture.handle == 0) {
    const reshade::api::resource_desc desc(reshade::api::resource_type::texture_2d, 16, 1, 1, 1,
                                           reshade::api::format::r32g32b32a32_float, 1,
                                           reshade::api::memory_heap::gpu_only,
                                           reshade::api::resource_usage::shader_resource
                                               | reshade::api::resource_usage::copy_dest);
    if (!device->create_resource(desc, nullptr, reshade::api::resource_usage::shader_resource, &constants_texture)) {
      constants_texture = {0u};
      return false;
    }
  }
  if (exposure_texture.handle == 0) {
    const reshade::api::resource_desc desc(reshade::api::resource_type::texture_2d, 1, 1, 1, 1,
                                           reshade::api::format::r32_float, 1, reshade::api::memory_heap::gpu_only,
                                           reshade::api::resource_usage::shader_resource
                                               | reshade::api::resource_usage::copy_dest);
    if (!device->create_resource(desc, nullptr, reshade::api::resource_usage::shader_resource, &exposure_texture)) {
      exposure_texture = {0u};
      return false;
    }
    exposure_applied = -1.f;
  }
  if (exposure_applied != tuning.exposure) {
    float exposure = std::max(tuning.exposure, 0.0001f);
    const reshade::api::subresource_data data = {.data = &exposure, .row_pitch = sizeof(float), .slice_pitch = sizeof(float)};
    device->update_texture_region(data, exposure_texture, 0, nullptr);
    exposure_applied = tuning.exposure;
  }
  if (motion_vector_texture.handle != 0 && upscaled_texture.handle != 0 && texture_width == width
      && texture_height == height) {
    return true;
  }

  ResetPassSlot(device, &motion_vector_slot);
  ResetPassSlot(device, &sharpen_slot);
  if (motion_vector_texture.handle != 0) device->destroy_resource(motion_vector_texture);
  if (upscaled_texture.handle != 0) device->destroy_resource(upscaled_texture);
  motion_vector_texture = {0u};
  upscaled_texture = {0u};
  texture_width = 0;
  texture_height = 0;

  const reshade::api::resource_desc motion_desc(reshade::api::resource_type::texture_2d, width, height, 1, 1,
                                                reshade::api::format::r16g16_float, 1,
                                                reshade::api::memory_heap::gpu_only,
                                                reshade::api::resource_usage::render_target
                                                    | reshade::api::resource_usage::shader_resource);
  if (!device->create_resource(motion_desc, nullptr, reshade::api::resource_usage::shader_resource,
                               &motion_vector_texture)) {
    motion_vector_texture = {0u};
    return false;
  }
  const reshade::api::resource_desc output_desc(reshade::api::resource_type::texture_2d, width, height, 1, 1,
                                                reshade::api::format::r16g16b16a16_float, 1,
                                                reshade::api::memory_heap::gpu_only,
                                                reshade::api::resource_usage::unordered_access
                                                    | reshade::api::resource_usage::shader_resource
                                                    | reshade::api::resource_usage::copy_source);
  if (!device->create_resource(output_desc, nullptr, reshade::api::resource_usage::shader_resource, &upscaled_texture)) {
    device->destroy_resource(motion_vector_texture);
    motion_vector_texture = {0u};
    upscaled_texture = {0u};
    return false;
  }
  texture_width = width;
  texture_height = height;
  return true;
}

// Creates the replacement shader pipelines once per device, on the render thread. They read their parameters from a
// push-constant buffer at b11.
inline bool EnsureOverridePipelines(reshade::api::device* device) {
  if (override_pipelines_ready) return true;
  if (override_layout.handle == 0) {
    reshade::api::pipeline_layout_param param = {};
    param.type = reshade::api::pipeline_layout_param_type::push_constants;
    param.push_constants = {
        .binding = 0,
        .dx_register_index = UV_SCALE_CONSTANT_SLOT,
        .dx_register_space = 0,
        .count = UV_SCALE_CONSTANT_COUNT,
        .visibility = reshade::api::shader_stage::all,
    };
    if (!device->create_pipeline_layout(1, &param, &override_layout)) {
      override_layout = {0u};
      return false;
    }
  }

  const auto create = [&](std::span<const uint8_t> code, reshade::api::pipeline_subobject_type type,
                          reshade::api::pipeline* pipeline) {
    if (pipeline->handle != 0) return true;
    reshade::api::shader_desc shader = {.code = code.data(), .code_size = code.size()};
    reshade::api::pipeline_subobject subobject = {.type = type, .count = 1, .data = &shader};
    return device->create_pipeline(override_layout, 1, &subobject, pipeline);
  };
  constexpr auto vertex = reshade::api::pipeline_subobject_type::vertex_shader;
  constexpr auto pixel = reshade::api::pipeline_subobject_type::pixel_shader;
  override_pipelines_ready =
      create(__uv_scale_quad_vertex_shader, vertex, &quad_vertex_override)
      && create(__uv_scale_shadow_vertex_shader, vertex, &shadow_vertex_override)
      && create(__uv_scale_copy_vertex_shader, vertex, &copy_vertex_override)
      && create(__motion_blur_off_pixel_shader, pixel, &motion_blur_off_override)
      && create(__water_scaled_pixel_shader, pixel, &water_override)
      && create(__light_shaft_scaled_pixel_shader, pixel, &light_shaft_override)
      && create(__ssao_scaled_pixel_shader, pixel, &ssao_override)
      && create(__dof_blur_16tap_pixel_shader, pixel, &dof_blur_override)
      && create(__dof_combine_pixel_shader, pixel, &dof_combine_override);
  if (!override_pipelines_ready) LogMessage(reshade::log::level::error, "could not create the replacement shader pipelines");
  return override_pipelines_ready;
}

// ---- Constant buffers of the game ----

// Reads the unjittered camera (projection + view) from the scene vertex constant buffer b0, mirrored by the buffer handlers.
inline void CaptureCamera(const CommandListState& state) {
  const auto& bound = state.vs_constant_buffers[0];
  const auto found = camera_buffer_contents.find(bound.handle);
  constexpr size_t BYTES = 52 * sizeof(float);
  if (bound.handle == 0 || found == camera_buffer_contents.end() || found->second.size() < bound.offset + BYTES) return;

  std::array<float, 52> f = {};
  std::memcpy(f.data(), found->second.data() + bound.offset, BYTES);

  Camera camera;
  camera.proj_x = f[0];
  camera.proj_y = f[5];
  camera.depth_a = f[10];
  camera.depth_b = -f[11];
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) camera.rotation[i][j] = f[32 + 4 * i + j];
    camera.translation[i] = f[32 + 4 * i + 3];
    camera.position[i] = f[28 + i];
  }

  bool sane = camera.proj_x > 0.f && camera.proj_y > 0.f && camera.depth_a > 1.f && camera.depth_b > 0.f;
  for (int i = 0; i < 3 && sane; i++) {
    const float length = std::sqrt(camera.rotation[i][0] * camera.rotation[i][0]
                                   + camera.rotation[i][1] * camera.rotation[i][1]
                                   + camera.rotation[i][2] * camera.rotation[i][2]);
    sane = std::fabs(length - 1.f) < 0.02f;
  }
  if (!sane) return;
  camera.valid = true;

  const bool consecutive = camera_current.valid && camera_capture_frame + 1 == frame_number;
  camera_previous = (consecutive ? camera_current : Camera{});
  camera_current = camera;
  camera_capture_frame = frame_number;
  camera_captured_this_frame = true;
  last_scene_frame = frame_number;
}

// A shader-readable view of the game's depth buffer for the depth of field replacements (bound at t10).
inline bool EnsureMainDepthView(reshade::api::device* device) {
  if (main_depth_resource.handle == 0 || !live_resources.contains(main_depth_resource.handle)) return false;
  if (main_depth_srv.handle != 0 && main_depth_srv_resource == main_depth_resource) return true;
  if (main_depth_srv.handle != 0) device->destroy_resource_view(main_depth_srv);
  main_depth_srv = {0u};
  const reshade::api::resource_view_desc desc(reshade::api::resource_view_type::texture_2d,
                                              reshade::api::format::r24_unorm_x8_uint, 0, 1, 0, 1);
  if (!device->create_resource_view(main_depth_resource, reshade::api::resource_usage::shader_resource, desc,
                                    &main_depth_srv)) {
    main_depth_srv = {0u};
    return false;
  }
  main_depth_srv_resource = main_depth_resource;
  return true;
}

// At the CoC draw the game's filter constants (cbFilter, slot b6) are bound: mirror them for the replacement shaders.
inline void CaptureDofConstants(const CommandListState& state) {
  const auto& bound = state.ps_constant_buffers[6];
  if (bound.handle == 0) return;
  tracked_filter_buffers.insert(bound.handle);  // contents are mirrored from the next write on
  const auto found = filter_buffer_contents.find(bound.handle);
  constexpr uint64_t NEEDED = 128;  // c0 .. c7
  if (found == filter_buffer_contents.end() || found->second.size() < bound.offset + NEEDED) return;
  const uint8_t* base = found->second.data() + bound.offset;
  float depth_param[4], near_far[4], no_blur[4], linear_factor[4];
  std::memcpy(depth_param, base + 0, 16);
  std::memcpy(near_far, base + 16, 16);
  std::memcpy(no_blur, base + 64, 16);
  std::memcpy(linear_factor, base + 112, 16);
  for (int i = 0; i < 4; i++) {
    uv_scale_values[8 + i] = depth_param[i];
    uv_scale_values[12 + i] = near_far[i];
  }
  uv_scale_values[16] = linear_factor[1];
  uv_scale_values[17] = no_blur[1];
  uv_scale_values[18] = 0.f;  // set to 1 by the draw that uses them
  const bool sane = std::isfinite(depth_param[0]) && std::isfinite(depth_param[1]) && depth_param[1] != 0.f
                    && std::isfinite(near_far[2]) && std::isfinite(near_far[3]) && near_far[3] != near_far[2];
  if (sane) dof_constants_frame = frame_number;
}

}  // namespace ds2

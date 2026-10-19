// The add-on's own passes: depth rebuild, effect stabiliser, and the upscale step (motion vectors, DLSS, sharpening).
#pragma once

#include "ds2_dlss.hpp"
#include "ds2_fsr.hpp"
#include "ds2_timing.hpp"
#include "ds2_dump.hpp"
#include "ds2_resources.hpp"

namespace ds2 {

// ---- Depth rebuild after upscaling ----
// The scene is rendered into the top-left [0, scale] part of the game's depth buffer. Draws that follow the upscaler (heat
// haze sprites, other depth-tested effects over the final image, the per-pixel depth of field) expect the depth of the
// full-size picture: a nearest-neighbour upsample of the low-resolution depth is written back into the same buffer (from a
// copy of it).
inline void ResetDepthRebuild(reshade::api::device* device, bool destroy) {
  if (destroy && device != nullptr) {
    if (depth_rebuild_copy_view.handle != 0) device->destroy_resource_view(depth_rebuild_copy_view);
    if (depth_rebuild_write_view.handle != 0) device->destroy_resource_view(depth_rebuild_write_view);
    if (depth_rebuild_copy.handle != 0) device->destroy_resource(depth_rebuild_copy);
  }
  depth_rebuild_copy_view = {0u};
  depth_rebuild_write_view = {0u};
  depth_rebuild_copy = {0u};
  depth_rebuild_source = {0u};
}

inline void RunDepthRebuild(reshade::api::command_list* cmd_list) {
  auto* device = cmd_list->get_device();
  if (main_depth_resource.handle == 0 || !live_resources.contains(main_depth_resource.handle)) return;
  const auto depth_desc = device->get_resource_desc(main_depth_resource);
  if (depth_desc.type != reshade::api::resource_type::texture_2d || depth_desc.texture.samples != 1) return;

  if (depth_rebuild_layout.handle == 0) {
    std::array<reshade::api::pipeline_layout_param, 2> params;
    params[0] = reshade::api::constant_range{.binding = 0,
                                             .dx_register_index = UV_SCALE_CONSTANT_SLOT,
                                             .dx_register_space = 0,
                                             .count = 4,
                                             .visibility = reshade::api::shader_stage::pixel};
    params[1] = reshade::api::descriptor_range{.binding = 0,
                                               .dx_register_index = 0,
                                               .dx_register_space = 0,
                                               .count = 1,
                                               .visibility = reshade::api::shader_stage::pixel,
                                               .type = reshade::api::descriptor_type::shader_resource_view};
    if (!device->create_pipeline_layout(2, params.data(), &depth_rebuild_layout)) {
      depth_rebuild_layout = {0u};
      return;
    }
  }
  if (depth_rebuild_pipeline.handle == 0) {
    auto topology = reshade::api::primitive_topology::triangle_list;
    reshade::api::rasterizer_desc rasterizer = {.cull_mode = reshade::api::cull_mode::none};
    reshade::api::blend_desc blend = {};
    reshade::api::depth_stencil_desc depth_stencil = {};
    depth_stencil.depth_enable = true;
    depth_stencil.depth_write_mask = true;
    depth_stencil.depth_func = reshade::api::compare_op::always;
    depth_stencil.stencil_enable = false;
    auto depth_format = reshade::api::format::d24_unorm_s8_uint;
    uint32_t vertex_count = 3;
    reshade::api::shader_desc vertex_shader = {.code = __fullscreen_vertex_shader.data(),
                                               .code_size = __fullscreen_vertex_shader.size()};
    reshade::api::shader_desc pixel_shader = {.code = __depth_rebuild_pixel_shader.data(),
                                              .code_size = __depth_rebuild_pixel_shader.size()};
    const std::array<reshade::api::pipeline_subobject, 8> subobjects = {{
        {reshade::api::pipeline_subobject_type::vertex_shader, 1, &vertex_shader},
        {reshade::api::pipeline_subobject_type::pixel_shader, 1, &pixel_shader},
        {reshade::api::pipeline_subobject_type::depth_stencil_format, 1, &depth_format},
        {reshade::api::pipeline_subobject_type::max_vertex_count, 1, &vertex_count},
        {reshade::api::pipeline_subobject_type::primitive_topology, 1, &topology},
        {reshade::api::pipeline_subobject_type::blend_state, 1, &blend},
        {reshade::api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer},
        {reshade::api::pipeline_subobject_type::depth_stencil_state, 1, &depth_stencil},
    }};
    if (!device->create_pipeline(depth_rebuild_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(),
                                 &depth_rebuild_pipeline)) {
      depth_rebuild_pipeline = {0u};
      LogMessage(reshade::log::level::error, "could not create the depth rebuild pipeline");
      return;
    }
  }
  if (depth_rebuild_source != main_depth_resource || depth_rebuild_copy.handle == 0) {
    ResetDepthRebuild(device, true);
    reshade::api::resource_desc copy_desc = depth_desc;
    copy_desc.heap = reshade::api::memory_heap::gpu_only;
    copy_desc.usage = reshade::api::resource_usage::shader_resource | reshade::api::resource_usage::copy_dest;
    const reshade::api::resource_view_desc read_desc(reshade::api::resource_view_type::texture_2d,
                                                     reshade::api::format::r24_unorm_x8_uint, 0, 1, 0, 1);
    const reshade::api::resource_view_desc write_desc(reshade::api::resource_view_type::texture_2d,
                                                      reshade::api::format::d24_unorm_s8_uint, 0, 1, 0, 1);
    if (!device->create_resource(copy_desc, nullptr, reshade::api::resource_usage::shader_resource, &depth_rebuild_copy)
        || !device->create_resource_view(depth_rebuild_copy, reshade::api::resource_usage::shader_resource, read_desc,
                                         &depth_rebuild_copy_view)
        || !device->create_resource_view(main_depth_resource, reshade::api::resource_usage::depth_stencil, write_desc,
                                         &depth_rebuild_write_view)) {
      ResetDepthRebuild(device, true);
      LogMessage(reshade::log::level::error, "could not create the depth rebuild resources");
      return;
    }
    depth_rebuild_source = main_depth_resource;
  }

  std::optional<renodx::utils::state::CommandListSnapshot> previous_state = renodx::utils::state::GetSnapshot(cmd_list);
  cmd_list->copy_resource(main_depth_resource, depth_rebuild_copy);
  cmd_list->bind_pipeline(reshade::api::pipeline_stage::all_graphics, depth_rebuild_pipeline);
  cmd_list->bind_render_targets_and_depth_stencil(0, nullptr, depth_rebuild_write_view);
  const reshade::api::viewport viewport = {0.f, 0.f, static_cast<float>(depth_desc.texture.width),
                                           static_cast<float>(depth_desc.texture.height), 0.f, 1.f};
  const reshade::api::rect scissor = {0, 0, static_cast<int32_t>(depth_desc.texture.width),
                                      static_cast<int32_t>(depth_desc.texture.height)};
  cmd_list->bind_viewports(0, 1, &viewport);
  cmd_list->bind_scissor_rects(0, 1, &scissor);
  cmd_list->push_constants(reshade::api::shader_stage::pixel, depth_rebuild_layout, 0, 0, 4, uv_scale_values.data());
  const reshade::api::resource_view source_view = depth_rebuild_copy_view;
  cmd_list->push_descriptors(reshade::api::shader_stage::pixel, depth_rebuild_layout, 1,
                             {.binding = 0,
                              .count = 1,
                              .type = reshade::api::descriptor_type::shader_resource_view,
                              .descriptors = &source_view});
  cmd_list->draw(3, 1, 0, 0);
  if (previous_state.has_value()) previous_state->Apply();
}

// ---- Effect stabiliser ----
// Blends one effect map with its previous stabilised version (called before the draw that follows the effect's draw).
inline void RunStabilizer(reshade::api::command_list* cmd_list, int index) {
  auto& effect = stabilized_effects[index];
  auto* device = cmd_list->get_device();
  if (effect.target.handle == 0 || !live_resources.contains(effect.target.handle)) return;
  const auto target_desc = device->get_resource_desc(effect.target);
  if (target_desc.type != reshade::api::resource_type::texture_2d || target_desc.texture.width < 8
      || target_desc.texture.height < 8) {
    return;
  }

  if (effect.textures[0].handle == 0 || effect.width != target_desc.texture.width
      || effect.height != target_desc.texture.height || effect.format != target_desc.texture.format) {
    for (int i = 0; i < 2; i++) {
      effect.passes[i].DestroyAll(device);
      effect.passes[i] = {};
      effect.pass_target[i] = {0u};
      if (effect.textures[i].handle != 0) device->destroy_resource(effect.textures[i]);
      effect.textures[i] = {0u};
    }
    const reshade::api::resource_desc desc(reshade::api::resource_type::texture_2d, target_desc.texture.width,
                                           target_desc.texture.height, 1, 1, target_desc.texture.format, 1,
                                           reshade::api::memory_heap::gpu_only,
                                           reshade::api::resource_usage::render_target
                                               | reshade::api::resource_usage::shader_resource
                                               | reshade::api::resource_usage::copy_dest
                                               | reshade::api::resource_usage::copy_source);
    for (int i = 0; i < 2; i++) {
      if (!device->create_resource(desc, nullptr, reshade::api::resource_usage::shader_resource, &effect.textures[i])) {
        effect.textures = {};
        return;
      }
    }
    effect.width = target_desc.texture.width;
    effect.height = target_desc.texture.height;
    effect.format = target_desc.texture.format;
    effect.history_index = 0;
    effect.last_frame = 0;
  }
  if (stabilize_constants.handle == 0) {
    const reshade::api::resource_desc desc(reshade::api::resource_type::texture_2d, 1, 1, 1, 1,
                                           reshade::api::format::r32g32b32a32_float, 1,
                                           reshade::api::memory_heap::gpu_only,
                                           reshade::api::resource_usage::shader_resource
                                               | reshade::api::resource_usage::copy_dest);
    if (!device->create_resource(desc, nullptr, reshade::api::resource_usage::shader_resource, &stabilize_constants)) {
      stabilize_constants = {0u};
      return;
    }
  }

  const bool history_valid = (effect.last_frame != 0 && effect.last_frame + 1 == frame_number && frame_scaled
                              && last_upscaled_frame + 1 >= frame_number);
  const float motion_factor = 1.f - std::clamp((camera_motion_pixels - 1.f) / 6.f, 0.f, 1.f);
  std::array<float, 4> params = {history_valid ? tuning.stabilize_weight * motion_factor : 0.f, 0.f, 0.f, 0.f};
  const reshade::api::subresource_data params_data = {
      .data = params.data(), .row_pitch = sizeof(params), .slice_pitch = sizeof(params)};
  device->update_texture_region(params_data, stabilize_constants, 0, nullptr);

  const int history_index = effect.history_index;
  const int scratch_index = 1 - history_index;
  auto& pass = effect.passes[history_index];
  if (effect.pass_target[history_index] != effect.target) {
    pass.DestroyAll(device);
    pass = {};
    const reshade::api::resource_view_desc color_view(reshade::api::resource_view_type::texture_2d, effect.format, 0, 1,
                                                      0, 1);
    const reshade::api::resource_view_desc params_view(reshade::api::resource_view_type::texture_2d,
                                                       reshade::api::format::r32g32b32a32_float, 0, 1, 0, 1);
    pass.render_target_slots.resources = {effect.textures[scratch_index]};
    pass.render_target_slots.view_descs = {color_view};
    pass.shader_resource_slots.resources = {effect.target, effect.textures[history_index], stabilize_constants};
    pass.shader_resource_slots.view_descs = {color_view, color_view, params_view};
    pass.pipeline_subobjects.vertex_shader = __fullscreen_vertex_shader;
    pass.pipeline_subobjects.pixel_shader = __effect_stabilize_pixel_shader;
    pass.render_target_load_op = reshade::api::render_pass_load_op::discard;
    pass.revert_state_after_render = true;
    effect.pass_target[history_index] = effect.target;
  }

  const bool was_in_own_pass = in_own_pass;
  in_own_pass = true;
  const bool rendered = pass.Render(cmd_list, nullptr);
  in_own_pass = was_in_own_pass;
  if (!rendered) return;
  cmd_list->copy_resource(effect.textures[scratch_index], effect.target);  // downstream passes read the smoothed map
  effect.history_index = scratch_index;
  effect.last_frame = frame_number;
}

// ---- Upscale step ----

// Texel layout of constants_texture (motion_vector_constants, 4 floats per texel).
namespace constant_texel {
constexpr int RELATIVE_ROWS = 0;  // 0..2: current view -> previous view (xyz rotation row, w translation)
constexpr int PROJECTION = 3;     // x/y projection diagonal, z/w depth = z - w / view_z
constexpr int PROJECTION_PREVIOUS = 4;
constexpr int SCREEN = 5;         // render rectangle: width, height, 1/width, 1/height
constexpr int MASK_OPTIONS = 6;   // y: stencil mask of moving objects
constexpr int MASK_STRENGTH = 8;  // x: DLSS bias current colour strength
constexpr int SHARPEN = 9;        // x: sharpness 0..1
constexpr int RESAMPLE = 10;      // xy: render size / full size (unscale resample pass)
constexpr int ENGINE = 11;        // x: use the game's velocity map on moving objects, y: its gain, zw: map size in texels
}  // namespace constant_texel

// Pass that draws `resources` into `target` with the given shader; rebuilt only when the resources change.
inline void SetupPass(PassSlot* slot, std::span<const uint8_t> pixel_shader, reshade::api::resource target,
                      reshade::api::format target_format, std::initializer_list<reshade::api::resource> inputs,
                      std::initializer_list<reshade::api::resource_view_desc> input_views) {
  auto& pass = slot->pass;
  pass.render_target_slots.resources = {target};
  pass.render_target_slots.view_descs = {reshade::api::resource_view_desc(reshade::api::resource_view_type::texture_2d,
                                                                          target_format, 0, 1, 0, 1)};
  pass.shader_resource_slots.resources = inputs;
  pass.shader_resource_slots.view_descs = input_views;
  pass.pipeline_subobjects.vertex_shader = __fullscreen_vertex_shader;
  pass.pipeline_subobjects.pixel_shader = pixel_shader;
  pass.render_target_load_op = reshade::api::render_pass_load_op::discard;
  pass.revert_state_after_render = true;
}

inline reshade::api::resource_view_desc ViewDesc(reshade::api::format format) {
  return reshade::api::resource_view_desc(reshade::api::resource_view_type::texture_2d, format, 0, 1, 0, 1);
}

// A texture drawn through the shrunk viewport holds its picture in the top-left [0, scale] part only. Game shaders that run
// after the upscaler (the motion blur) sample such textures with full-range UVs, so they get a bilinear full-range copy
// instead. Returns the view to bind, or 0 when `source_view_handle` is not such a texture (or the copy cannot be made).
inline uint64_t ResampleScaledTexture(reshade::api::command_list* cmd_list, uint64_t source_view_handle) {
  auto* device = cmd_list->get_device();
  const reshade::api::resource_view source_view = {source_view_handle};
  const auto source = device->get_resource_from_view(source_view);
  if (source.handle == 0 || !scaled_written_resources.contains(source.handle) || !live_resources.contains(source.handle)
      || constants_texture.handle == 0) {
    return 0;
  }
  const auto desc = device->get_resource_desc(source);
  if (desc.type != reshade::api::resource_type::texture_2d || desc.texture.samples != 1 || desc.texture.depth_or_layers != 1) {
    return 0;
  }
  const auto view_format = device->get_resource_view_desc(source_view).format;
  auto& slot = resample_slots[source.handle];
  if (slot.scratch.handle == 0 || slot.width != desc.texture.width || slot.height != desc.texture.height
      || slot.format != desc.texture.format) {
    slot.pass.pass.DestroyAll(device);
    slot.pass = {};
    if (slot.scratch_view.handle != 0) device->destroy_resource_view(slot.scratch_view);
    if (slot.scratch.handle != 0) device->destroy_resource(slot.scratch);
    slot.scratch = {0u};
    slot.scratch_view = {0u};
    const reshade::api::resource_desc scratch_desc(reshade::api::resource_type::texture_2d, desc.texture.width,
                                                   desc.texture.height, 1, 1, desc.texture.format, 1,
                                                   reshade::api::memory_heap::gpu_only,
                                                   reshade::api::resource_usage::render_target
                                                       | reshade::api::resource_usage::shader_resource);
    const reshade::api::resource_view_desc scratch_view_desc(reshade::api::resource_view_type::texture_2d, view_format, 0, 1, 0, 1);
    if (!device->create_resource(scratch_desc, nullptr, reshade::api::resource_usage::shader_resource, &slot.scratch)
        || !device->create_resource_view(slot.scratch, reshade::api::resource_usage::shader_resource, scratch_view_desc,
                                         &slot.scratch_view)) {
      if (slot.scratch.handle != 0) device->destroy_resource(slot.scratch);
      slot.scratch = {0u};
      slot.scratch_view = {0u};
      return 0;
    }
    slot.width = desc.texture.width;
    slot.height = desc.texture.height;
    slot.format = desc.texture.format;
    slot.source_format = reshade::api::format::unknown;
  }
  if (slot.pass.first != source || slot.source_format != view_format) {
    slot.pass.pass.DestroyAll(device);
    slot.pass = {};
    slot.pass.first = source;
    slot.pass.target = slot.scratch;
    slot.source_format = view_format;
    SetupPass(&slot.pass, __unscale_resample_pixel_shader, slot.scratch, view_format, {source, constants_texture},
              {ViewDesc(view_format), ViewDesc(reshade::api::format::r32g32b32a32_float)});
  }
  const bool was_in_own_pass = in_own_pass;
  in_own_pass = true;
  const bool rendered = slot.pass.pass.Render(cmd_list, nullptr);
  in_own_pass = was_in_own_pass;
  return rendered ? slot.scratch_view.handle : 0;
}

// Writes the DLSS output over the scene texture, with contrast adaptive sharpening when it is switched on.
inline void WriteBackUpscaled(reshade::api::command_list* cmd_list, reshade::api::device* device,
                              reshade::api::resource scene_resource) {
  if (settings.upscaler == 0 && settings.image_sharpening && settings.image_sharpening_strength > 0.f) {  // (FSR sharpens with its own RCAS pass)
    auto& slot = sharpen_slot;
    if (slot.first != upscaled_texture || slot.target != scene_resource) {
      ResetPassSlot(device, &slot);
      slot.first = upscaled_texture;
      slot.target = scene_resource;
      SetupPass(&slot, __image_sharpen_pixel_shader, scene_resource, reshade::api::format::r16g16b16a16_float,
                {upscaled_texture, constants_texture},
                {ViewDesc(reshade::api::format::r16g16b16a16_float), ViewDesc(reshade::api::format::r32g32b32a32_float)});
    }
    if (slot.pass.Render(cmd_list, nullptr)) return;
  }
  cmd_list->copy_resource(upscaled_texture, scene_resource);
}

// Builds the motion vectors, runs DLSS on the scene and copies the result back into the scene texture.
inline void RunUpscale(reshade::api::command_list* cmd_list, uint64_t scene_view_handle) {
  auto* device = cmd_list->get_device();
  if (!camera_captured_this_frame || !camera_current.valid) {
    status_text = "no camera captured this frame";
    return;
  }
  if (main_depth_resource.handle == 0 || !live_resources.contains(main_depth_resource.handle)) {
    main_depth_resource = {0u};
    status_text = "depth buffer not seen yet";
    return;
  }
  if (scene_view_handle == 0 || !live_views.contains(scene_view_handle)) {
    status_text = "scene texture binding unknown";
    return;
  }
  const auto scene_resource = device->get_resource_from_view({scene_view_handle});
  if (scene_resource.handle == 0 || !live_resources.contains(scene_resource.handle)) {
    status_text = "scene texture is gone";
    return;
  }
  const auto scene_desc = device->get_resource_desc(scene_resource);
  if (scene_desc.type != reshade::api::resource_type::texture_2d || scene_desc.texture.width != back_buffer_width
      || scene_desc.texture.height != back_buffer_height
      || scene_desc.texture.format != reshade::api::format::r16g16b16a16_float) {
    status_text = "pass input is not a full-size RGBA16F scene";
    return;
  }

  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  auto* native_device = reinterpret_cast<ID3D11Device*>(device->get_native());
  if (context == nullptr || native_device == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
    status_text = "not the immediate context";
    return;
  }

  if (scaling_active && !frame_scaled) {
    // The scene was not drawn at the lower size from the start of this frame (first scene frame after a menu, or a mode
    // change from a non-scene frame): its textures are full size, so leave this frame as the game rendered it.
    status_text = "waiting for a scene frame to start scaling";
    return;
  }
  const uint32_t render_w = (frame_scaled ? render_width : scene_desc.texture.width);
  const uint32_t render_h = (frame_scaled ? render_height : scene_desc.texture.height);
  if (!EnsureTextures(device, scene_desc.texture.width, scene_desc.texture.height)) {
    status_text = "could not create upscaler textures";
    return;
  }
  if (settings.upscaler == 0 && !EnsureDlssFeature(native_device, context, render_w, render_h, scene_desc.texture.width, scene_desc.texture.height)) {
    return;
  }

  // History is not valid after a camera cut / teleport or when the feature was just (re)created.
  bool history_valid = camera_previous.valid && last_upscaled_frame != 0 && last_upscaled_frame + 1 == frame_number;
  if (history_valid) {
    float distance_squared = 0.f;
    for (int i = 0; i < 3; i++) {
      const float delta = camera_current.position[i] - camera_previous.position[i];
      distance_squared += delta * delta;
    }
    if (distance_squared > tuning.camera_cut_distance * tuning.camera_cut_distance) history_valid = false;
  }

  // Current view space -> previous view space: R_prev * R_cur^T and t_prev - R_rel * t_cur.
  const Camera& previous = camera_previous.valid ? camera_previous : camera_current;
  float relative[3][3] = {};
  float relative_translation[3] = {};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      for (int k = 0; k < 3; k++) relative[i][j] += previous.rotation[i][k] * camera_current.rotation[j][k];
    }
  }
  for (int i = 0; i < 3; i++) {
    relative_translation[i] = previous.translation[i];
    for (int j = 0; j < 3; j++) relative_translation[i] -= relative[i][j] * camera_current.translation[j];
  }
  {
    // Screen motion of the camera in render pixels (rotation only, small-angle): how much the effect maps move.
    const float trace = relative[0][0] + relative[1][1] + relative[2][2];
    const float angle = std::acos(std::clamp((trace - 1.f) * 0.5f, -1.f, 1.f));
    camera_motion_pixels = angle * 0.5f * static_cast<float>(render_h) * camera_current.proj_y;
  }

  auto& c = motion_vector_constants;
  for (int i = 0; i < 3; i++) {
    c[4 * (constant_texel::RELATIVE_ROWS + i) + 0] = relative[i][0];
    c[4 * (constant_texel::RELATIVE_ROWS + i) + 1] = relative[i][1];
    c[4 * (constant_texel::RELATIVE_ROWS + i) + 2] = relative[i][2];
    c[4 * (constant_texel::RELATIVE_ROWS + i) + 3] = relative_translation[i];
  }
  c[4 * constant_texel::PROJECTION + 0] = camera_current.proj_x;
  c[4 * constant_texel::PROJECTION + 1] = camera_current.proj_y;
  c[4 * constant_texel::PROJECTION + 2] = camera_current.depth_a;
  c[4 * constant_texel::PROJECTION + 3] = camera_current.depth_b;
  c[4 * constant_texel::PROJECTION_PREVIOUS + 0] = previous.proj_x;
  c[4 * constant_texel::PROJECTION_PREVIOUS + 1] = previous.proj_y;
  // Motion vectors, depth and colour live in the render rectangle (top-left of the full-size textures).
  c[4 * constant_texel::SCREEN + 0] = static_cast<float>(render_w);
  c[4 * constant_texel::SCREEN + 1] = static_cast<float>(render_h);
  c[4 * constant_texel::SCREEN + 2] = 1.f / static_cast<float>(render_w);
  c[4 * constant_texel::SCREEN + 3] = 1.f / static_cast<float>(render_h);
  c[4 * constant_texel::MASK_OPTIONS + 1] = static_cast<float>(STENCIL_MOVING_BIT);
  const float character_mask_strength = (settings.upscaler == 1 ? tuning.fsr_character_reactive : tuning.character_bias);
  const float foliage_mask_strength = (settings.upscaler == 1 ? tuning.fsr_foliage_reactive : 0.f);
  c[4 * constant_texel::MASK_STRENGTH + 0] = character_mask_strength;
  c[4 * constant_texel::MASK_STRENGTH + 1] = foliage_mask_strength;
  c[4 * constant_texel::MASK_OPTIONS + 2] = (settings.upscaler == 1 ? static_cast<float>(STENCIL_FOLIAGE_BIT) : 0.f);
  c[4 * constant_texel::SHARPEN + 0] = std::clamp(settings.image_sharpening_strength, 0.f, 100.f) / 100.f;
  c[4 * constant_texel::RESAMPLE + 0] = scale_x;
  c[4 * constant_texel::RESAMPLE + 1] = scale_y;

  const auto depth_view = ViewDesc(reshade::api::format::r24_unorm_x8_uint);
  const auto stencil_view = ViewDesc(reshade::api::format::x24_unorm_g8_uint);
  const auto constants_view = ViewDesc(reshade::api::format::r32g32b32a32_float);

  // The game's velocity map (only there while its Motion Blur option is on) replaces the camera-only vector on moving objects.
  // DLSS: only presets J and K (L and M cope with camera-only vectors). FSR always.
  const bool engine_active = (settings.upscaler == 1 || settings.preset == 10 || settings.preset == 11) && engine_velocity_resource.handle != 0
                             && live_resources.contains(engine_velocity_resource.handle);
  if (engine_velocity_dummy.handle == 0) {
    const reshade::api::resource_desc dummy_desc(reshade::api::resource_type::texture_2d, 1, 1, 1, 1,
                                                 reshade::api::format::r8g8b8a8_unorm, 1, reshade::api::memory_heap::gpu_only,
                                                 reshade::api::resource_usage::shader_resource);
    static uint32_t dummy_texel = 0xFFFFFFFFu;
    const reshade::api::subresource_data dummy_data = {.data = &dummy_texel, .row_pitch = 4, .slice_pitch = 4};
    if (!device->create_resource(dummy_desc, &dummy_data, reshade::api::resource_usage::shader_resource, &engine_velocity_dummy)) {
      engine_velocity_dummy = {0u};
      status_text = "could not create the velocity placeholder";
      return;
    }
  }
  const reshade::api::resource velocity_input = (engine_active ? engine_velocity_resource : engine_velocity_dummy);
  c[4 * constant_texel::ENGINE + 0] = engine_active ? 1.f : 0.f;
  c[4 * constant_texel::ENGINE + 1] = 0.5f;  // the game's velocity is twice the real frame-to-frame motion (measured)
  c[4 * constant_texel::ENGINE + 2] = static_cast<float>(engine_active ? engine_velocity_width : 1u);
  c[4 * constant_texel::ENGINE + 3] = static_cast<float>(engine_active ? engine_velocity_height : 1u);

  auto& motion_slot = motion_vector_slot;
  if (motion_slot.first != main_depth_resource || motion_slot.target != motion_vector_texture
      || motion_slot.second != velocity_input) {
    ResetPassSlot(device, &motion_slot);
    motion_slot.first = main_depth_resource;
    motion_slot.second = velocity_input;
    motion_slot.target = motion_vector_texture;
    SetupPass(&motion_slot, __motion_vector_pixel_shader, motion_vector_texture, reshade::api::format::r16g16_float,
              {main_depth_resource, constants_texture, main_depth_resource, velocity_input},
              {depth_view, constants_view, stencil_view, ViewDesc(reshade::api::format::r8g8b8a8_unorm)});
  }
  {
    const reshade::api::subresource_data constants_data = {
        .data = motion_vector_constants.data(),
        .row_pitch = static_cast<uint32_t>(motion_vector_constants.size() * sizeof(float)),
        .slice_pitch = static_cast<uint32_t>(motion_vector_constants.size() * sizeof(float)),
    };
    device->update_texture_region(constants_data, constants_texture, 0, nullptr);
  }

  GpuTimerBegin(native_device, context);
  in_own_pass = true;
  if (!motion_slot.pass.Render(cmd_list, nullptr)) {
    in_own_pass = false;
    status_text = "motion vector pass failed";
    return;
  }

  // Bias-current-colour mask for moving objects (stencil bit), a second small pass in front of DLSS.
  bool use_bias_mask = false;
  if (character_mask_strength > 0.001f || foliage_mask_strength > 0.001f) {
    if (bias_mask_texture.handle == 0 || bias_mask_width != texture_width || bias_mask_height != texture_height) {
      ResetPassSlot(device, &bias_mask_slot);
      if (bias_mask_texture.handle != 0) device->destroy_resource(bias_mask_texture);
      bias_mask_texture = {0u};
      const reshade::api::resource_desc mask_desc(reshade::api::resource_type::texture_2d, texture_width, texture_height,
                                                  1, 1, reshade::api::format::r8_unorm, 1,
                                                  reshade::api::memory_heap::gpu_only,
                                                  reshade::api::resource_usage::render_target
                                                      | reshade::api::resource_usage::shader_resource);
      if (device->create_resource(mask_desc, nullptr, reshade::api::resource_usage::shader_resource,
                                  &bias_mask_texture)) {
        bias_mask_width = texture_width;
        bias_mask_height = texture_height;
      } else {
        bias_mask_texture = {0u};
      }
    }
    if (bias_mask_texture.handle != 0) {
      auto& mask_slot = bias_mask_slot;
      if (mask_slot.first != main_depth_resource || mask_slot.target != bias_mask_texture) {
        ResetPassSlot(device, &mask_slot);
        mask_slot.first = main_depth_resource;
        mask_slot.target = bias_mask_texture;
        SetupPass(&mask_slot, __bias_mask_pixel_shader, bias_mask_texture, reshade::api::format::r8_unorm,
                  {main_depth_resource, constants_texture}, {stencil_view, constants_view});
      }
      use_bias_mask = mask_slot.pass.Render(cmd_list, nullptr);
    }
  }

  DumpCaptureInputs(device, cmd_list, scene_resource, render_w, render_h, scene_desc.texture.width, scene_desc.texture.height);
  GpuTimerStamp(context, 1);

  if (settings.upscaler == 1) {
    // The overlay strength (0..100) drives FSR's RCAS sharpness p (0 = no sharpening pass at all). RCAS is weaker than the filter used with DLSS for the
    // same number, so the curve rises quickly (20 -> 0.95) and saturates at 1.1: AMD's nominal maximum is 1, artifacts appear from about 1.2.
    const float cas_strength = std::clamp(settings.image_sharpening_strength, 0.f, 100.f) / 100.f;
    const float overlay_sharpness = 1.1f * (1.f - std::exp(-cas_strength / 0.1f));
    const bool reset_fsr = !history_valid || reset_history_request;
    reset_history_request = false;
    if (!RunFsr(native_device, context, reinterpret_cast<ID3D11Resource*>(scene_resource.handle),
                reinterpret_cast<ID3D11Resource*>(main_depth_resource.handle),
                reinterpret_cast<ID3D11Resource*>(motion_vector_texture.handle),
                reinterpret_cast<ID3D11Resource*>(upscaled_texture.handle),
                use_bias_mask ? reinterpret_cast<ID3D11Resource*>(bias_mask_texture.handle) : nullptr,
                render_w, render_h, scene_desc.texture.width, scene_desc.texture.height, reset_fsr,
                settings.image_sharpening ? overlay_sharpness : tuning.fsr_sharpness)) {
      in_own_pass = false;
      static std::string fsr_failure;
      fsr_failure = "FSR failed: " + fsr.message + " (switched back to DLSS)";
      settings.upscaler = 0;  // not saved: the next frame runs DLSS instead of leaving the scene un-upscaled
      status_text = fsr_failure.c_str();
      return;
    }
    GpuTimerStamp(context, 2);
    DumpCaptureOutput(device, cmd_list);
    WriteBackUpscaled(cmd_list, device, scene_resource);
    if (frame_scaled) RunDepthRebuild(cmd_list);
    GpuTimerEnd(context);
    DumpCaptureDepthAfter(device, cmd_list);
    in_own_pass = false;
    last_upscaled_frame = frame_number;
    frames_upscaled++;
    status_text = reset_fsr ? "FSR running (history reset)" : "FSR running";
    status_running = true;
    return;
  }

  NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
  eval.Feature.pInColor = reinterpret_cast<ID3D11Resource*>(scene_resource.handle);
  eval.Feature.pInOutput = reinterpret_cast<ID3D11Resource*>(upscaled_texture.handle);
  eval.pInDepth = reinterpret_cast<ID3D11Resource*>(main_depth_resource.handle);
  eval.pInMotionVectors = reinterpret_cast<ID3D11Resource*>(motion_vector_texture.handle);
  eval.pInBiasCurrentColorMask = (use_bias_mask ? reinterpret_cast<ID3D11Resource*>(bias_mask_texture.handle) : nullptr);
  eval.InJitterOffsetX = jitter_x;
  eval.InJitterOffsetY = jitter_y;
  eval.InRenderSubrectDimensions = {render_w, render_h};
  eval.InReset = (history_valid && !reset_history_request) ? 0 : 1;
  eval.InMVScaleX = 1.f;
  eval.InMVScaleY = 1.f;
  eval.pInExposureTexture = reinterpret_cast<ID3D11Resource*>(exposure_texture.handle);
  eval.InPreExposure = 1.f;
  eval.InExposureScale = 1.f;
  reset_history_request = false;

  const NVSDK_NGX_Result evaluate_result = NGX_D3D11_EVALUATE_DLSS_EXT(context, dlss.feature, dlss.parameters, &eval);
  if (NVSDK_NGX_FAILED(evaluate_result)) {
    in_own_pass = false;
    evaluate_failures++;
    last_ngx_message = "DLSS evaluate failed: " + NgxResultText(evaluate_result);
    status_text = "DLSS evaluate failed";
    if (evaluate_failures == 1 || evaluate_failures % 300 == 0) LogMessage(reshade::log::level::error, last_ngx_message);
    return;
  }

  GpuTimerStamp(context, 2);
  DumpCaptureOutput(device, cmd_list);
  WriteBackUpscaled(cmd_list, device, scene_resource);
  if (frame_scaled) RunDepthRebuild(cmd_list);
  GpuTimerEnd(context);
  DumpCaptureDepthAfter(device, cmd_list);
  in_own_pass = false;

  last_upscaled_frame = frame_number;
  frames_upscaled++;
  status_text = history_valid ? "DLSS running" : "DLSS running (history reset)";
  status_running = true;
}

}  // namespace ds2

// ReShade event handlers: they mirror what the game binds, decide per draw which replacement applies, trigger the upscale
// step, and keep the per-frame bookkeeping at present.
#pragma once

#include "ds2_hud.hpp"
#include "ds2_passes.hpp"

namespace ds2 {

// ---- Mirrors of the game's state ----

inline CommandListState& StateFor(reshade::api::command_list* cmd_list) {
  static reshade::api::command_list* cached_list = nullptr;
  static CommandListState* cached_state = nullptr;
  if (cmd_list != cached_list || cached_state == nullptr) {
    cached_state = &command_list_states[cmd_list];  // unordered_map nodes keep their address
    cached_list = cmd_list;
  }
  return *cached_state;
}

inline void OnInitPipeline(reshade::api::device* device, reshade::api::pipeline_layout layout, uint32_t subobject_count,
                           const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline) {
  DS2_PROBE(ResourceEvents);
  for (uint32_t i = 0; i < subobject_count; i++) {
    if (subobjects[i].type == reshade::api::pipeline_subobject_type::input_layout) {
      const auto* elements = static_cast<const reshade::api::input_element*>(subobjects[i].data);
      for (uint32_t e = 0; elements != nullptr && e < subobjects[i].count; e++) {
        if (elements[e].semantic == nullptr || std::strcmp(elements[e].semantic, "POSITION") != 0 || elements[e].semantic_index != 0) continue;
        uint32_t components = 0;
        switch (elements[e].format) {
          case reshade::api::format::r32g32b32a32_float: components = 4; break;
          case reshade::api::format::r32g32b32_float: components = 3; break;
          case reshade::api::format::r32g32_float: components = 2; break;
          default: break;
        }
        DS2_LOCK_OR_RETURN();
        position_layouts[pipeline.handle] = {elements[e].offset, components};
      }
      continue;
    }
    const bool is_pixel = (subobjects[i].type == reshade::api::pipeline_subobject_type::pixel_shader);
    const bool is_vertex = (subobjects[i].type == reshade::api::pipeline_subobject_type::vertex_shader);
    if (!is_pixel && !is_vertex) continue;
    const auto* shader = static_cast<const reshade::api::shader_desc*>(subobjects[i].data);
    if (shader == nullptr || shader->code == nullptr || shader->code_size == 0) continue;
    const uint32_t hash = ComputeCrc32(static_cast<const uint8_t*>(shader->code), shader->code_size);
    DS2_LOCK_OR_RETURN();
    (is_pixel ? pipeline_pixel_hash : pipeline_vertex_hash)[pipeline.handle] = hash;
  }
}

inline void OnBindPipeline(reshade::api::command_list* cmd_list, reshade::api::pipeline_stage stages,
                           reshade::api::pipeline pipeline) {
  DS2_PROBE(BindPipeline);
  if (in_own_pass) return;
  const bool pixel = HasFlag(stages, reshade::api::pipeline_stage::pixel_shader);
  const bool vertex = HasFlag(stages, reshade::api::pipeline_stage::vertex_shader);
  const bool layout = HasFlag(stages, reshade::api::pipeline_stage::input_assembler);
  if (!pixel && !vertex && !layout) return;
  DS2_LOCK_OR_RETURN();
  auto& state = StateFor(cmd_list);
  if (layout) state.input_layout = pipeline.handle;
  if (pixel) {
    const auto found = pipeline_pixel_hash.find(pipeline.handle);
    state.ps_hash = (found != pipeline_pixel_hash.end() ? found->second : 0u);
    state.game_ps_pipeline = pipeline.handle;
    state.ps_overridden = false;  // the game's own bind replaced ours
  }
  if (vertex) {
    const auto found = pipeline_vertex_hash.find(pipeline.handle);
    state.vs_hash = (found != pipeline_vertex_hash.end() ? found->second : 0u);
    state.game_vs_pipeline = pipeline.handle;
    state.vs_overridden = false;
  }
}

inline void OnPushDescriptors(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
                              reshade::api::pipeline_layout layout, uint32_t layout_param,
                              const reshade::api::descriptor_table_update& update) {
  DS2_PROBE(PushDescriptors);
  if (in_own_pass || update.descriptors == nullptr) return;
  const bool pixel = HasFlag(stages, reshade::api::shader_stage::pixel);
  if (update.type == reshade::api::descriptor_type::sampler) {
    if (!pixel) return;
    const auto* samplers = static_cast<const reshade::api::sampler*>(update.descriptors);
    DS2_LOCK_OR_RETURN();
    auto& state = StateFor(cmd_list);
    if (sampler_layout.handle == 0u) {
      sampler_layout = layout;
      sampler_layout_param = layout_param;
    }
    for (uint32_t i = 0; i < update.count; i++) {
      const uint32_t slot = update.binding + i;
      if (slot >= state.game_samplers.size()) break;
      state.game_samplers[slot] = samplers[i].handle;
      state.bound_samplers[slot] = samplers[i].handle;  // the game's bind replaced whatever we had there
    }
    state.sampler_dirty = true;
    return;
  }
  if (update.type == reshade::api::descriptor_type::shader_resource_view) {
    if (!pixel) return;
    const auto* views = static_cast<const reshade::api::resource_view*>(update.descriptors);
    DS2_LOCK_OR_RETURN();
    auto& state = StateFor(cmd_list);
    for (uint32_t i = 0; i < update.count; i++) {
      const uint32_t slot = update.binding + i;
      if (slot >= state.ps_srvs.size()) break;
      state.ps_srvs[slot] = views[i].handle;
    }
    return;
  }
  if (update.type != reshade::api::descriptor_type::constant_buffer) return;
  const auto* ranges = static_cast<const reshade::api::buffer_range*>(update.descriptors);
  DS2_LOCK_OR_RETURN();
  auto& state = StateFor(cmd_list);
  if (pixel) {
    for (uint32_t i = 0; i < update.count; i++) {
      const uint32_t slot = update.binding + i;
      if (slot >= state.ps_constant_buffers.size()) break;
      state.ps_constant_buffers[slot] = {
          .handle = ranges[i].buffer.handle, .offset = ranges[i].offset, .size = ranges[i].size};
    }
  }
  if (!HasFlag(stages, reshade::api::shader_stage::vertex)) return;
  for (uint32_t i = 0; i < update.count; i++) {
    const uint32_t slot = update.binding + i;
    if (slot >= state.vs_constant_buffers.size()) break;
    state.vs_constant_buffers[slot] = {
        .handle = ranges[i].buffer.handle, .offset = ranges[i].offset, .size = ranges[i].size};
  }
}

// Only buffers that look like the scene camera buffer (recognised when they are created, see OnInitResource) are copied, plus the
// DoF filter buffer once it is known, so unrelated constant buffers cost nothing.
inline void StoreCameraBytes(uint64_t handle, uint64_t offset, const void* data, uint64_t size) {
  if (data == nullptr || size == 0 || offset + size > SCENE_CAMERA_BUFFER_BYTES) return;
  auto& bytes = camera_buffer_contents[handle];
  if (bytes.size() < SCENE_CAMERA_BUFFER_BYTES) bytes.resize(SCENE_CAMERA_BUFFER_BYTES, 0);
  std::memcpy(bytes.data() + offset, data, static_cast<size_t>(size));
}

inline void StoreFilterBytes(uint64_t handle, uint64_t buffer_size, uint64_t offset, const void* data, uint64_t size) {
  if (data == nullptr || size == 0 || buffer_size == 0 || buffer_size > 65536 || offset + size > buffer_size) return;
  auto& bytes = filter_buffer_contents[handle];
  if (bytes.size() < buffer_size) bytes.resize(static_cast<size_t>(buffer_size), 0);
  std::memcpy(bytes.data() + offset, data, static_cast<size_t>(size));
}

// HUD scaling reads the quad vertices the game writes into a dynamic vertex buffer (WantsVertexMirror, ds2_hud.hpp).
// Only the buffers the HUD draws were seen reading from are mirrored (learned from the first HUD draw of a frame).
inline bool IsHudVertexBuffer(reshade::api::device* device, reshade::api::resource resource) {
  if (!hud_vertex_buffers.contains(resource.handle)) return false;
  const auto desc = device->get_resource_desc(resource);
  return desc.type == reshade::api::resource_type::buffer && desc.buffer.size <= (16u << 20);
}

// Copies the new vertices of a write into the mirror: only a window of the written range (see HudVertexWindow).
inline void StoreVertexBytes(uint64_t handle, uint64_t buffer_size, uint64_t offset, const void* data, uint64_t size) {
  if (data == nullptr || size == 0 || buffer_size == 0 || offset + size > buffer_size) return;
  auto& bytes = vertex_buffer_contents[handle];
  const uint64_t mirror_size = bytes.size();
  if (bytes.size() < buffer_size) bytes.resize(static_cast<size_t>(buffer_size), 0);
  uint64_t begin = 0;
  uint64_t end = 0;
  TakeHudWindow(&HudWindowFor(handle, mirror_size), offset, size, &begin, &end);
  std::memcpy(bytes.data() + begin, static_cast<const uint8_t*>(data) + (begin - offset), static_cast<size_t>(end - begin));
}

inline void OnMapBufferRegion(reshade::api::device* device, reshade::api::resource resource, uint64_t offset,
                              uint64_t size, reshade::api::map_access access, void** data) {
  DS2_PROBE(MapBuffer);
  if (access == reshade::api::map_access::read_only || data == nullptr || *data == nullptr) return;
  DS2_LOCK_OR_RETURN();
  const bool camera = camera_buffers.contains(resource.handle);
  const bool filter = !camera && tracked_filter_buffers.contains(resource.handle);
  const bool vertex = !camera && !filter && WantsVertexMirror() && IsHudVertexBuffer(device, resource);
  if (!camera && !filter && !vertex) return;
  if (vertex && access == reshade::api::map_access::write_discard) ResetHudWindow(resource.handle);  // the game starts a new fill
  const uint64_t buffer_size = ((filter || vertex) ? device->get_resource_desc(resource).buffer.size : SCENE_CAMERA_BUFFER_BYTES);
  mapped_regions[resource.handle] = {
      .data = static_cast<uint8_t*>(*data),
      .offset = offset,
      .size = (size == UINT64_MAX ? buffer_size : size),
      .filter = filter,
      .vertex = vertex,
      .buffer_size = buffer_size,
  };
}

inline void OnUnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource) {
  DS2_PROBE(UnmapBuffer);
  DS2_LOCK_OR_RETURN();
  const auto found = mapped_regions.find(resource.handle);
  if (found == mapped_regions.end()) return;
  const MappedRegion& region = found->second;
  if (region.vertex) {
    StoreVertexBytes(resource.handle, region.buffer_size, region.offset, region.data, region.size);
  } else if (region.filter) {
    StoreFilterBytes(resource.handle, region.buffer_size, region.offset, region.data, region.size);
  } else {
    StoreCameraBytes(resource.handle, region.offset, region.data, region.size);
  }
  mapped_regions.erase(found);
}

inline bool OnUpdateBufferRegion(reshade::api::device* device, const void* data, reshade::api::resource dest,
                                 uint64_t dest_offset, uint64_t size) {
  DS2_PROBE(UpdateBuffer);
  DS2_LOCK_OR_RETURN(false);
  if (camera_buffers.contains(dest.handle)) {
    StoreCameraBytes(dest.handle, dest_offset, data, (size == UINT64_MAX ? SCENE_CAMERA_BUFFER_BYTES : size));
  } else if (tracked_filter_buffers.contains(dest.handle)) {
    const uint64_t buffer_size = device->get_resource_desc(dest).buffer.size;
    StoreFilterBytes(dest.handle, buffer_size, dest_offset, data, (size == UINT64_MAX ? buffer_size : size));
  } else if (WantsVertexMirror() && IsHudVertexBuffer(device, dest)) {
    const uint64_t buffer_size = device->get_resource_desc(dest).buffer.size;
    StoreVertexBytes(dest.handle, buffer_size, dest_offset, data, (size == UINT64_MAX ? buffer_size : size));
  }
  return false;
}

inline bool OnUpdateBufferRegionCommand(reshade::api::command_list* cmd_list, const void* data,
                                        reshade::api::resource dest, uint64_t dest_offset, uint64_t size) {
  return OnUpdateBufferRegion(cmd_list->get_device(), data, dest, dest_offset, size);
}

inline void OnBindVertexBuffers(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
                                const reshade::api::resource* buffers, const uint64_t* offsets, const uint32_t* strides) {
  DS2_PROBE(VertexBuffers);
  if (!WantsVertexMirror() || first != 0 || count == 0) return;
  DS2_LOCK_OR_RETURN();
  auto& state = StateFor(cmd_list);
  state.vertex_buffer0 = buffers[0].handle;
  state.vertex_offset0 = (offsets != nullptr ? offsets[0] : 0);
  state.vertex_stride0 = (strides != nullptr ? strides[0] : 0);
}

inline void OnBindViewports(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
                            const reshade::api::viewport* viewports) {
  DS2_PROBE(Viewports);
  if (in_own_pass || first != 0 || count == 0) return;
  DS2_LOCK_OR_RETURN();
  auto& state = StateFor(cmd_list);
  state.game_viewport = viewports[0];
  state.has_viewport = true;
  state.viewport_overridden = false;  // the game's own bind replaced ours
}

// The game's HUD lists clip their text with a scissor rectangle: it has to follow the scaled HUD (ApplyHudScale).
inline void OnBindScissorRects(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
                               const reshade::api::rect* rects) {
  DS2_PROBE(Scissors);
  if (in_own_pass || rects == nullptr || count == 0) return;
  DS2_LOCK_OR_RETURN();
  auto& state = StateFor(cmd_list);
  for (uint32_t i = 0; i < count && first + i < state.game_scissors.size(); i++) state.game_scissors[first + i] = rects[i];
  state.game_scissor_count = std::max(state.game_scissor_count, std::min<uint32_t>(first + count, state.game_scissors.size()));
  state.scissor_overridden = false;  // the game's own bind replaced ours
}

inline void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count,
                                const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv) {
  DS2_PROBE(RenderTargets);
  if (in_own_pass) return;
  auto* device = cmd_list->get_device();
  DS2_LOCK_OR_RETURN();
  auto& state = StateFor(cmd_list);

  state.render_target_count = count;
  state.render_target_resource = 0;
  state.render_target_format = reshade::api::format::unknown;
  state.render_target_width = 0;
  state.render_target_height = 0;
  if (count > 0 && rtvs[0].handle != 0) {
    const auto target_resource = device->get_resource_from_view(rtvs[0]);
    const auto desc = device->get_resource_desc(target_resource);
    state.render_target_resource = target_resource.handle;
    state.render_target_format = device->get_resource_view_desc(rtvs[0]).format;
    state.render_target_width = desc.texture.width;
    state.render_target_height = desc.texture.height;
  }

  state.has_depth_stencil = (dsv.handle != 0);
  if (state.has_depth_stencil) {
    const auto depth_resource = device->get_resource_from_view(dsv);
    const auto desc = device->get_resource_desc(depth_resource);
    if (desc.texture.format == reshade::api::format::r24_g8_typeless && back_buffer_width != 0
        && desc.texture.width == back_buffer_width && desc.texture.height == back_buffer_height) {
      main_depth_resource = depth_resource;
    }
  }
}

// ---- Counting draws per pixel shader (only the ordinal of the draw before the tonemap is ever needed) ----
class HashCounter {
 public:
  uint32_t Increment(uint32_t key) {
    Slot& slot = Find(key);
    slot.count++;
    return slot.count;
  }
  uint32_t Get(uint32_t key) { return Find(key).count; }
  void Clear() {
    if (++generation_ == 0) {
      slots_ = {};
      generation_ = 1;
    }
  }

 private:
  struct Slot {
    uint32_t key = 0;
    uint32_t generation = 0;
    uint32_t count = 0;
  };
  Slot& Find(uint32_t key) {
    uint32_t index = (key * 2654435761u) >> 22;  // 1024 slots
    for (uint32_t probe = 0; probe < slots_.size(); probe++, index = (index + 1) & 1023u) {
      Slot& slot = slots_[index];
      if (slot.generation != generation_) {
        slot = {key, generation_, 0};
        return slot;
      }
      if (slot.key == key) return slot;
    }
    return slots_[0];  // table full: cannot happen with the game's few hundred distinct shaders
  }
  std::array<Slot, 1024> slots_ = {};
  uint32_t generation_ = 1;
};
inline HashCounter hash_occurrences;

// ---- Draw handling ----

// Camera: the first full-size scene draw of a frame carries the unjittered camera constants.
inline void CaptureCameraIfSceneDraw(const CommandListState& state) {
  if (camera_captured_this_frame || !state.has_depth_stencil || back_buffer_width == 0
      || state.render_target_format != reshade::api::format::r10g10b10a2_unorm
      || state.render_target_width != back_buffer_width || state.render_target_height != back_buffer_height) {
    return;
  }
  CaptureCamera(state);
}

// Upscaler trigger: runs DLSS at the first draw of the post chain blit, with the draw before the tonemap as fallback.
inline void TriggerUpscale(reshade::api::command_list* cmd_list, const CommandListState& state, uint32_t draw_hash,
                           uint32_t occurrence) {
  if (!settings.enabled || upscaled_this_frame || draw_hash == 0 || frame_holding) return;
  if (draw_hash == POST_CHAIN_BLIT_PIXEL_SHADER) {
    frames_without_anchor = 0;
    upscaled_this_frame = true;
    DS2_PROBE(Upscale);
    RunUpscale(cmd_list, state.ps_srvs[0]);
  } else if (draw_hash == TONEMAP_PIXEL_SHADER) {
    // Fallback when the post chain has no blit (options changed): the draw before the tonemap, with the tonemap's own
    // input as the scene (learned here, used from the next frame on).
    frames_without_anchor++;
    tonemap_scene_view = state.ps_srvs[0];
    if (last_draw_ps_hash != 0) {
      fallback_hash = last_draw_ps_hash;
      fallback_ordinal = last_draw_ordinal;
    }
  } else if (frames_without_anchor > 2 && fallback_hash != 0 && draw_hash == fallback_hash
             && occurrence == fallback_ordinal) {
    upscaled_this_frame = true;
    RunUpscale(cmd_list, tonemap_scene_view);
  }
}

// Viewport: render scale and sub-pixel jitter, only for passes that cover a whole screen-sized (or half / quarter ...)
// render target. Viewport units are render pixels, which is also what DLSS expects the jitter in.
inline bool ApplyViewport(reshade::api::command_list* cmd_list, CommandListState& state, bool scaled_domain) {
  bool wants_viewport = false;
  bool scaled_viewport = false;  // the viewport was shrunk to the render rectangle
  auto wanted_viewport = state.game_viewport;
  const bool viewport_matches_target =
      (state.has_viewport && state.render_target_count > 0 && state.render_target_width != 0
       && std::fabs(state.game_viewport.width - static_cast<float>(state.render_target_width)) <= 1.f
       && std::fabs(state.game_viewport.height - static_cast<float>(state.render_target_height)) <= 1.f);
  if (viewport_matches_target) {
    bool screen_fraction = false;
    for (int divisor = 1; divisor <= 16 && !screen_fraction; divisor *= 2) {
      const int delta_x = std::abs(static_cast<int>(state.render_target_width) * divisor - static_cast<int>(back_buffer_width));
      const int delta_y = std::abs(static_cast<int>(state.render_target_height) * divisor - static_cast<int>(back_buffer_height));
      screen_fraction = (delta_x <= divisor && delta_y <= divisor);
    }
    if (screen_fraction && scaled_domain) {
      wanted_viewport.width = static_cast<float>(state.render_target_width) * scale_x;
      wanted_viewport.height = static_cast<float>(state.render_target_height) * scale_y;
      wants_viewport = true;
      scaled_viewport = true;
    }
    const bool full_size_target = (state.render_target_width == back_buffer_width
                                   && state.render_target_height == back_buffer_height);
    bool jitter_format = false;
    switch (state.render_target_format) {
      case reshade::api::format::r10g10b10a2_unorm:
      case reshade::api::format::r16g16b16a16_float:
      case reshade::api::format::r8g8b8a8_unorm:
        jitter_format = true;
        break;
      default:
        break;
    }
    // Only the scene passes before the upscaler are jittered; everything after it (HUD, menus) and frames without a 3D scene stay still.
    if (settings.enabled && scene_frame_hint && !upscaled_this_frame && !frame_holding
        && state.has_depth_stencil && full_size_target && jitter_format) {
      wanted_viewport.x += jitter_x;
      wanted_viewport.y += jitter_y;
      wants_viewport = true;
    }
  }
  if (wants_viewport) {
    // Rebinding every time is cheap and keeps the viewport right when the game re-binds between draws.
    cmd_list->bind_viewports(0, 1, &wanted_viewport);
    state.viewport_overridden = true;
  } else if (state.viewport_overridden) {
    cmd_list->bind_viewports(0, 1, &state.game_viewport);
    state.viewport_overridden = false;
  }
  return scaled_viewport;
}

// Vertex shader replacement: full-screen quads read UVs from their vertex data, which must follow the shrunk viewport.
inline void ApplyVertexShaderOverride(reshade::api::command_list* cmd_list, CommandListState& state, bool indexed,
                                      uint32_t draw_hash, bool scaled_domain) {
  uint64_t wanted_vs = 0;
  float offset_x = 0.f;
  float offset_y = 0.f;
  if (!indexed && state.vs_hash != 0) {
    const bool quad = (state.vs_hash == QUAD_VERTEX_SHADER || state.vs_hash == SHADOW_QUAD_VERTEX_SHADER);
    // The CoC pass runs after the upscaler (full viewport) but reads the shrunk depth chain.
    const bool reads_low_resolution_depth =
        (frame_scaled && upscaled_this_frame && draw_hash == COC_PIXEL_SHADER && state.vs_hash == QUAD_VERTEX_SHADER);
    const bool ssao_history_copy = (scaled_domain && state.vs_hash == SSAO_HISTORY_COPY_VERTEX_SHADER
                                    && draw_hash == SSAO_HISTORY_COPY_PIXEL_SHADER);
    if ((quad || ssao_history_copy) && scaled_domain && state.render_target_width != 0 && state.render_target_height != 0) {
      // The viewport shrinks by the scale but the quad's half-texel position offset does not: a pass that reads the
      // same texel grid lands 0.5 * (1 - scale) texels off compared to the unscaled game; correct it in UV space.
      offset_x = 0.5f * (1.f - scale_x) / static_cast<float>(state.render_target_width);
      offset_y = 0.5f * (1.f - scale_y) / static_cast<float>(state.render_target_height);
    }
    if (quad && (scaled_domain || reads_low_resolution_depth) && EnsureOverridePipelines(cmd_list->get_device())) {
      wanted_vs = (state.vs_hash == SHADOW_QUAD_VERTEX_SHADER ? shadow_vertex_override : quad_vertex_override).handle;
    } else if (ssao_history_copy && EnsureOverridePipelines(cmd_list->get_device())) {
      wanted_vs = copy_vertex_override.handle;
    }
  }
  if (wanted_vs != 0) {
    if (!state.vs_overridden || state.vs_override_pipeline != wanted_vs) {
      cmd_list->bind_pipeline(reshade::api::pipeline_stage::vertex_shader, {wanted_vs});
      state.vs_overridden = true;
      state.vs_override_pipeline = wanted_vs;
    }
    uv_scale_values[4] = offset_x;
    uv_scale_values[5] = offset_y;
    cmd_list->push_constants(reshade::api::shader_stage::vertex, override_layout, 0, 0, UV_SCALE_CONSTANT_COUNT,
                             uv_scale_values.data());
  } else if (state.vs_overridden) {
    cmd_list->bind_pipeline(reshade::api::pipeline_stage::vertex_shader, {state.game_vs_pipeline});
    state.vs_overridden = false;
    state.vs_override_pipeline = 0;
  }
}

// Pixel shader replacements: the game's motion blur becomes a passthrough; the water, SSAO and light shaft mask are
// patched copies that address their render-scaled inputs correctly; the depth of field blur / combine use the full
// resolution depth.
inline void ApplyPixelShaderOverride(reshade::api::command_list* cmd_list, CommandListState& state, uint32_t draw_hash,
                                     bool scaled_domain) {
  uint64_t wanted_ps = 0;
  bool needs_constants = false;
  bool wants_main_depth_srv = false;
  auto* device = cmd_list->get_device();
  const bool relevant = draw_hash == MOTION_BLUR_PIXEL_SHADER || draw_hash == MASKED_MOTION_BLUR_PIXEL_SHADER
                        || draw_hash == WATER_PIXEL_SHADER || draw_hash == SSAO_PIXEL_SHADER
                        || draw_hash == LIGHT_SHAFT_PIXEL_SHADER || draw_hash == DOF_BLUR_PIXEL_SHADER
                        || draw_hash == DOF_COMBINE_PIXEL_SHADER;
  if (draw_hash == MOTION_BLUR_PIXEL_SHADER && state.ps_srvs[4] != 0 && live_views.contains(state.ps_srvs[4])) {
    DumpCaptureVelocity(device, cmd_list, {state.ps_srvs[4]});  // (only when a frame dump is pending)
    // The velocity map is written before the upscale trigger of every frame and read here: remember where it lives.
    const auto velocity = device->get_resource_from_view({state.ps_srvs[4]});
    if (velocity.handle != 0 && live_resources.contains(velocity.handle)) {
      const auto velocity_desc = device->get_resource_desc(velocity);
      if (velocity_desc.type == reshade::api::resource_type::texture_2d && velocity_desc.texture.samples == 1
          && velocity_desc.texture.depth_or_layers == 1
          && (velocity_desc.texture.format == reshade::api::format::r8g8b8a8_unorm
              || velocity_desc.texture.format == reshade::api::format::r8g8b8a8_typeless)) {
        engine_velocity_resource = velocity;
        engine_velocity_width = velocity_desc.texture.width;
        engine_velocity_height = velocity_desc.texture.height;
      }
    }
  }
  if (relevant && (draw_hash == MOTION_BLUR_PIXEL_SHADER || draw_hash == MASKED_MOTION_BLUR_PIXEL_SHADER)) {
    if (settings.motion_blur_off) {
      if (EnsureOverridePipelines(device)) wanted_ps = motion_blur_off_override.handle;
    } else if (frame_scaled && upscaled_this_frame) {
      // The game's own blur runs, but its velocity (and mask) maps only hold data in the render rectangle: hand it
      // full-range copies.
      for (uint32_t slot = 0; slot < state.ps_srvs.size(); slot++) {
        const uint64_t view = state.ps_srvs[slot];
        if (view == 0 || !live_views.contains(view)) continue;
        const uint64_t replacement = ResampleScaledTexture(cmd_list, view);
        if (replacement == 0) continue;
        const reshade::api::resource_view replacement_view = {replacement};
        cmd_list->push_descriptors(reshade::api::shader_stage::pixel, {0u}, 0,
                                   {.binding = slot,
                                    .count = 1,
                                    .type = reshade::api::descriptor_type::shader_resource_view,
                                    .descriptors = &replacement_view});
      }
    }
  } else if (relevant && draw_hash == WATER_PIXEL_SHADER) {
    if (scaled_domain && EnsureOverridePipelines(device)) {
      wanted_ps = water_override.handle;
      needs_constants = true;
    }
  } else if (relevant && draw_hash == SSAO_PIXEL_SHADER) {
    if (scaled_domain && EnsureOverridePipelines(device)) {
      wanted_ps = ssao_override.handle;
      needs_constants = true;
    }
  } else if (relevant && draw_hash == LIGHT_SHAFT_PIXEL_SHADER) {
    if (frame_scaled && upscaled_this_frame && EnsureOverridePipelines(device)) {
      wanted_ps = light_shaft_override.handle;
      needs_constants = true;
    }
  } else if (relevant && settings.enabled && upscaled_this_frame) {  // depth of field blur / combine
    if (EnsureOverridePipelines(device)) {
      wanted_ps = (draw_hash == DOF_BLUR_PIXEL_SHADER ? dof_blur_override.handle : dof_combine_override.handle);
      const bool constants_ready = (dof_constants_frame == frame_number && EnsureMainDepthView(device));
      uv_scale_values[18] = constants_ready ? 1.f : 0.f;
      wants_main_depth_srv = constants_ready;
      needs_constants = true;
    }
  }
  if (wanted_ps != 0) {
    if (!state.ps_overridden || state.ps_override_pipeline != wanted_ps) {
      cmd_list->bind_pipeline(reshade::api::pipeline_stage::pixel_shader, {wanted_ps});
      state.ps_overridden = true;
      state.ps_override_pipeline = wanted_ps;
    }
    if (needs_constants) {
      cmd_list->push_constants(reshade::api::shader_stage::pixel, override_layout, 0, 0, UV_SCALE_CONSTANT_COUNT,
                               uv_scale_values.data());
    }
    if (wants_main_depth_srv && main_depth_srv.handle != 0) {
      cmd_list->push_descriptors(reshade::api::shader_stage::pixel, {0u}, 0,
                                 {.binding = 10,
                                  .count = 1,
                                  .type = reshade::api::descriptor_type::shader_resource_view,
                                  .descriptors = &main_depth_srv});
    }
  } else if (state.ps_overridden) {
    cmd_list->bind_pipeline(reshade::api::pipeline_stage::pixel_shader, {state.game_ps_pipeline});
    state.ps_overridden = false;
    state.ps_override_pipeline = 0;
  }
}

// Texture sharpening: bind the biased twin of every sampler the game set while the scene renders below output size, and
// put the originals back afterwards. Only slots whose bound sampler differs get re-pushed, and only when the game
// changed its samplers or the twin mode flipped.
inline void ApplySamplerTwins(reshade::api::command_list* cmd_list, CommandListState& state, bool scaled_domain) {
  if (sampler_layout.handle == 0u || (mip_bias == 0.f && twin_bias == 0.f)) return;
  const bool skinned_draw = (state.vs_hash == SKINNED_VERTEX_SHADER_A || state.vs_hash == SKINNED_VERTEX_SHADER_B);
  const bool want_twins = (scaled_domain && mip_bias != 0.f && !(skinned_draw && !settings.character_sharpening));
  auto* device = cmd_list->get_device();
  if (twin_bias != mip_bias) {
    // The bias changed (mode switch): old twins are stale.
    for (const auto& [original, twin] : sampler_twins) {
      own_samplers.erase(twin);
      device->destroy_sampler({twin});
    }
    sampler_twins.clear();
    samplers_without_twin.clear();
    twin_bias = mip_bias;
    for (auto& [list, list_state] : command_list_states) list_state.sampler_dirty = true;
  }
  if (!state.sampler_dirty && state.twins_applied == want_twins) return;

  std::array<uint64_t, 16> desired = state.game_samplers;
  if (want_twins) {
    for (size_t slot = 0; slot < desired.size(); slot++) {
      const uint64_t original = state.game_samplers[slot];
      if (original == 0) continue;
      auto twin = sampler_twins.find(original);
      if (twin == sampler_twins.end() && !samplers_without_twin.contains(original)) {
        // Comparison samplers (shadow maps) and samplers we know nothing about keep the game's own object.
        const auto description = sampler_descs.find(original);
        bool created_twin = false;
        if (description != sampler_descs.end()
            && static_cast<uint32_t>(description->second.filter)
                   < static_cast<uint32_t>(reshade::api::filter_mode::compare_min_mag_mip_point)) {
          auto biased = description->second;
          biased.mip_lod_bias += mip_bias;
          reshade::api::sampler created = {0u};
          if (device->create_sampler(biased, &created) && created.handle != 0) {
            own_samplers.insert(created.handle);
            twin = sampler_twins.emplace(original, created.handle).first;
            created_twin = true;
          }
        }
        if (!created_twin) samplers_without_twin.insert(original);
      }
      if (twin != sampler_twins.end()) desired[slot] = twin->second;
    }
  }
  // One push per run of consecutive slots that differ from what is bound.
  for (uint32_t slot = 0; slot < desired.size();) {
    if (desired[slot] == 0 || desired[slot] == state.bound_samplers[slot]) {
      slot++;
      continue;
    }
    std::array<reshade::api::sampler, 16> run = {};
    uint32_t count = 0;
    const uint32_t first = slot;
    while (slot < desired.size() && desired[slot] != 0 && desired[slot] != state.bound_samplers[slot]) {
      run[count++] = {desired[slot]};
      state.bound_samplers[slot] = desired[slot];
      slot++;
    }
    cmd_list->push_descriptors(reshade::api::shader_stage::pixel, sampler_layout, sampler_layout_param,
                               {.binding = first,
                                .count = count,
                                .type = reshade::api::descriptor_type::sampler,
                                .descriptors = run.data()});
  }
  state.sampler_dirty = false;
  state.twins_applied = want_twins;
}

// Wind-animated foliage (VS 0x4A2EC780) is drawn with an extra stencil bit, so the pass that builds the upscaler masks can find it: the game's
// depth-stencil state is kept and only its reference value changes, for that draw only (restored before the next draw).
inline void ApplyFoliageStencil(reshade::api::command_list* cmd_list, const CommandListState& state) {
  if (foliage_saved_state != nullptr) {
    ID3D11DepthStencilState* current = nullptr;
    UINT current_ref = 0;
    foliage_context->OMGetDepthStencilState(&current, &current_ref);
    if (current == foliage_saved_state && current_ref == foliage_written_ref) {
      foliage_context->OMSetDepthStencilState(foliage_saved_state, foliage_saved_ref);
    }
    if (current != nullptr) current->Release();
    foliage_saved_state->Release();
    foliage_saved_state = nullptr;
  }
  if (settings.upscaler != 1 || state.vs_hash != FOLIAGE_VERTEX_SHADER) return;  // (only FSR uses the foliage mask)
  auto* context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  if (context == nullptr) return;
  ID3D11DepthStencilState* depth_stencil = nullptr;
  UINT reference = 0;
  context->OMGetDepthStencilState(&depth_stencil, &reference);
  if (depth_stencil == nullptr) return;
  D3D11_DEPTH_STENCIL_DESC desc = {};
  depth_stencil->GetDesc(&desc);
  // Only a state that writes stencil unconditionally (the G-buffer pass), never one that tests it.
  if (desc.StencilEnable && desc.FrontFace.StencilFunc == D3D11_COMPARISON_ALWAYS && desc.FrontFace.StencilPassOp == D3D11_STENCIL_OP_REPLACE
      && (desc.StencilWriteMask & STENCIL_FOLIAGE_BIT) != 0) {
    foliage_saved_state = depth_stencil;  // keeps the reference taken by OMGet
    foliage_saved_ref = reference;
    foliage_written_ref = reference | STENCIL_FOLIAGE_BIT;
    foliage_context = context;
    context->OMSetDepthStencilState(depth_stencil, foliage_written_ref);
  } else {
    depth_stencil->Release();
  }
}

inline bool OnDraw(reshade::api::command_list* cmd_list, const DrawArgs& args) {
  DS2_PROBE(Draw);
  if (in_own_pass) return false;
  DS2_LOCK_OR_RETURN(false);
  auto& state = StateFor(cmd_list);
  draws_this_frame++;
  // The game's FXAA reads a copy of the picture and writes the picture the HUD is drawn on; without it that picture stays as the tonemap
  // pass left it (the upscaled image).
  if (state.ps_hash == FXAA_PIXEL_SHADER && settings.disable_fxaa && settings.enabled) return true;
  ApplyFoliageStencil(cmd_list, state);
  const bool indexed = args.indexed;
  RestoreHudConstants(cmd_list, state);
  if (pending_stabilize >= 0) {
    const int index = pending_stabilize;
    pending_stabilize = -1;
    RunStabilizer(cmd_list, index);
  }
  CaptureCameraIfSceneDraw(state);

  const uint32_t draw_hash = state.ps_hash;
  if (draw_hash == COC_PIXEL_SHADER) CaptureDofConstants(state);
  uint32_t occurrence = 0;
  if (draw_hash != 0) occurrence = hash_occurrences.Increment(draw_hash);
  TriggerUpscale(cmd_list, state, draw_hash, occurrence);
  last_draw_ps_hash = draw_hash;
  last_draw_ordinal = occurrence;

  // Scene passes before the upscaler runs are drawn into the top-left sub-rectangle (render scale < 1). The scale applies
  // from the first draw of the frame: the reflection map and other half-resolution inputs are drawn before the camera
  // is seen. A frame is scaled when the previous one was a scene frame (see OnPresent).
  const bool scaled_domain = (frame_scaled && !upscaled_this_frame);
  const bool scaled_viewport = ApplyViewport(cmd_list, state, scaled_domain);
  if (frame_scaled && state.render_target_resource != 0) {
    // Which textures hold their picture only in the top-left render rectangle (valid until they are drawn again at full size).
    if (scaled_viewport) {
      scaled_written_resources.insert(state.render_target_resource);
    } else if (!scaled_written_resources.empty()) {
      scaled_written_resources.erase(state.render_target_resource);
    }
  }
  ApplyVertexShaderOverride(cmd_list, state, indexed, draw_hash, scaled_domain);
  ApplyPixelShaderOverride(cmd_list, state, draw_hash, scaled_domain);

  if (frame_scaled && upscaled_this_frame && state.render_target_resource != 0
      && (draw_hash == COC_PIXEL_SHADER || draw_hash == LIGHT_SHAFT_PIXEL_SHADER)) {
    const int index = (draw_hash == COC_PIXEL_SHADER ? 0 : 1);
    stabilized_effects[index].target = {state.render_target_resource};
    pending_stabilize = index;
  }
  ApplySamplerTwins(cmd_list, state, scaled_domain);
  return ApplyHudScale(cmd_list, state, args);
}

inline bool OnDrawVertices(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count,
                           uint32_t first_vertex, uint32_t first_instance) {
  return OnDraw(cmd_list, {.indexed = false, .count = vertex_count, .first = first_vertex});
}

inline bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
                          uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  return OnDraw(cmd_list, {.indexed = true, .count = index_count, .first = first_index});
}

// ---- Samplers ----
inline void OnInitSampler(reshade::api::device* device, const reshade::api::sampler_desc& desc,
                          reshade::api::sampler sampler) {
  DS2_PROBE(ResourceEvents);
  DS2_LOCK_OR_RETURN();
  if (own_samplers.contains(sampler.handle)) return;
  sampler_descs[sampler.handle] = desc;
}

inline void OnDestroySampler(reshade::api::device* device, reshade::api::sampler sampler) {
  DS2_PROBE(ResourceEvents);
  DS2_LOCK_OR_RETURN();
  if (own_samplers.contains(sampler.handle)) return;  // one of our twins going away: nothing else refers to it
  sampler_descs.erase(sampler.handle);
  samplers_without_twin.erase(sampler.handle);
  const auto twin = sampler_twins.find(sampler.handle);
  if (twin != sampler_twins.end()) {
    own_samplers.erase(twin->second);
    device->destroy_sampler({twin->second});
    sampler_twins.erase(twin);
  }
  for (auto& [list, list_state] : command_list_states) {
    for (size_t slot = 0; slot < list_state.game_samplers.size(); slot++) {
      if (list_state.game_samplers[slot] == sampler.handle) list_state.game_samplers[slot] = 0;
      if (list_state.bound_samplers[slot] == sampler.handle) list_state.bound_samplers[slot] = 0;
    }
  }
}

// ---- Resource lifetime tracking: the game recreates its textures when settings or resolution change ----
inline void OnInitResource(reshade::api::device* device, const reshade::api::resource_desc& desc,
                           const reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state,
                           reshade::api::resource resource) {
  DS2_PROBE(ResourceEvents);
  DS2_LOCK_OR_RETURN();
  live_resources.insert(resource.handle);
  if (desc.type == reshade::api::resource_type::buffer && desc.buffer.size == SCENE_CAMERA_BUFFER_BYTES
      && HasFlag(desc.usage, reshade::api::resource_usage::constant_buffer)) {
    camera_buffers.insert(resource.handle);
  }
  // Small static vertex buffers (HUD quads) are written once at creation: keep that data for HUD scaling.
  if (desc.type == reshade::api::resource_type::buffer && desc.buffer.size <= (16u << 10) && initial_data != nullptr
      && initial_data->data != nullptr && HasFlag(desc.usage, reshade::api::resource_usage::vertex_buffer)) {
    auto& bytes = vertex_buffer_contents[resource.handle];
    bytes.assign(static_cast<const uint8_t*>(initial_data->data),
                 static_cast<const uint8_t*>(initial_data->data) + static_cast<size_t>(desc.buffer.size));
  }
}

inline void OnDestroyResource(reshade::api::device* device, reshade::api::resource resource) {
  DS2_PROBE(ResourceEvents);
  DS2_LOCK_OR_RETURN();
  live_resources.erase(resource.handle);
  camera_buffers.erase(resource.handle);
  if (main_depth_resource.handle == resource.handle) main_depth_resource = {0u};
  if (engine_velocity_resource.handle == resource.handle) engine_velocity_resource = {0u};
  FsrForgetResource(resource.handle);
  vertex_buffer_contents.erase(resource.handle);
  hud_vertex_buffers.erase(resource.handle);
  hud_vertex_windows.erase(resource.handle);
  camera_buffer_contents.erase(resource.handle);
  filter_buffer_contents.erase(resource.handle);
  tracked_filter_buffers.erase(resource.handle);
  scaled_written_resources.erase(resource.handle);
  if (const auto found = resample_slots.find(resource.handle); found != resample_slots.end()) {
    found->second.pass.pass.DestroyAll(device);
    if (found->second.scratch_view.handle != 0) device->destroy_resource_view(found->second.scratch_view);
    if (found->second.scratch.handle != 0) device->destroy_resource(found->second.scratch);
    resample_slots.erase(found);
  }
  for (auto& effect : stabilized_effects) {
    if (effect.target.handle == resource.handle) {
      effect.target = {0u};
      effect.pass_target = {};
    }
  }
  for (PassSlot* slot : {&motion_vector_slot, &bias_mask_slot, &sharpen_slot}) {
    if (slot->first == resource || slot->second == resource || slot->third == resource || slot->target == resource) ResetPassSlot(device, slot);
  }
}

inline void OnInitResourceView(reshade::api::device* device, reshade::api::resource resource,
                               reshade::api::resource_usage usage_type, const reshade::api::resource_view_desc& desc,
                               reshade::api::resource_view view) {
  DS2_PROBE(ResourceEvents);
  DS2_LOCK_OR_RETURN();
  live_views.insert(view.handle);
}

inline void OnDestroyResourceView(reshade::api::device* device, reshade::api::resource_view view) {
  DS2_PROBE(ResourceEvents);
  DS2_LOCK_OR_RETURN();
  live_views.erase(view.handle);
}

inline void ResetTemporalState() {
  main_depth_resource = {0u};
  camera_current = {};
  camera_previous = {};
  last_upscaled_frame = 0;
  fallback_hash = 0;
  fallback_ordinal = 0;
}

inline void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  DS2_LOCK_OR_RETURN();
  ResetTemporalState();
}

inline void OnDestroySwapchain(reshade::api::swapchain* swapchain, bool resize) {
  DS2_LOCK_OR_RETURN();
  ResetTemporalState();
}

inline void OnDestroyDevice(reshade::api::device* device) {
  DS2_LOCK_OR_RETURN();
  ShutdownDlss();
  ResetPassSlot(device, &motion_vector_slot);
  ResetPassSlot(device, &bias_mask_slot);
  ResetPassSlot(device, &sharpen_slot);
  for (auto& [source, slot] : resample_slots) slot.pass.pass.DestroyAll(device);
  resample_slots.clear();
  scaled_written_resources.clear();
  bias_mask_texture = {0u};
  bias_mask_width = 0;
  bias_mask_height = 0;
  main_depth_srv = {0u};
  main_depth_srv_resource = {0u};
  ResetDepthRebuild(nullptr, false);
  depth_rebuild_layout = {0u};
  depth_rebuild_pipeline = {0u};
  constants_texture = {0u};
  engine_velocity_dummy = {0u};
  engine_velocity_resource = {0u};
  FsrReleaseResources();
  motion_vector_texture = {0u};
  exposure_texture = {0u};
  exposure_applied = -1.f;
  upscaled_texture = {0u};
  texture_width = 0;
  texture_height = 0;
  for (auto& effect : stabilized_effects) {
    for (int i = 0; i < 2; i++) {
      effect.passes[i].DestroyAll(device);
      effect.passes[i] = {};
      effect.pass_target[i] = {0u};
      effect.textures[i] = {0u};
    }
    effect.target = {0u};
    effect.width = 0;
  }
  stabilize_constants = {0u};
  pending_stabilize = -1;
  override_layout = {0u};
  quad_vertex_override = {0u};
  shadow_vertex_override = {0u};
  copy_vertex_override = {0u};
  motion_blur_off_override = {0u};
  water_override = {0u};
  light_shaft_override = {0u};
  ssao_override = {0u};
  dof_blur_override = {0u};
  dof_combine_override = {0u};
  override_pipelines_ready = false;
}

}  // namespace ds2

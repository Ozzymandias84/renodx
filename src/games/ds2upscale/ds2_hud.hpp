// HUD scaling. The game's front end draws every HUD element as a textured quad after the tonemap, with a transform matrix in a
// vertex constant buffer (cbFeShader at b8 for the sprites, cbParam at b0 for the text / icon shader). For a scaled HUD the
// matrix is replaced, for that draw only, by one that scales the element in clip space around an anchor: the screen edge
// the element sits at (so a bar in the top-left corner stays in the corner), or the screen centre for elements in the
// middle. The element's box comes from its matrix applied to the mirrored vertices; quads whose boxes touch form a widget
// (cluster) that shares one anchor.
#pragma once

#include "ds2_resources.hpp"

namespace ds2 {

constexpr uint32_t HUD_SPRITE_VERTEX_SHADER = 0x343F370Du;       // cbFeShader (b8), float4 position
constexpr uint32_t HUD_MASKED_VERTEX_SHADER = 0x3A5EFA00u;       // cbFeShader (b8), float4 position + mask UVs
constexpr uint32_t HUD_MASKED2_VERTEX_SHADER = 0x592CE497u;      // cbFeShader (b8), position + two masks (boss bar, prompts, dialogue)
constexpr uint32_t HUD_MASKED2_OTHER_HASH = 0x4E0DEEB8u;         // its pixel shader (the capture tool labels the pair either way round)
constexpr uint32_t HUD_TEXT_VERTEX_SHADER = 0xC8DE0A35u;         // cbParam (b0), float2 position
constexpr uint32_t HUD_SPRITE_SLOT = 8;
constexpr uint32_t HUD_SPRITE_FLOATS = 40;  // g_TransformMatrixT + both mask matrices (10 float4)
constexpr uint32_t HUD_TEXT_SLOT = 0;
constexpr uint32_t HUD_TEXT_FLOATS = 16;    // matWVP

struct DrawArgs {
  bool indexed = false;
  uint32_t count = 0;  // vertices (or indices)
  uint32_t first = 0;  // first vertex (or index)
};

inline reshade::api::pipeline_layout hud_sprite_layout = {0u};
inline reshade::api::pipeline_layout hud_text_layout = {0u};

// The HUD vertices are only mirrored while the HUD is scaled.
inline bool WantsVertexMirror() { return settings.hud_scale != 1.f; }

// ---- Vertex mirror windows ----
// The game appends the quads of every element to one big dynamic vertex buffer (Map, write, Unmap, draw). Copying the whole
// 2 MB buffer at each Unmap was ~300 MB per frame; the new vertices start where the previous draw ended, so only a window
// as big as the recent draws needed is copied. The range that holds data written since the buffer was last discarded is
// remembered: a draw that reads outside of it is left unscaled instead of being scaled from stale data.
struct HudVertexWindow {
  uint64_t valid_lo = 0;      // mirror range written since the buffer was last discarded
  uint64_t valid_hi = 0;
  uint64_t cursor = 0;        // end of the last draw that read from the buffer: the next vertices are written from here
  uint64_t window_start = 0;  // where the last copy began
  uint64_t needed = 0;        // largest read (counted from window_start) of a recent draw
};
inline std::unordered_map<uint64_t, HudVertexWindow> hud_vertex_windows;
constexpr uint64_t HUD_WINDOW_MIN = 8u << 10;
constexpr uint64_t HUD_WINDOW_MAX = 256u << 10;

// A small static vertex buffer is complete from its initial data; a big dynamic one starts empty.
inline HudVertexWindow& HudWindowFor(uint64_t buffer, uint64_t mirror_size) {
  const auto [found, created] = hud_vertex_windows.try_emplace(buffer);
  if (created && mirror_size != 0 && mirror_size <= (16u << 10)) found->second.valid_hi = mirror_size;
  return found->second;
}

// The part of a write into [offset, offset + size) of a HUD vertex buffer that the mirror copies.
inline void TakeHudWindow(HudVertexWindow* window, uint64_t offset, uint64_t size, uint64_t* begin, uint64_t* end) {
  const uint64_t write_end = offset + size;
  *begin = offset;
  *end = write_end;
  if (size > HUD_WINDOW_MIN) {
    if (window->cursor >= offset && window->cursor < write_end) *begin = window->cursor;
    *end = std::min(write_end, *begin + std::clamp(2 * window->needed + 2048, HUD_WINDOW_MIN, HUD_WINDOW_MAX));
  }
  window->window_start = *begin;
  if (*begin <= window->valid_hi && *end >= window->valid_lo) {
    window->valid_lo = std::min(window->valid_lo, *begin);
    window->valid_hi = std::max(window->valid_hi, *end);
  } else {
    window->valid_lo = *begin;
    window->valid_hi = *end;
  }
}

// The buffer was discarded: nothing in the mirror is valid any more and the game starts appending from the start.
inline void ResetHudWindow(uint64_t buffer) {
  if (const auto found = hud_vertex_windows.find(buffer); found != hud_vertex_windows.end()) {
    found->second.valid_lo = 0;
    found->second.valid_hi = 0;
    found->second.cursor = 0;
    found->second.window_start = 0;
  }
}

// ---- Widget clusters ----
// Quads whose boxes touch belong together and must scale around the same anchor. Built at present from the boxes of the
// frame, used by the next frame (the HUD is static from one frame to the next).
//
// Menus and corner widgets overlap on screen (a dialog over the souls counter, a message panel over the equipment slots) and
// the game draws them in no reliable order, so draw distance cannot tell them apart. What does: how long a quad has been
// there. A quad is identified by its shader pair and rounded box; one that was in the previous frame keeps the cluster it
// had, and an old one never merges with anything new (it only takes in new quads that lie inside it, like the damage
// preview inside a bar). New quads merge among themselves, which is how a menu that opens becomes one cluster.
struct HudBox {
  float lo[2];
  float hi[2];
  bool wide;      // stretched / clipped quad (bar fill, backdrop): joins a widget but does not decide its anchor
  bool text;      // a glyph: belongs to the box it was drawn on, not to whatever it happens to overlap
  uint64_t key;   // shader pair + rounded box: the same quad in the next frame has the same key
  float anchor[2];  // the anchor the quad was drawn with this frame
  bool online;      // that anchor came from a group made at draw time (nothing known about the quad yet)
  bool settled;     // the same quad has been there for a while
  bool provisional; // anchor guessed from incomplete information (first frame of a popup): drawn only once it is settled
  bool menu;        // part of a menu page / panel: left exactly as the game draws it (not scaled)
  bool hidden;      // held back this frame (not drawn)
};
struct HudCluster {
  float lo[2];  // union of all member boxes
  float hi[2];
  float anchor[2];
  bool in_place;  // anchor is the widget's own centre (it sits on a backdrop that is not scaled), not a screen edge
  bool settled;   // has members that have been there for a while (a new popup cluster has none)
};
constexpr uint32_t HUD_MAX_BOXES = 512;
inline std::array<HudBox, HUD_MAX_BOXES> hud_boxes;
inline uint32_t hud_box_count = 0;
// Backdrops that are not scaled (fades, banners' gradients): elements on top of them scale in place instead of toward an edge.
inline std::array<HudBox, 16> hud_backdrops;
inline uint32_t hud_backdrop_count = 0;
inline std::array<HudCluster, 96> hud_clusters;
inline uint32_t hud_cluster_count = 0;
// Quads of the previous frame: cluster they ended up in and for how many frames they have been there.
struct HudPrevBox {
  uint32_t group;
  uint32_t age;
  float anchor[2];  // the anchor this quad keeps for as long as it is drawn (no flicker when clusters are rebuilt)
};
inline std::unordered_map<uint64_t, HudPrevBox> hud_prev_boxes;
constexpr uint32_t HUD_OLD_AGE = 12;  // frames after which a quad is a settled part of its widget
// The settled quads of the previous frame with the anchor of their cluster: a changed quad that lies inside one of them
// (a bar's fill, a damage preview) is drawn with the same anchor.
struct HudPrevSettled {
  float lo[2];
  float hi[2];
  float anchor[2];
};
inline std::array<HudPrevSettled, 256> hud_prev_settled;
inline uint32_t hud_prev_settled_count = 0;
// The non-glyph quads of the previous frame with the anchor they ended up with. A quad whose box changes a little every frame (a
// banner's strip that grows, a sprite that slides) has a new key each time; it is the same quad when the previous frame had one
// with the same shaders and almost the same box, and it keeps that quad's anchor instead of being treated as new (and held back).
struct HudPrevGeometry {
  float lo[2];
  float hi[2];
  uint32_t shaders;
  float anchor[2];
};
inline std::array<HudPrevGeometry, HUD_MAX_BOXES> hud_prev_geometry;
inline uint32_t hud_prev_geometry_count = 0;
inline std::array<uint32_t, HUD_MAX_BOXES> hud_box_shaders = {};  // shader pair of each box of hud_boxes
inline uint32_t hud_hide_streak = 0;     // consecutive frames in which provisional quads were held back
inline bool hud_hidden_this_frame = false;
inline uint32_t hud_hide_cooldown = 0;   // frames during which nothing is held back (after two held-back frames in a row)
// Menu pages and panels are not scaled. A frame is a "menu frame" from the first quad that is a page / panel (big and tall:
// area >= 0.4 and height >= 0.45; fades and horizontal gradient bands do not count); every quad that is new in such a frame is a
// menu quad (its key is remembered for the next frame). Quads that were part of the gameplay HUD before the menu opened, and
// glyphs / sprites that belong to them, keep being scaled.
inline std::unordered_set<uint64_t> hud_menu_keys;       // menu quads of this frame
inline std::unordered_set<uint64_t> hud_menu_keys_prev;  // menu quads of the previous frame
inline bool hud_menu_frame = false;
inline bool hud_menu_frame_prev = false;
// The lock-on elements: the crosshair (3 near-identical sprites, ~0.08 x 0.14) and the enemy's little health bar above it (3 near-
// identical quads, ~0.15 x 0.02), drawn at the enemy's screen position. They have to stay on the enemy, so they are scaled around
// their own centre. Recognised as a third near-identical small quad in a row, or as a small quad that lies where such a group was
// in the previous frame (up to 8 groups). Never in the bottom-left / bottom-right corner widgets, which have such duplicates too.
inline std::array<std::array<float, 2>, 8> hud_marker_now = {};
inline std::array<std::array<float, 2>, 8> hud_marker_prev = {};
inline uint32_t hud_marker_now_count = 0;
inline uint32_t hud_marker_prev_count = 0;
// The two quads processed before the current one in this frame (flagged or not): a group is only "confirmed" by a third copy, so
// the groups of the previous frame can not keep themselves alive by flagging the quads next to them.
inline std::array<std::array<float, 4>, 2> hud_recent_quads = {};
inline uint32_t hud_recent_quad_count = 0;
// The banner of this frame (YOU DIED, RETRIEVAL: a wide quad, >= HUD_BANNER_MIN_WIDTH across, drawn inside a gradient band): the other quads that
// lie within its height (the sprite with the word itself) belong to it and are scaled around the same centre from their first
// frame on.
inline bool hud_banner_this_frame = false;
inline float hud_banner_centre[2] = {};
inline float hud_banner_y_range[2] = {};
// Groups made at draw time for quads whose cluster is not known yet (first frame of a popup): they follow what was drawn
// just before so the whole popup gets one anchor from its first frame on.
struct HudOnlineGroup {
  float lo[2];
  float hi[2];
  float anchor[2];
};
inline std::array<HudOnlineGroup, 32> hud_online_groups;
inline uint32_t hud_online_count = 0;

inline uint64_t HudBoxKey(uint32_t vs, uint32_t ps, const float lo[2], const float hi[2]) {
  uint64_t hash = 1469598103934665603ull;
  const auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
  mix(vs);
  mix(ps);
  for (int axis = 0; axis < 2; axis++) {
    mix(static_cast<uint64_t>(std::lround(lo[axis] * 256.f) + 8192));
    mix(static_cast<uint64_t>(std::lround(hi[axis] * 256.f) + 8192));
  }
  return hash;
}

// Distance from the screen centre (NDC) beyond which an element keeps its edge, per axis. Vertically the limit is small: the
// floor message dialog is centred at y -0.39 and must sit on the bottom edge, not in the middle.
inline float HudZone(int axis) { return axis == 0 ? tuning.hud_zone : tuning.hud_zone_y; }

// An anchor that is a screen edge / centre (-1, 0, 1 on both axes). A banner is scaled around its own centre: its numeric anchor
// must never be handed on to other quads (the bonfire prompt that appears while the YOU DIED band fades out lies inside it).
inline bool HudEdgeAnchor(const float a[2]) {
  return (a[0] == -1.f || a[0] == 0.f || a[0] == 1.f) && (a[1] == -1.f || a[1] == 0.f || a[1] == 1.f);
}

// The top-left corner (health / stamina bars, the in-game menu tab bar and the drop-down that hangs under it) always belongs to
// the top-left corner: a drop-down that formed its own cluster used to get the centre anchor and sat far to the right.
// A wide quad that sits symmetrically about the screen centre (the strip behind a "Poisoned" / "A primal bonfire was rekindled"
// notification): it reaches left of x -0.2 like the corner widgets do, but it belongs to the centre.
inline bool HudCentred(const float lo[2], const float hi[2]) {
  return hi[0] - lo[0] >= 0.4f && std::fabs(lo[0] + hi[0]) <= 0.1f * (hi[0] - lo[0]);
}
inline bool HudInTopLeft(const float lo[2], const float hi[2]) {
  return lo[0] <= -0.2f && 0.5f * (lo[1] + hi[1]) >= 0.2f && !HudCentred(lo, hi);
}

inline uint32_t HudShaderPair(uint32_t vs, uint32_t ps) { return vs * 0x9E3779B1u ^ ps; }

// A glyph is drawn on a box when one of the `count` earlier boxes of the frame contains its centre; it then follows that box. A glyph
// on no box (the arrow beside the menu title at x -0.21 lies just outside the title's boxes) is an ordinary quad for the corner rule.
inline bool HudGlyphOnBox(uint32_t count, const float lo[2], const float hi[2]) {
  const float centre_x = 0.5f * (lo[0] + hi[0]);
  const float centre_y = 0.5f * (lo[1] + hi[1]);
  for (uint32_t i = 0; i < count; i++) {
    const HudBox& box = hud_boxes[i];
    if (!box.text && centre_x >= box.lo[0] && centre_x <= box.hi[0] && centre_y >= box.lo[1] && centre_y <= box.hi[1]) return true;
  }
  return false;
}

// The quad of the previous frame this one continues: same shaders, every edge within one animation step of it.
inline const HudPrevGeometry* FindHudContinuation(uint32_t shaders, const float lo[2], const float hi[2]) {
  constexpr float STEP = 0.03f;
  const HudPrevGeometry* best = nullptr;
  float best_distance = STEP;
  for (uint32_t i = 0; i < hud_prev_geometry_count; i++) {
    const HudPrevGeometry& old = hud_prev_geometry[i];
    if (old.shaders != shaders) continue;
    const float distance = std::max(std::max(std::fabs(old.lo[0] - lo[0]), std::fabs(old.lo[1] - lo[1])),
                                    std::max(std::fabs(old.hi[0] - hi[0]), std::fabs(old.hi[1] - hi[1])));
    if (distance <= best_distance) {
      best = &old;
      best_distance = distance;
    }
  }
  return best;
}

// The two bottom corner widgets (equipment slots and their captions at the bottom left, the souls counter at the bottom right)
// always belong to their corner. Their anchor used to come from the cluster they were drawn in: after a trip the quads first
// show up while the area-title strip crosses the screen, join its cluster (wider than the screen) and kept the centre anchor
// for good, so the whole widget sat far to the right of its corner.
inline bool HudCornerWidgetAnchor(const float lo[2], const float hi[2], float anchor[2]) {
  const float centre_y = 0.5f * (lo[1] + hi[1]);
  if (hi[0] <= -0.4f && lo[0] >= -1.f && centre_y >= -0.95f && centre_y <= -0.1f) {
    anchor[0] = -1.f;
    anchor[1] = -1.f;
    return true;
  }
  if (lo[0] >= 0.42f && hi[0] <= 1.f && centre_y <= -0.745f && centre_y >= -1.f) {
    anchor[0] = 1.f;
    anchor[1] = -1.f;
    return true;
  }
  return false;
}

// Screen edge (-1 / 1) or centre (0) for a box on one axis: decided by the centre of the part that is on screen.
inline float HudAnchor(float lo, float hi, int axis) {
  const float centre = 0.5f * (std::clamp(lo, -1.f, 1.f) + std::clamp(hi, -1.f, 1.f));
  return (centre <= -HudZone(axis)) ? -1.f : (centre >= HudZone(axis) ? 1.f : 0.f);
}

// The cluster of the previous frame a box belongs to: the smallest one that contains the whole box, else the smallest one
// that contains its centre.
inline const HudCluster* FindHudCluster(const float lo[2], const float hi[2]) {
  constexpr float EPS = 0.004f;
  const HudCluster* whole = nullptr;
  const HudCluster* centred = nullptr;
  float whole_area = 1e30f;
  float centred_area = 1e30f;
  const float centre_x = 0.5f * (lo[0] + hi[0]);
  const float centre_y = 0.5f * (lo[1] + hi[1]);
  for (uint32_t i = 0; i < hud_cluster_count; i++) {
    const HudCluster& c = hud_clusters[i];
    const float area = (c.hi[0] - c.lo[0]) * (c.hi[1] - c.lo[1]);
    if (lo[0] >= c.lo[0] - EPS && hi[0] <= c.hi[0] + EPS && lo[1] >= c.lo[1] - EPS && hi[1] <= c.hi[1] + EPS && area < whole_area) {
      whole = &c;
      whole_area = area;
    }
    if (centre_x >= c.lo[0] - EPS && centre_x <= c.hi[0] + EPS && centre_y >= c.lo[1] - EPS && centre_y <= c.hi[1] + EPS
        && area < centred_area) {
      centred = &c;
      centred_area = area;
    }
  }
  return whole != nullptr ? whole : centred;
}

// Two new boxes belong to one widget when they overlap substantially, or share an edge while being drawn close together (a
// bar next to its cap). A box that only grazes a neighbour's corner (a wide bar passing a corner of another widget) does
// not join it.
inline bool HudBoxesJoin(const HudBox& a, const HudBox& b, bool drawn_close) {
  constexpr float MARGIN = 0.015f;
  const float overlap_x = std::min(a.hi[0], b.hi[0]) - std::max(a.lo[0], b.lo[0]);
  const float overlap_y = std::min(a.hi[1], b.hi[1]) - std::max(a.lo[1], b.lo[1]);
  if (overlap_x < -MARGIN || overlap_y < -MARGIN) return false;
  const float width_a = a.hi[0] - a.lo[0];
  const float height_a = a.hi[1] - a.lo[1];
  const float width_b = b.hi[0] - b.lo[0];
  const float height_b = b.hi[1] - b.lo[1];
  if (overlap_x > 0.f && overlap_y > 0.f) {
    const float smaller = std::min(width_a * height_a, width_b * height_b);
    return smaller <= 1e-7f || overlap_x * overlap_y >= 0.25f * smaller;  // (a degenerate box inside another one joins)
  }
  if (!drawn_close) return false;
  if (overlap_x <= 0.f && overlap_y > 0.f) return overlap_y >= 0.5f * std::min(height_a, height_b);  // side by side
  if (overlap_y <= 0.f && overlap_x > 0.f) return overlap_x >= 0.5f * std::min(width_a, width_b);    // one above the other
  return false;
}

// A box is "settled" when it was already there (same key) for a while: it keeps its cluster and takes no new neighbours.
constexpr uint32_t HUD_NEW_NEIGHBOUR_DRAWS = 48;
// A widget reaching further than this (NDC) to both sides of the screen centre on an axis is centred on that axis.
constexpr float HUD_CENTRE_STRADDLE = 0.15f;
// A wide strip at least this across, inside a gradient band, is a banner (YOU DIED, RETRIEVAL). The strip grows while it fades in
// (1.33 -> 1.6 for YOU DIED); with the old limit of 1.4 its first 9 frames were ordinary quads.
constexpr float HUD_BANNER_MIN_WIDTH = 1.2f;
// Tall sprites (height >= 1.2 x width) up to this large (NDC, per axis) can be lock-on crosshair layers while it pulses.
constexpr float HUD_MARKER_MAX_SIZE = 0.5f;

// Groups the frame's boxes (union-find) and decides each group's anchor. The anchor comes from the area-weighted centre of
// the group's non-wide members: a boss bar's fill stretches over the whole bar but the caps at its ends say where the widget
// is, and a small sprite that shows up at the far end of a bar (the yellow damage preview) does not move it. A group that
// sits on a backdrop that is not scaled scales in place instead of toward an edge. An anchor changes only when the centre
// moved clearly past the zone limit (otherwise an animated panel flips between two anchors).
inline void BuildHudClusters() {
  // Menu quads are not part of any gameplay widget. New quads that were guessed before the frame turned out to be a menu frame
  // (they were drawn before the page itself) are menu quads too.
  {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < hud_box_count; i++) {
      const HudBox box = hud_boxes[i];
      if (box.menu || (hud_menu_frame && box.provisional && !hud_prev_boxes.contains(box.key))) {
        hud_menu_keys.insert(box.key);
        continue;
      }
      hud_box_shaders[kept] = hud_box_shaders[i];
      hud_boxes[kept++] = box;
    }
    hud_box_count = kept;
  }
  if (hud_box_count == 0) {  // nothing but menu quads: the gameplay clusters stay as they were
    hud_backdrop_count = 0;
    return;
  }
  const uint32_t count = hud_box_count;
  std::array<uint16_t, HUD_MAX_BOXES> parent = {};
  for (uint32_t i = 0; i < count; i++) parent[i] = static_cast<uint16_t>(i);
  const auto find = [&](uint32_t i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
  };
  const auto merge = [&](uint32_t a, uint32_t b) { parent[find(a)] = static_cast<uint16_t>(find(b)); };

  std::array<uint32_t, HUD_MAX_BOXES> age = {};
  std::array<bool, HUD_MAX_BOXES> known = {};
  std::array<bool, HUD_MAX_BOXES> settled = {};
  std::array<bool, HUD_MAX_BOXES> banner = {};  // in-place anchored (a banner): other quads never attach to it
  std::unordered_map<uint32_t, uint32_t> first_in_group;  // previous cluster id -> first box that carried it
  for (uint32_t i = 0; i < count; i++) {
    const auto previous = hud_prev_boxes.find(hud_boxes[i].key);
    if (previous == hud_prev_boxes.end()) continue;
    known[i] = true;
    age[i] = previous->second.age + 1;
    settled[i] = (age[i] >= HUD_OLD_AGE);
    banner[i] = !HudEdgeAnchor(previous->second.anchor);
    const auto [first, inserted] = first_in_group.try_emplace(previous->second.group, i);
    if (!inserted) merge(i, first->second);  // boxes that shared a cluster keep sharing it
  }

  // Boxes that are not settled attach to what they were drawn on (glyphs), what contains them (a sprite inside a bar), or
  // each other.
  std::array<bool, HUD_MAX_BOXES> attached = {};
  std::array<bool, HUD_MAX_BOXES> blocked = {};  // glyphs: they never link two neighbours
  for (uint32_t i = 0; i < count; i++) {
    if (settled[i]) continue;
    const HudBox& box = hud_boxes[i];
    if (box.text) {  // the latest earlier box that contains the glyph's centre
      const float centre_x = 0.5f * (box.lo[0] + box.hi[0]);
      const float centre_y = 0.5f * (box.lo[1] + box.hi[1]);
      for (uint32_t j = i; j-- > 0;) {
        const HudBox& parent_box = hud_boxes[j];
        if (parent_box.text || centre_x < parent_box.lo[0] || centre_x > parent_box.hi[0] || centre_y < parent_box.lo[1]
            || centre_y > parent_box.hi[1]) {
          continue;
        }
        merge(i, j);
        attached[i] = true;
        blocked[i] = true;
        break;
      }
      if (attached[i]) continue;
    }
    // Mostly inside a settled box that is at least as big: part of that box's widget.
    const float area = std::max(1e-6f, (box.hi[0] - box.lo[0]) * (box.hi[1] - box.lo[1]));
    int best = -1;
    float best_area = 1e30f;
    for (uint32_t j = 0; j < count; j++) {
      if (!settled[j] || banner[j]) continue;
      const HudBox& other = hud_boxes[j];
      const float overlap_x = std::min(box.hi[0], other.hi[0]) - std::max(box.lo[0], other.lo[0]);
      const float overlap_y = std::min(box.hi[1], other.hi[1]) - std::max(box.lo[1], other.lo[1]);
      const float other_area = (other.hi[0] - other.lo[0]) * (other.hi[1] - other.lo[1]);
      if (overlap_x > 0.f && overlap_y > 0.f && overlap_x * overlap_y >= 0.7f * area && other_area >= area && other_area < best_area) {
        best = static_cast<int>(j);
        best_area = other_area;
      }
    }
    if (best >= 0) {
      merge(i, static_cast<uint32_t>(best));
      attached[i] = true;
      continue;
    }
    // Drawn right after a settled quad it touches (a drop-down hanging under its tab): part of that widget.
    for (uint32_t j = i; j-- > 0 && i - j <= 3 && !attached[i];) {
      if (settled[j] && !banner[j] && HudBoxesJoin(box, hud_boxes[j], true)) {
        merge(i, j);
        attached[i] = true;
      }
    }
  }
  for (uint32_t i = 0; i < count; i++) {
    if (settled[i] || blocked[i]) continue;
    for (uint32_t j = i + 1; j < count; j++) {
      if (settled[j] || blocked[j] || find(i) == find(j)) continue;
      if (HudBoxesJoin(hud_boxes[i], hud_boxes[j], j - i <= HUD_NEW_NEIGHBOUR_DRAWS)) merge(i, j);
    }
  }

  // Leftovers (a selection highlight that stopped moving, a lone sprite) lying inside a bigger widget belong to it.
  {
    struct Extent {
      float lo[2] = {1e30f, 1e30f};
      float hi[2] = {-1e30f, -1e30f};
      uint32_t members = 0;
    };
    std::array<Extent, HUD_MAX_BOXES> extents = {};
    for (uint32_t i = 0; i < count; i++) {
      Extent& extent = extents[find(i)];
      extent.members++;
      for (int axis = 0; axis < 2; axis++) {
        extent.lo[axis] = std::min(extent.lo[axis], hud_boxes[i].lo[axis]);
        extent.hi[axis] = std::max(extent.hi[axis], hud_boxes[i].hi[axis]);
      }
    }
    constexpr float NEST_EPS = 0.01f;
    std::array<std::pair<uint32_t, uint32_t>, 64> nested = {};
    uint32_t nested_count = 0;
    for (uint32_t inner_root = 0; inner_root < count && nested_count < nested.size(); inner_root++) {
      if (extents[inner_root].members == 0 || extents[inner_root].members > 3) continue;
      int best = -1;
      float best_area = 1e30f;
      for (uint32_t outer_root = 0; outer_root < count; outer_root++) {
        const Extent& outer = extents[outer_root];
        if (outer_root == inner_root || outer.members <= 3) continue;
        const Extent& inner = extents[inner_root];
        const float area = (outer.hi[0] - outer.lo[0]) * (outer.hi[1] - outer.lo[1]);
        if (inner.lo[0] >= outer.lo[0] - NEST_EPS && inner.hi[0] <= outer.hi[0] + NEST_EPS && inner.lo[1] >= outer.lo[1] - NEST_EPS
            && inner.hi[1] <= outer.hi[1] + NEST_EPS && area < best_area) {
          best = static_cast<int>(outer_root);
          best_area = area;
        }
      }
      if (best >= 0) nested[nested_count++] = {inner_root, static_cast<uint32_t>(best)};
    }
    for (uint32_t n = 0; n < nested_count; n++) merge(nested[n].first, nested[n].second);
  }

  struct Group {
    float lo[2] = {1e30f, 1e30f};
    float hi[2] = {-1e30f, -1e30f};
    float core_area = 0.f;  // non-wide members: area and area-weighted centre
    float core_sum[2] = {};
    float core_lo[2] = {1e30f, 1e30f};  // their extent, cut to the screen
    float core_hi[2] = {-1e30f, -1e30f};
    float all_area = 0.f;
    float all_sum[2] = {};
    bool used = false;
    bool settled = false;  // has a member that was there for a while
    int first_online = -1;  // first member that was drawn with an anchor decided at draw time
    bool has_top_left = false;  // contains a quad of the top-left corner (those are always drawn with the corner anchor)
    bool has_banner = false;  // contains a wide quad >= 1.4 across (YOU DIED / RETRIEVAL strip)
    float banner_sum[2] = {};  // centres of those strips
    uint32_t banner_count = 0;
  };
  std::array<Group, HUD_MAX_BOXES> groups = {};
  for (uint32_t i = 0; i < count; i++) {
    const HudBox& box = hud_boxes[i];
    Group& group = groups[find(i)];
    group.used = true;
    group.settled |= settled[i];
    group.has_top_left |= !box.text && HudInTopLeft(box.lo, box.hi);
    if (box.wide && box.hi[0] - box.lo[0] >= HUD_BANNER_MIN_WIDTH) {
      group.has_banner = true;
      group.banner_count++;
      for (int axis = 0; axis < 2; axis++) {
        group.banner_sum[axis] += 0.5f * (std::clamp(box.lo[axis], -1.f, 1.f) + std::clamp(box.hi[axis], -1.f, 1.f));
      }
    }
    if (box.online && !box.hidden && group.first_online < 0) group.first_online = static_cast<int>(i);  // (was drawn that way)
    const float area = std::max(1e-4f, (box.hi[0] - box.lo[0]) * (box.hi[1] - box.lo[1]));
    group.all_area += area;
    if (!box.wide) group.core_area += area;
    for (int axis = 0; axis < 2; axis++) {
      group.lo[axis] = std::min(group.lo[axis], box.lo[axis]);
      group.hi[axis] = std::max(group.hi[axis], box.hi[axis]);
      const float centre = 0.5f * (std::clamp(box.lo[axis], -1.f, 1.f) + std::clamp(box.hi[axis], -1.f, 1.f));
      group.all_sum[axis] += area * centre;
      if (!box.wide) {
        group.core_sum[axis] += area * centre;
        group.core_lo[axis] = std::min(group.core_lo[axis], std::clamp(box.lo[axis], -1.f, 1.f));
        group.core_hi[axis] = std::max(group.core_hi[axis], std::clamp(box.hi[axis], -1.f, 1.f));
      }
    }
  }

  constexpr float BACKDROP_EPS = 0.01f;
  constexpr float HYSTERESIS = 0.12f;  // how far past the zone limit the centre must move before the anchor changes
  std::array<HudCluster, 96> built = {};
  std::array<int, HUD_MAX_BOXES> cluster_of_root = {};
  cluster_of_root.fill(-1);
  uint32_t built_count = 0;
  for (uint32_t root = 0; root < count && built_count < built.size(); root++) {
    const Group& group = groups[root];
    if (!group.used) continue;
    cluster_of_root[root] = static_cast<int>(built_count);
    HudCluster& cluster = built[built_count++];
    cluster.settled = group.settled;
    const bool has_core = (group.core_area > 0.f);
    float centre[2];
    for (int axis = 0; axis < 2; axis++) {
      cluster.lo[axis] = group.lo[axis];
      cluster.hi[axis] = group.hi[axis];
      centre[axis] = has_core ? group.core_sum[axis] / group.core_area : group.all_sum[axis] / group.all_area;
    }
    cluster.in_place = false;
    // Extent of the widget cut to the screen (the non-wide members, or everything for a banner made of one wide quad).
    float extent_lo[2];
    float extent_hi[2];
    for (int axis = 0; axis < 2; axis++) {
      extent_lo[axis] = has_core ? group.core_lo[axis] : std::clamp(group.lo[axis], -1.f, 1.f);
      extent_hi[axis] = has_core ? group.core_hi[axis] : std::clamp(group.hi[axis], -1.f, 1.f);
    }
    // A widget that fills more than half of the screen on an axis (a menu page, a dialog column) is centred on that axis: its
    // centre of mass says nothing about which edge it belongs to.
    const bool fills[2] = {extent_hi[0] - extent_lo[0] > 1.0f, extent_hi[1] - extent_lo[1] > 1.0f};
    const bool small_cluster = (extent_hi[0] - extent_lo[0] <= 1.9f) && (extent_hi[1] - extent_lo[1] <= 0.8f);
    // A banner (YOU DIED, RETRIEVAL: wide quads only) stays where it is, and so does anything small that sits on a backdrop
    // that is not scaled (a gradient band): both are scaled around their own centre.
    // A banner (YOU DIED, RETRIEVAL: a strip of wide quads, with or without the sprite that carries the word) on a gradient band
    // stays where the game puts it and is scaled around its own centre. A prompt that happens to lie on the fading band is
    // not a banner (nothing in it is >= 1.4 across).
    cluster.in_place = false;
    for (uint32_t b = 0; b < hud_backdrop_count && small_cluster && group.has_banner; b++) {
      const HudBox& back = hud_backdrops[b];
      cluster.in_place |= (group.lo[0] >= back.lo[0] - BACKDROP_EPS && group.hi[0] <= back.hi[0] + BACKDROP_EPS
                           && group.lo[1] >= back.lo[1] - BACKDROP_EPS && group.hi[1] <= back.hi[1] + BACKDROP_EPS);
    }
    // The same widget in the previous frame (the smallest old cluster around this centre) decides how far the centre has to
    // move before the anchor changes.
    const HudCluster* previous = nullptr;
    float previous_area = 1e30f;
    for (uint32_t i = 0; i < hud_cluster_count; i++) {
      const HudCluster& old = hud_clusters[i];
      const float area = (old.hi[0] - old.lo[0]) * (old.hi[1] - old.lo[1]);
      if (centre[0] >= old.lo[0] && centre[0] <= old.hi[0] && centre[1] >= old.lo[1] && centre[1] <= old.hi[1] && area < previous_area) {
        previous = &old;
        previous_area = area;
      }
    }
    if (cluster.in_place && group.banner_count > 0) {
      // The banner is scaled around the centre of its own strips: a prompt that overlaps it (and joined its cluster) must not
      // pull the centre away (the two layers of the word would separate).
      for (int axis = 0; axis < 2; axis++) centre[axis] = group.banner_sum[axis] / static_cast<float>(group.banner_count);
    }
    for (int axis = 0; axis < 2; axis++) {
      if (fills[axis] && !cluster.in_place) {
        cluster.anchor[axis] = 0.f;
        continue;
      }
      if (cluster.in_place) {
        const float target = centre[axis];  // a banner is scaled around its own centre and stays where the game puts it
        const bool keep = (previous != nullptr && previous->in_place && std::fabs(target - previous->anchor[axis]) < 0.05f);
        cluster.anchor[axis] = keep ? previous->anchor[axis] : target;
        continue;
      }
      float low = -HudZone(axis);
      float high = HudZone(axis);
      if (previous != nullptr && !previous->in_place) {
        if (previous->anchor[axis] <= -1.f) {
          low += HYSTERESIS;
        } else if (previous->anchor[axis] >= 1.f) {
          high -= HYSTERESIS;
        } else {
          low -= HYSTERESIS;
          high += HYSTERESIS;
        }
      }
      cluster.anchor[axis] = (centre[axis] <= low) ? -1.f : (centre[axis] >= high ? 1.f : 0.f);
    }
    // A popup that was drawn from its first frame with one anchor keeps it: the anchor was decided when it appeared. Except on an
    // axis the popup straddles around the screen centre: that guess came from whichever quad was drawn first (the right border of
    // the "Save and quit" box gave the whole box the right screen edge, the left end cap of a boss health bar gave it the bottom
    // left corner), while the widget as a whole spans the centre, so it is scaled around the centre on that axis.
    if (!cluster.in_place && !cluster.settled && group.first_online >= 0) {
      for (int axis = 0; axis < 2; axis++) {
        const bool straddles_centre = extent_lo[axis] < -HUD_CENTRE_STRADDLE && extent_hi[axis] > HUD_CENTRE_STRADDLE;
        cluster.anchor[axis] = straddles_centre ? 0.f : hud_boxes[group.first_online].anchor[axis];
      }
    }
    // A widget with a quad in the top-left corner is the corner's as a whole. The emote menu's row of eight slots reaches from the
    // corner past the screen centre: the quads left of x -0.2 were forced to the corner anchor one by one while the others kept the
    // centre anchor of their cluster, so the menu was cut in two.
    if (!cluster.in_place && group.has_top_left) {
      cluster.anchor[0] = -1.f;
      cluster.anchor[1] = 1.f;
    }
  }

  // Remember every box with the cluster it is in, for the next frame.
  std::unordered_map<uint64_t, HudPrevBox> next;
  next.reserve(count);
  std::array<std::array<float, 2>, HUD_MAX_BOXES> kept_anchor = {};
  for (uint32_t i = 0; i < count; i++) {
    const HudBox& box = hud_boxes[i];
    const auto old = hud_prev_boxes.find(box.key);
    const int index = cluster_of_root[find(i)];
    for (int axis = 0; axis < 2; axis++) {
      if (old != hud_prev_boxes.end()) {
        kept_anchor[i][axis] = old->second.anchor[axis];  // sticky
      } else if (box.provisional && index >= 0) {
        kept_anchor[i][axis] = built[index].anchor[axis];  // the cluster is known now
      } else {
        kept_anchor[i][axis] = box.anchor[axis];  // what it was drawn with
      }
    }
    if ((!box.text || !HudGlyphOnBox(i, box.lo, box.hi)) && HudInTopLeft(box.lo, box.hi)) {  // (a glyph on a box follows that box)
      kept_anchor[i][0] = -1.f;
      kept_anchor[i][1] = 1.f;
    } else if (float corner_anchor[2]; HudCornerWidgetAnchor(box.lo, box.hi, corner_anchor)) {
      kept_anchor[i][0] = corner_anchor[0];
      kept_anchor[i][1] = corner_anchor[1];
    }
    next[box.key] = {find(i), age[i], {kept_anchor[i][0], kept_anchor[i][1]}};
  }
  hud_prev_boxes = std::move(next);

  hud_prev_settled_count = 0;
  for (uint32_t i = 0; i < count && hud_prev_settled_count < hud_prev_settled.size(); i++) {
    if (!settled[i]) continue;
    HudPrevSettled& entry = hud_prev_settled[hud_prev_settled_count++];
    for (int axis = 0; axis < 2; axis++) {
      entry.lo[axis] = hud_boxes[i].lo[axis];
      entry.hi[axis] = hud_boxes[i].hi[axis];
      entry.anchor[axis] = kept_anchor[i][axis];
    }
  }

  hud_prev_geometry_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (hud_boxes[i].text) continue;
    HudPrevGeometry& entry = hud_prev_geometry[hud_prev_geometry_count++];
    entry.shaders = hud_box_shaders[i];
    for (int axis = 0; axis < 2; axis++) {
      entry.lo[axis] = hud_boxes[i].lo[axis];
      entry.hi[axis] = hud_boxes[i].hi[axis];
      entry.anchor[axis] = kept_anchor[i][axis];
    }
  }

  hud_clusters = built;
  hud_cluster_count = built_count;
  hud_box_count = 0;
  hud_backdrop_count = 0;
}

inline bool EnsureHudLayouts(reshade::api::device* device) {
  const auto make = [&](uint32_t slot, uint32_t count, reshade::api::pipeline_layout* layout) {
    if (layout->handle != 0) return true;
    reshade::api::pipeline_layout_param param = {};
    param.type = reshade::api::pipeline_layout_param_type::push_constants;
    param.push_constants = {.binding = 0,
                            .dx_register_index = slot,
                            .dx_register_space = 0,
                            .count = count,
                            .visibility = reshade::api::shader_stage::vertex};
    return device->create_pipeline_layout(1, &param, layout);
  };
  return make(HUD_SPRITE_SLOT, HUD_SPRITE_FLOATS, &hud_sprite_layout) && make(HUD_TEXT_SLOT, HUD_TEXT_FLOATS, &hud_text_layout);
}

inline bool IsHudVertexShader(uint32_t hash) {
  return hash == HUD_SPRITE_VERTEX_SHADER || hash == HUD_MASKED_VERTEX_SHADER || hash == HUD_MASKED2_VERTEX_SHADER
         || hash == HUD_MASKED2_OTHER_HASH || hash == HUD_TEXT_VERTEX_SHADER;
}

// Gives the game's own buffer back to the slot we replaced for the previous HUD draw.
inline void RestoreHudConstants(reshade::api::command_list* cmd_list, CommandListState& state) {
  if (state.scissor_overridden) {
    cmd_list->bind_scissor_rects(0, state.game_scissor_count, state.game_scissors.data());
    state.scissor_overridden = false;
  }
  if (state.hud_constants_slot < 0) return;
  const BoundBuffer& bound = state.vs_constant_buffers[state.hud_constants_slot];
  if (bound.handle != 0) {
    const reshade::api::buffer_range range = {{bound.handle}, bound.offset, bound.size};
    cmd_list->push_descriptors(reshade::api::shader_stage::vertex, {0u}, 0,
                               {.binding = static_cast<uint32_t>(state.hud_constants_slot),
                                .count = 1,
                                .type = reshade::api::descriptor_type::constant_buffer,
                                .descriptors = &range});
  }
  state.hud_constants_slot = -1;
}

// Scales the HUD draw the game is about to make (see the header comment).
// Returns true when the draw must not be made (a popup quad whose anchor is not settled yet).
inline bool ApplyHudScale(reshade::api::command_list* cmd_list, CommandListState& state, const DrawArgs& args) {
  if (args.indexed) return false;
  const bool mirrored_buffer = WantsVertexMirror() && state.vertex_buffer0 != 0 && hud_vertex_buffers.contains(state.vertex_buffer0);
  if (mirrored_buffer) {
    // Where the game appends its next vertices (see the mirror windows above).
    const auto bytes = vertex_buffer_contents.find(state.vertex_buffer0);
    HudVertexWindow& window = HudWindowFor(state.vertex_buffer0, bytes != vertex_buffer_contents.end() ? bytes->second.size() : 0);
    const uint64_t read_end = state.vertex_offset0 + static_cast<uint64_t>(args.first + args.count) * state.vertex_stride0;
    window.cursor = std::max(window.cursor, read_end);
    if (read_end > window.window_start) window.needed = std::max(read_end - window.window_start, window.needed - (window.needed >> 5));
  }
  if (!IsHudVertexShader(state.vs_hash)) return false;
  // Gameplay only: the title screen is left alone. A frame in which the camera was not seen (a menu opening) still counts
  // for two seconds, otherwise that frame would show the HUD unscaled for an instant.
  if (!camera_captured_this_frame && frame_number - last_scene_frame > 120) return false;
  if (state.vertex_buffer0 != 0) hud_vertex_buffers.insert(state.vertex_buffer0);
  const bool text = (state.vs_hash == HUD_TEXT_VERTEX_SHADER);
  const uint32_t slot = (text ? HUD_TEXT_SLOT : HUD_SPRITE_SLOT);
  const uint32_t floats = (text ? HUD_TEXT_FLOATS : HUD_SPRITE_FLOATS);
  const BoundBuffer& bound = state.vs_constant_buffers[slot];
  if (bound.handle == 0) return false;
  const float scale = settings.hud_scale;
  if (scale == 1.f) return false;
  if (!text) tracked_filter_buffers.insert(bound.handle);  // the matrix is mirrored from the next write on

  // The text shader's b0 is the 320 byte scene camera buffer (mirrored as such); the sprite shaders' b8 is a filter-style mirror.
  const std::vector<uint8_t>* matrix_data = nullptr;
  if (text) {
    if (const auto found = camera_buffer_contents.find(bound.handle); found != camera_buffer_contents.end()) matrix_data = &found->second;
  } else if (const auto found = filter_buffer_contents.find(bound.handle); found != filter_buffer_contents.end()) {
    matrix_data = &found->second;
  }
  const auto vertex_bytes = vertex_buffer_contents.find(state.vertex_buffer0);
  bool ok = false;
  std::array<float, HUD_SPRITE_FLOATS> m = {};
  float lo[2] = {1e30f, 1e30f};
  float hi[2] = {-1e30f, -1e30f};
  if (matrix_data != nullptr && matrix_data->size() >= bound.offset + floats * 4) {
    std::memcpy(m.data(), matrix_data->data() + bound.offset, floats * 4);
    uint32_t components = (text ? 2 : 4);
    uint32_t position_offset = 0;
    if (const auto layout = position_layouts.find(state.input_layout); layout != position_layouts.end() && layout->second.second >= 2) {
      position_offset = layout->second.first;
      components = std::min<uint32_t>(layout->second.second, 4);
    }
    const uint32_t stride = state.vertex_stride0;
    if (vertex_bytes != vertex_buffer_contents.end() && stride >= position_offset + components * 4 && args.count > 0) {
      const uint32_t count = std::min<uint32_t>(args.count, 64);
      // The vertices must lie in the part of the buffer the mirror has copied since the game last discarded it.
      const uint64_t read_begin = state.vertex_offset0 + static_cast<uint64_t>(args.first) * stride;
      const uint64_t read_end = read_begin + static_cast<uint64_t>(count) * stride;
      const auto window = hud_vertex_windows.find(state.vertex_buffer0);
      const bool stale = (window != hud_vertex_windows.end()
                          && (read_begin < window->second.valid_lo || read_end > window->second.valid_hi));
      uint32_t valid = 0;
      for (uint32_t i = 0; i < count && !stale; i++) {
        const uint64_t at = read_begin + static_cast<uint64_t>(i) * stride + position_offset;
        if (at + components * 4 > vertex_bytes->second.size()) break;
        float v[4] = {0.f, 0.f, 0.f, 1.f};
        std::memcpy(v, vertex_bytes->second.data() + at, components * 4);
        float clip[4];
        if (text) {  // cbParam: row vector times matrix
          for (int c = 0; c < 4; c++) clip[c] = v[0] * m[c] + v[1] * m[4 + c] + m[12 + c];
        } else {  // cbFeShader: transposed matrix, rows dotted with the vertex
          for (int r = 0; r < 4; r++) clip[r] = m[4 * r] * v[0] + m[4 * r + 1] * v[1] + m[4 * r + 2] * v[2] + m[4 * r + 3] * v[3];
        }
        if (!(std::fabs(clip[3]) > 1e-6f)) continue;
        for (int axis = 0; axis < 2; axis++) {
          const float ndc = clip[axis] / clip[3];
          lo[axis] = std::min(lo[axis], ndc);
          hi[axis] = std::max(hi[axis], ndc);
        }
        valid++;
      }
      ok = (valid > 0);
    }
  }
  if (!ok) return false;
  // Full-screen fades and horizontal gradient bands that run past both screen edges stay as they are; a parchment, a panel or a
  // long thin divider scales with its widget (a panel just wider than 1.8 used to stay full size behind a scaled menu).
  const float box_width = hi[0] - lo[0];
  const float box_height = hi[1] - lo[1];
  const bool fullscreen = (box_width > 1.9f && box_height > 1.9f);
  const bool large = fullscreen || (box_width > 1.95f && box_height < 1.2f);
  const bool wide = !large && (box_width > 1.0f || box_height > 1.0f);  // stretched quad (bar fill, backdrop): joins a widget but does not decide its anchor
  const bool menu_trigger = !large && box_width * box_height >= 0.4f && box_height >= 0.45f;
  if (menu_trigger) hud_menu_frame = true;
  if (large && !fullscreen && hud_backdrop_count < hud_backdrops.size()) {
    hud_backdrops[hud_backdrop_count++] = {{lo[0], lo[1]}, {hi[0], hi[1]}, false, false, 0, {}, false, false, false, false, false};  // a band, not a fade
  }

  // Anchor: the screen edge the whole widget (cluster of touching quads) belongs to, the centre otherwise.
  //  - the same quad as in the previous frame: the cluster it was in;
  //  - a changed quad: what it was drawn on (glyph), the settled quad it lies inside (bar fill, damage preview), the cluster
  //    of the previous frame when that is a young popup;
  //  - anything else is new, usually the first frame of a popup: it follows the new quads drawn just before it, so the whole
  //    popup gets one anchor from its first frame on (not the one of whatever bigger widget it happens to lie over).
  const uint64_t key = HudBoxKey(state.vs_hash, state.ps_hash, lo, hi);
  const uint32_t shaders = HudShaderPair(state.vs_hash, state.ps_hash);
  float anchor[2] = {0.f, 0.f};
  bool online = false;
  bool provisional = false;
  bool menu = false;
  const HudCluster* cluster = large ? nullptr : FindHudCluster(lo, hi);
  const auto known = hud_prev_boxes.find(key);
  // A wide strip inside a gradient band that is not scaled: a banner (YOU DIED, RETRIEVAL) once it is 1.4 across. The strip grows
  // while it fades in, starting well below that; its first frames get the edge anchor of an ordinary quad. It must not carry that
  // anchor on as a continuing quad, or the banner stays on the edge anchor and slides away from its (unscaled) word.
  bool on_backdrop = false;
  for (uint32_t b = 0; b < hud_backdrop_count && wide && box_height <= 0.8f && !on_backdrop; b++) {
    const HudBox& back = hud_backdrops[b];
    on_backdrop = lo[0] >= back.lo[0] - 0.01f && hi[0] <= back.hi[0] + 0.01f && lo[1] >= back.lo[1] - 0.01f && hi[1] <= back.hi[1] + 0.01f;
  }
  const HudPrevGeometry* continued = text ? nullptr : FindHudContinuation(shaders, lo, hi);
  if (continued != nullptr && on_backdrop && HudEdgeAnchor(continued->anchor)) continued = nullptr;
  bool marker = false;
  bool marker_confirmed = false;
  // The lock-on crosshair pulses when a target is locked: its three sprites grow from about 0.11 x 0.19 to 0.24 x 0.43 and back in 20
  // frames, the third one a little larger than the other two. Beyond the usual 0.2 x 0.2 the size limit therefore also admits TALL
  // sprites (height >= 1.2 x width, like the crosshair; the wide, stacked highlight behind a menu option must not pass as one), and
  // their copy tolerance grows with the size. With the old limits the crosshair was an ordinary HUD piece for those frames: anchored
  // to the top edge, drawn far off the target, then snapping into place when the pulse ended.
  const bool tall_sprite = box_height >= 1.2f * box_width;
  const bool marker_size = (box_width <= 0.2f && box_height <= 0.2f) || (tall_sprite && box_width <= HUD_MARKER_MAX_SIZE && box_height <= HUD_MARKER_MAX_SIZE);
  if (!large && !text && (state.vs_hash == HUD_SPRITE_VERTEX_SHADER || state.vs_hash == HUD_MASKED_VERTEX_SHADER) && box_width >= 0.04f
      && box_height >= 0.01f && marker_size) {
    const float mx = 0.5f * (lo[0] + hi[0]);
    const float my = 0.5f * (lo[1] + hi[1]);
    const bool corner_widget = (hi[0] <= -0.4f && my >= -0.95f && my <= -0.1f) || (lo[0] >= 0.42f && my <= -0.745f);
    // A sprite that lies on a settled widget (a menu column, a dialog) is part of that widget, never a lock-on crosshair: the glow layer
    // of the selected menu icon is a stack of near-identical sprites like the crosshair, was taken for one now and then, and was scaled
    // around its own centre while the rest of the menu scaled toward the corner (the "Read History" icon drifted away from its submenu).
    bool on_widget = false;
    for (uint32_t i = 0; i < hud_prev_settled_count && !on_widget; i++) {
      const HudPrevSettled& settled = hud_prev_settled[i];
      on_widget = HudEdgeAnchor(settled.anchor) && (settled.hi[0] - settled.lo[0]) * (settled.hi[1] - settled.lo[1]) >= 0.02f
                  && mx >= settled.lo[0] && mx <= settled.hi[0] && my >= settled.lo[1] && my <= settled.hi[1];
    }
    if (!corner_widget && !on_widget) {
      const float copy_tolerance = tall_sprite ? std::max(0.012f, 0.07f * box_height) : 0.012f;
      const auto near_copy = [&](const std::array<float, 4>& other) {
        return std::fabs(other[0] - lo[0]) < copy_tolerance && std::fabs(other[1] - lo[1]) < copy_tolerance
               && std::fabs(other[2] - hi[0]) < copy_tolerance && std::fabs(other[3] - hi[1]) < copy_tolerance;
      };
      marker_confirmed = hud_recent_quad_count >= 2 && near_copy(hud_recent_quads[0]) && near_copy(hud_recent_quads[1]);
      bool where_it_was = false;
      for (uint32_t i = 0; i < hud_marker_prev_count; i++) {
        where_it_was |= (std::fabs(mx - hud_marker_prev[i][0]) < 0.12f && std::fabs(my - hud_marker_prev[i][1]) < 0.12f);
      }
      marker = marker_confirmed || where_it_was;
    }
    hud_recent_quads[1] = hud_recent_quads[0];
    hud_recent_quads[0] = {lo[0], lo[1], hi[0], hi[1]};
    hud_recent_quad_count = std::min<uint32_t>(hud_recent_quad_count + 1, 2);
  } else if (!large) {
    hud_recent_quad_count = 0;  // something else in between: a group is a run of consecutive quads
  }
  if (large) {
    for (int axis = 0; axis < 2; axis++) anchor[axis] = HudAnchor(lo[axis], hi[axis], axis);
  } else if (menu_trigger || ((hud_menu_frame || hud_menu_frame_prev) && hud_menu_keys_prev.contains(key))) {
    menu = true;  // (quads drawn before the page of a menu frame are known from the previous menu frame)
  } else if (marker) {
    anchor[0] = 0.5f * (lo[0] + hi[0]);
    anchor[1] = 0.5f * (lo[1] + hi[1]);
    bool known_group = !marker_confirmed;  // only a third copy keeps a group alive for the next frame
    for (uint32_t i = 0; i < hud_marker_now_count && !known_group; i++) {
      known_group |= (std::fabs(hud_marker_now[i][0] - anchor[0]) < 0.05f && std::fabs(hud_marker_now[i][1] - anchor[1]) < 0.05f);
    }
    if (!known_group && hud_marker_now_count < hud_marker_now.size()) hud_marker_now[hud_marker_now_count++] = {anchor[0], anchor[1]};
  } else if (known != hud_prev_boxes.end()) {
    // The same quad as in the previous frame keeps the anchor it was given, whatever the clusters look like now.
    anchor[0] = known->second.anchor[0];
    anchor[1] = known->second.anchor[1];
  } else if (continued != nullptr) {
    // The same quad as in the previous frame, only its box moved or grew a little (the strip behind RETRIEVAL grows every frame, so
    // its key changes every frame): it keeps the anchor and is never held back.
    anchor[0] = continued->anchor[0];
    anchor[1] = continued->anchor[1];
  } else {
    bool found = false;
    if (text) {
      const float centre_x = 0.5f * (lo[0] + hi[0]);
      const float centre_y = 0.5f * (lo[1] + hi[1]);
      for (uint32_t i = hud_box_count; i-- > 0 && !found;) {
        const HudBox& parent_box = hud_boxes[i];
        if (parent_box.text || centre_x < parent_box.lo[0] || centre_x > parent_box.hi[0] || centre_y < parent_box.lo[1]
            || centre_y > parent_box.hi[1]) {
          continue;
        }
        anchor[0] = parent_box.anchor[0];
        anchor[1] = parent_box.anchor[1];
        provisional = parent_box.provisional;
        menu = parent_box.menu;
        found = true;
      }
    }
    if (!found) {
      const float area = std::max(1e-6f, (hi[0] - lo[0]) * (hi[1] - lo[1]));
      float best_area = 1e30f;
      for (uint32_t i = 0; i < hud_prev_settled_count; i++) {
        const HudPrevSettled& settled = hud_prev_settled[i];
        if (!HudEdgeAnchor(settled.anchor)) continue;
        const float overlap_x = std::min(hi[0], settled.hi[0]) - std::max(lo[0], settled.lo[0]);
        const float overlap_y = std::min(hi[1], settled.hi[1]) - std::max(lo[1], settled.lo[1]);
        const float settled_area = (settled.hi[0] - settled.lo[0]) * (settled.hi[1] - settled.lo[1]);
        if (overlap_x > 0.f && overlap_y > 0.f && overlap_x * overlap_y >= 0.7f * area && settled_area >= area
            && settled_area < best_area) {
          best_area = settled_area;
          anchor[0] = settled.anchor[0];
          anchor[1] = settled.anchor[1];
          found = true;
        }
      }
    }
    if (!found && hud_menu_frame) {  // new while a menu is open: part of the menu
      menu = true;
      found = true;
    }
    if (!found) {  // drawn right after a settled quad it touches (a drop-down under its tab)
      const HudBox current = {{lo[0], lo[1]}, {hi[0], hi[1]}, wide, text, key, {}, false, false, false, false, false};
      for (uint32_t i = hud_box_count; i-- > 0 && hud_box_count - i <= 3 && !found;) {
        if (hud_boxes[i].settled && HudEdgeAnchor(hud_boxes[i].anchor) && HudBoxesJoin(current, hud_boxes[i], true)) {
          anchor[0] = hud_boxes[i].anchor[0];
          anchor[1] = hud_boxes[i].anchor[1];
          found = true;
        }
      }
    }
    if (!found && cluster != nullptr && !cluster->settled) {
      anchor[0] = cluster->anchor[0];
      anchor[1] = cluster->anchor[1];
      provisional = true;
      found = true;
    }
    if (!found && hud_banner_this_frame && lo[1] >= hud_banner_y_range[0] - 0.02f && hi[1] <= hud_banner_y_range[1] + 0.02f) {
      anchor[0] = hud_banner_centre[0];  // the sprite with the word: same centre as the strip
      anchor[1] = hud_banner_centre[1];
      online = true;
      provisional = true;
      found = true;
    }
    if (!found && on_backdrop && box_width >= HUD_BANNER_MIN_WIDTH) {
      // A banner on a gradient band that is not scaled (YOU DIED, RETRIEVAL): scaled around its own centre from its first frame.
      anchor[0] = 0.5f * (lo[0] + hi[0]);
      anchor[1] = 0.5f * (lo[1] + hi[1]);
      hud_banner_this_frame = true;
      hud_banner_centre[0] = anchor[0];
      hud_banner_centre[1] = anchor[1];
      hud_banner_y_range[0] = lo[1];
      hud_banner_y_range[1] = hi[1];
      online = true;
      provisional = true;
      found = true;
    }
    if (!found) {
      constexpr float MARGIN = 0.02f;
      HudOnlineGroup* group = nullptr;
      for (uint32_t i = 0; i < hud_online_count && group == nullptr; i++) {
        HudOnlineGroup& candidate = hud_online_groups[i];
        if (lo[0] <= candidate.hi[0] + MARGIN && hi[0] >= candidate.lo[0] - MARGIN && lo[1] <= candidate.hi[1] + MARGIN
            && hi[1] >= candidate.lo[1] - MARGIN) {
          group = &candidate;
        }
      }
      if (group == nullptr && hud_online_count < hud_online_groups.size()) {
        group = &hud_online_groups[hud_online_count++];
        *group = {{lo[0], lo[1]}, {hi[0], hi[1]}, {HudAnchor(lo[0], hi[0], 0), HudAnchor(lo[1], hi[1], 1)}};
      }
      if (group != nullptr && !text && HudInTopLeft(lo, hi)) {  // a popup that reaches into the top-left corner belongs to it as a whole
        group->anchor[0] = -1.f;
        group->anchor[1] = 1.f;
      }
      for (int axis = 0; axis < 2; axis++) {
        if (group != nullptr) {
          group->lo[axis] = std::min(group->lo[axis], lo[axis]);
          group->hi[axis] = std::max(group->hi[axis], hi[axis]);
        }
        anchor[axis] = (group != nullptr) ? group->anchor[axis] : HudAnchor(lo[axis], hi[axis], axis);
      }
      online = true;
      provisional = true;
    }
  }
  if (!large && !menu && !marker) {
    // Text that sits on the dark band across the bottom of the screen (NPC subtitles, prompts) is centred on the screen: its
    // anchor is the bottom centre, whatever glyph was drawn first (the first letter of a long line lies in the left part and
    // used to give the whole line the left corner, which moved it far to the left).
    bool on_bottom_band = false;
    for (uint32_t b = 0; b < hud_backdrop_count && !on_bottom_band; b++) {
      const HudBox& back = hud_backdrops[b];
      on_bottom_band = back.lo[0] <= -0.9f && back.hi[0] >= 0.9f && back.lo[1] <= -0.95f && back.hi[1] <= -0.2f
                       && lo[0] >= back.lo[0] - 0.01f && hi[0] <= back.hi[0] + 0.01f && lo[1] >= back.lo[1] - 0.01f
                       && hi[1] <= back.hi[1] + 0.01f && hi[1] - lo[1] <= 0.25f;
    }
    if (on_bottom_band) {
      anchor[0] = 0.f;
      anchor[1] = -1.f;
    } else if ((!text || !HudGlyphOnBox(hud_box_count, lo, hi)) && HudInTopLeft(lo, hi)) {
      // (a glyph on a box keeps the anchor of that box: the notification text was split at x -0.2; a glyph on no box, like the arrow
      // beside the menu title, is forced like any other top-left quad, otherwise its first-frame guess left it offset now and then)
      anchor[0] = -1.f;
      anchor[1] = 1.f;
    } else {
      HudCornerWidgetAnchor(lo, hi, anchor);
    }
  }
  int pushed_index = -1;
  if (!large && !marker && hud_box_count < hud_boxes.size()) {
    const auto previous = hud_prev_boxes.find(key);
    const bool settled_now = (previous != hud_prev_boxes.end() && previous->second.age + 1 >= HUD_OLD_AGE);
    pushed_index = static_cast<int>(hud_box_count);
    hud_box_shaders[hud_box_count] = shaders;
    hud_boxes[hud_box_count++] = {{lo[0], lo[1]}, {hi[0], hi[1]}, wide, text, key, {anchor[0], anchor[1]}, online, settled_now,
                                  provisional, menu, false};
  }
  if (menu) {  // left as the game draws it
    hud_menu_keys.insert(key);
    return false;
  }

  // The game's scissor rectangle (lists clip their text with it) is scaled with the element, around the same anchor.
  const float offset_x = anchor[0] * (1.f - scale);
  const float offset_y = anchor[1] * (1.f - scale);
  reshade::api::rect scissor = {};
  const float target_width = static_cast<float>(state.render_target_width);
  const float target_height = static_cast<float>(state.render_target_height);
  if (state.game_scissor_count > 0 && target_width > 0.f && target_height > 0.f) {
    const reshade::api::rect& game = state.game_scissors[0];
    const bool covers_target = (game.left <= 0 && game.top <= 0 && game.right >= static_cast<int32_t>(target_width)
                                && game.bottom >= static_cast<int32_t>(target_height));
    if (!covers_target && game.right > game.left && game.bottom > game.top) {
      const auto to_pixels_x = [&](int32_t px) { return (scale * (2.f * px / target_width - 1.f) + offset_x + 1.f) * 0.5f * target_width; };
      const auto to_pixels_y = [&](int32_t py) { return (1.f - (scale * (1.f - 2.f * py / target_height) + offset_y)) * 0.5f * target_height; };
      scissor.left = static_cast<int32_t>(std::floor(to_pixels_x(game.left)));
      scissor.right = static_cast<int32_t>(std::ceil(to_pixels_x(game.right)));
      scissor.top = static_cast<int32_t>(std::floor(to_pixels_y(game.top)));
      scissor.bottom = static_cast<int32_t>(std::ceil(to_pixels_y(game.bottom)));
    }
  }
  if (large || !EnsureHudLayouts(cmd_list->get_device())) return false;
  // A quad of a popup that just appeared has an anchor guessed from the quads drawn so far; the clusters are only known at the
  // end of the frame. It is held back for a frame or two (never more than two frames in a row) instead of being shown in a
  // place it will not stay at.
  // Every provisional quad is held back, small ones too: the first frame of a lock-on (crosshair layers, target bar), of the "Save and
  // quit" box (3 of its 20 pieces were drawn at the right-edge guess) and of a banner's second stage (two small ornaments were drawn
  // 0.3 screens too low for one frame) all flashed at the guessed anchor when the first few provisional quads were let through. A lone
  // small quad that moves a little every frame no longer needs that exception: it continues the quad of the previous frame (see
  // FindHudContinuation) and is not provisional.
  if (provisional && hud_hide_streak < 2 && hud_hide_cooldown == 0) {
    hud_hidden_this_frame = true;
    if (pushed_index >= 0) hud_boxes[pushed_index].hidden = true;
    return true;
  }
  if (scissor.right > scissor.left) {
    cmd_list->bind_scissor_rects(0, 1, &scissor);
    state.scissor_overridden = true;
  }

  // clip' = (s * x + a_x (1 - s) * w, s * y + a_y (1 - s) * w, z, w), written into the matrix.
  if (text) {
    for (int row = 0; row < 4; row++) {
      m[4 * row + 0] = scale * m[4 * row + 0] + offset_x * m[4 * row + 3];
      m[4 * row + 1] = scale * m[4 * row + 1] + offset_y * m[4 * row + 3];
    }
  } else {
    for (int c = 0; c < 4; c++) {
      m[c] = scale * m[c] + offset_x * m[12 + c];
      m[4 + c] = scale * m[4 + c] + offset_y * m[12 + c];
    }
  }
  cmd_list->push_constants(reshade::api::shader_stage::vertex, text ? hud_text_layout : hud_sprite_layout, 0, 0, floats,
                           m.data());
  state.hud_constants_slot = static_cast<int>(slot);
  return false;
}

// At present: the clusters for the next frame are built from this frame's quads, and the per-frame state starts over.
inline void HudEndFrame() {
  if (hud_box_count > 0) BuildHudClusters();
  if (!hud_menu_frame) hud_menu_keys.clear();  // menu keys only mean something in a menu frame
  hud_menu_keys_prev = std::move(hud_menu_keys);
  hud_menu_keys.clear();
  hud_banner_this_frame = false;
  hud_marker_prev = hud_marker_now;
  hud_marker_prev_count = hud_marker_now_count;
  hud_marker_now_count = 0;
  hud_recent_quad_count = 0;
  hud_menu_frame_prev = hud_menu_frame;
  hud_menu_frame = false;
  hud_backdrop_count = 0;
  hud_hide_streak = hud_hidden_this_frame ? hud_hide_streak + 1 : 0;
  if (hud_hide_streak >= 2) {
    hud_hide_cooldown = 90;  // animated popups would otherwise blink: draw them as they come for a while
  } else if (hud_hide_cooldown > 0) {
    hud_hide_cooldown--;
  }
  hud_hidden_this_frame = false;
  hud_online_count = 0;
}

}  // namespace ds2

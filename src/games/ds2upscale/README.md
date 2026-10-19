# DS2 Upscaler (ds2upscale)

NVIDIA DLSS and AMD FSR 3.1 for Dark Souls II: Scholar of the First Sin (DirectX 11, x64), as a ReShade add-on. It runs next to the
RenoDX HDR add-on in `../darksouls2` but does not depend on it. Licences of the bundled code: `THIRD_PARTY_NOTICES.md` (AMD
FidelityFX SDK, MIT; NVIDIA NGX; RenoDX; ReShade).

## What it does

- Jitters the scene passes, builds motion vectors from the game's depth buffer, camera matrices and velocity map, runs DLSS or FSR 3.1
  on the scene before depth of field / motion blur / bloom, and copies the result back into the game's scene texture.
- Below native resolution the scene is drawn into the top-left part of the game's render targets; the passes that read those
  textures (SSAO, water, depth of field, sun rays, motion blur) are replaced or corrected, and the depth buffer is rebuilt at full size.
- FSR 3.1 is AMD's own compute shaders (`fsr3/`) run with plain Direct3D 11 calls (`ds2_fsr.hpp`); DLSS goes through NGX (`ds2_dlss.hpp`).
- Characters get the game's own velocity map (needs the game's Motion Blur option on), wind-animated foliage is flagged in the stencil
  buffer, and both feed FSR's reactive mask. The game's FXAA and motion blur can be switched off, and the HUD can be scaled.

## Overlay (ReShade > Add-ons > DS2 Upscaler)

| Control | Default | What it does |
| --- | --- | --- |
| Enable Upscaler | on | Switches the add-on on / off. |
| Upscaler | FSR 3.1 | FSR 3.1 (any GPU) or DLSS (RTX, needs `nvngx_dlss.dll` next to the executable). |
| Upscaling mode | Native | Native (AA only), Quality, Balanced, Performance, Ultra Performance. |
| DLSS preset | M | Default, J, K, L, M (DLSS only). |
| Disable motion blur | on | Replaces the game's blur by a passthrough; keep the game's own Motion Blur option on. |
| Disable FXAA | on | Skips the game's FXAA while the upscaler is on. |
| Texture sharpening for characters | off | Applies the world's sharper texture sampling to characters too. |
| Image sharpening, strength | off, 20 | 0 - 100 (CAS for DLSS, RCAS for FSR). |
| HUD scale | 1.00 | 0.10 - 2. |

Values are saved in `ReShade.ini`, section `[DS2Upscale]`. The behaviour that is not in the overlay (jitter, mip bias, FSR reactivity,
camera-cut distance, ...) is the `Tuning` struct in `ds2_settings.hpp`, compiled in as constants.

## Build

```
tools\devshell.cmd cmake --build --preset clang-x64-release --target ds2upscale
```

Output: `renodx\build\Release\renodx-ds2upscale.addon64`. Build the Release preset: the Debug preset does not optimise and the add-on's
handlers run for every draw call of the game. Copy the file next to `DarkSoulsII.exe` (ReShade with add-on support installed) together
with `nvngx_dlss.dll` from `external/DLSS/lib/Windows_x86_64/rel/`.

## Developer build

Set `DS2_DEVELOPER` to 1 in `ds2_build.hpp` (and rebuild) to get the developer tools; the overlay then says "Developer build".

- `ds2upscale.cfg` next to the executable (key=value lines, re-read when it changes) overrides the settings and tuning values. Keys:
  `enabled`, `mode`, `upscaler`, `preset`, `motion_blur_off`, `disable_fxaa`, `character_sharpening`, `image_sharpening`,
  `image_sharpening_strength`, `hud_scale`, `stabilize_weight`, `mip_bias_offset`, `fsr_mip_bias_offset`, `character_bias`,
  `fsr_sharpness`, `fsr_character_reactive`, `fsr_velocity_factor`, `fsr_reactiveness`, `fsr_shading_change`, `fsr_accumulation`,
  `fsr_min_disocclusion`, `fsr_foliage_reactive`, `exposure`, `camera_cut_distance`, `hud_zone`, `hud_zone_y`, `dump_frames=N`
  (writes `ds2upscale_dump_*`), `reset_history=1`, `reset_defaults=1`.
- `ds2upscale_status.txt` is rewritten every 30 frames: state, settings, `gpu_ms:` (GPU time of the upscale step from timestamp queries)
  and, with `DS2_PROFILE`, a `cpu:` line (time in every event handler and the render thread's CPU time per frame).

Source layout: see the header comment of `addon.cpp`.

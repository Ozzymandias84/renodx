# Dark Souls II: Scholar of the First Sin (DX11, x64) - HDR

RenoDX HDR add-on. Build target `darksouls2`, output `renodx-darksouls2.addon64`. It works on its own or next to the DS2 Upscaler
add-on in `../ds2upscale`. User documentation: the `README.md` of the release package.

## What it does

The late frame is: float scene -> tonemap + LUT grade -> FXAA -> HUD sprites -> copy -> final brightness table -> swapchain. All passes
after the grade use screen-sized RGBA8 buffers, and the HUD is blended into the same buffer as the scene.

| Draw | Shader (pixel) | Handling |
|---|---|---|
| Tonemap + grade | `0x8DE1836D` | Replaced. Vanilla reproduces the game 1:1. Vanilla+ rebuilds the signal the game's LUT clamps away (the larger of the curve + bloom value before the LUT clamp and the float scene normalised by the tone table's SDR white point, found per pixel by searching the table in the shader), keeps the game's grade, applies the user grading (Exposure, Highlights, Shadows, Contrast, Saturation, Highlight Saturation, Blowout, Flare) with Hue Shift and Per Channel Blowout pulling hue and bleaching toward the game's real per-channel clipped image (compared at the same luminance), and rolls the result off to the display peak with a Hermite spline in PQ on the brightest channel, which keeps hue. Output goes through `RenderIntermediatePass`, so the HUD blends in encoded space at UI brightness. The Bloom slider (0-200%, 100% = game) scales the game's bloom only where the pixel's own curve value is dim, so objects keep their brightness and only the halo changes. |
| FXAA, luma, copy | `0xDA4A34C6`, `0x27943DD0`, `0xE5206B32` | Untouched. Run on the upgraded RGBA16F buffers. |
| HUD | `0x8888E31A`, `0xE580C0B1`, `0xF9D38F1E` | Untouched. Blend into the scene buffer at UI brightness. |
| Final pass | `0xAB5E17CE` | Replaced. Bypasses the 1D brightness table, which clamps to [0, 1]. |

Screen-sized `r8g8b8a8_unorm` render targets are upgraded to `r16g16b16a16_float` with resource view cloning. Output is HDR10 through
the swap chain proxy; SDR output is selectable. The Vanilla+ grading code is adapted from the Dark Souls Remastered mod
(`../darksouls/tonemap.hlsli`).

## Settings and defaults (RenoDX panel, Preset #1)

Output Mode HDR10, Tone Mapper Vanilla+, Peak Brightness 1000 (the undo arrow sets the display's reported peak), Game Brightness 203,
UI Brightness 203, SDR EOTF Emulation 2.2, Hue Shift 100, Per Channel Blowout 50, colour grading neutral (Exposure 1.00, the others
50, Blowout 0, Flare 0, Scene Grading 100), Bloom 100.

## Known limitations

- The in-game brightness / gamma option has no effect (its 1D table is bypassed). Use Game Brightness.
- The game's own content has little range above SDR white (sun glow ~1.8x, brightest glints ~1.4x luminance in Majula), so default
  highlights peak near 300 nits at 203 nit Game Brightness.
- Dark Souls II online play and anti-cheat have not been evaluated. Play offline.

## Build and check

1. `tools\devshell.cmd cmake --build --preset clang-x64-release --target darksouls2`
2. Copy `build\Release\renodx-darksouls2.addon64` next to `DarkSoulsII.exe` (remove other RenoDX scene add-ons first).
3. Start the game and confirm `ReShade.log` lists `renodx-darksouls2.addon64` as loaded.
4. Set Tone Mapper to Vanilla and compare against SDR output: the image should match.
5. Set Tone Mapper to Vanilla+ and look at sun, sky and water glints in Majula: highlights should keep their detail.
6. Check that the HUD stays at the chosen UI Brightness while Game Brightness changes the scene only.

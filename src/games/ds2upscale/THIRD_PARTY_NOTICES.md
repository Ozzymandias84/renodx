# Third-party notices

The DS2 Upscaler add-on contains or uses the following third-party software.

## This project

DS2 Upscaler (src/games/ds2upscale) and DS2 HDR (src/games/darksouls2): Copyright (c) 2026 Ozzymandias84, MIT licence (LICENSE-DS2-Mods-MIT.txt). The third-party parts below keep their own licences.

## AMD FidelityFX SDK (FSR 3.1) - MIT licence

The FSR 3.1 backend (`ds2_fsr.hpp`, `fsr3/`) runs AMD's FidelityFX Super Resolution 3.1 upscaler passes. The compute shaders and shader
headers under `fsr3/amd/` are copies of these files of the AMD FidelityFX SDK (`Kits/FidelityFX/api/internal/gpu/` and
`Kits/FidelityFX/upscalers/fsr3/`), compiled for Direct3D 11 and embedded in the add-on. Each of them is listed in the SDK's
`LICENSE.txt` as licensed under the MIT licence below and carries the same notice in its header. The SDK's separate binary-only
licence applies to other parts of the SDK (its prebuilt libraries), none of which is used or distributed here. The complete,
unmodified licence file of the SDK is kept next to the sources as `fsr3/LICENSE-AMD-FidelityFX-SDK.md`.

Local changes to AMD's files are limited to what the Direct3D 11 / Shader Model 5.0 compiler needs: include paths are made relative,
two structs are zero-initialised and one `inout` parameter is copied locally (compiler error X3508), and the luma pyramid pass
numbers its UAV slots differently to stay within Direct3D 11's limit. The options in `fsr3/fsr3_options.hlsli` select the
no-wave-operations path of AMD's single-pass downsampler.

```
Copyright (C) Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

"AMD" and "FidelityFX" are trademarks of Advanced Micro Devices, Inc.

## NVIDIA DLSS (NGX SDK)

DLSS support uses the NVIDIA NGX SDK, linked into the add-on, and NVIDIA's runtime library `nvngx_dlss.dll` (version 310.7.0.0,
unmodified, NVIDIA-signed) distributed next to it. Both are provided under the NVIDIA RTX SDKs LICENSE (v. March 14, 2024), which is
included as `LICENSE-NVIDIA-RTX-SDK.txt` and applies to the library for everyone who receives it. In short: it may only be used
together with this add-on and not as a stand-alone product, it may not be reverse engineered, DLSS is licensed for systems with
NVIDIA GPUs only, and NVIDIA does not sponsor or endorse this project. NVIDIA DLSS is a trademark of NVIDIA Corporation.

## RenoDX

The add-on is built on the RenoDX framework (`src/utils`, `src/mods`, `src/shaders`), MIT licence,
Copyright (c) 2025 Carlos Lopez Jr. (https://github.com/clshortfuse/renodx). The Vanilla+ colour grading of the HDR add-on is
adapted from RenoDX's Dark Souls Remastered mod.

## ReShade and Dear ImGui

The add-on is a ReShade add-on and uses the ReShade add-on API header (`reshade.hpp`, BSD-3-Clause or MIT, Copyright (C) Patrick
Mours / crosire) and the Dear ImGui interface library that ReShade provides (MIT licence, Copyright (c) Omar Cornut).

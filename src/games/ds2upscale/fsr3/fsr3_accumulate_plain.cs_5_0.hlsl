// FSR 3.1 upscaler pass 'accumulate' without the sharpening pass (AMD FidelityFX SDK, MIT) compiled for DirectX 11.
#include "fsr3_options.hlsli"
#undef FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING
#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0  // this variant writes the final image itself (no sharpening pass)
#include "amd/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_accumulate_pass.hlsl"

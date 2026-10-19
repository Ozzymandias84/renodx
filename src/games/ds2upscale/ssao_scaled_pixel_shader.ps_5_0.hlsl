// Patched copy of the game's SSAO pass 0xACABD0A4 (half-resolution ambient occlusion with its own temporal
// accumulation) for render scale < 1. The scene is drawn into the top-left [0, scale] part of the render targets and the
// quad vertex shader hands this shader UVs in that sub-rectangle ("scaled UV"), but the game's maths assume the whole
// texture: the view-space positions rebuilt from the UV, the world-space sampling radius and the reprojection into the
// previous frame. Without this patch the occlusion gets distorted (the lateral distances shrink by the scale, flat ground
// self-occludes) and the temporal accumulation rejects its history, so the AO flickers more the lower the resolution.
// Differences to the original decompilation are marked with "// scaled:".
cbuffer UvScale : register(b11) {
  float4 g_uv_scale;  // xy: render size / full size
};

cbuffer cbFilter : register(b6)
{
  float4 g_vDepthComputeParam : packoffset(c0);
  float4 g_vAOScaleOffset : packoffset(c1);
  float4 g_vBlendScale : packoffset(c2);
  float4 g_avSampleOffsets[16] : packoffset(c3);
  float4x4 g_mInvProj : packoffset(c19);
  float4 g_vScreenSize : packoffset(c23);
  float4x4 g_mView : packoffset(c24);
  float4 g_vAOScaleOffset2 : packoffset(c28);
  float4 g_vSampleRangeParam : packoffset(c29);
  float4x4 g_mCurToPrevScreen : packoffset(c30);
  float4 g_vPrevFrameStrengh : packoffset(c34);
  float4 g_vMiscParam : packoffset(c35);
}

SamplerState DepthTexSampler_sampler_s : register(s0);
SamplerState NormalSampler_sampler_s : register(s1);
SamplerState PrevAOSampler_sampler_s : register(s2);
SamplerState PrevAOSampler_Point_sampler_s : register(s3);
SamplerState RandomRotationTexSampler_sampler_s : register(s4);
Texture2D<float4> DepthTexSampler_texture : register(t0);
Texture2D<float4> NormalSampler_texture : register(t1);
Texture2D<float4> PrevAOSampler_texture : register(t2);
Texture2D<float4> PrevAOSampler_Point_texture : register(t3);
Texture2D<float4> RandomRotationTexSampler_texture : register(t4);

#define cmp -

void main(
  float4 v0 : SV_Position0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8;

  const float2 scale = g_uv_scale.xy;
  const float2 full_uv = v1.xy / scale;  // scaled: the UV the game's maths expect (0..1 over the whole picture)

  r0.x = DepthTexSampler_texture.SampleLevel(DepthTexSampler_sampler_s, v1.xy, 0).x;
  r1.xy = full_uv * float2(2,-2) + float2(-1,1);  // scaled: NDC from the full-size UV
  r1.z = r0.x;
  r1.w = 1;
  r2.x = dot(r1.xyzw, g_mInvProj._m00_m10_m20_m30);
  r2.y = dot(r1.xyzw, g_mInvProj._m01_m11_m21_m31);
  r2.z = dot(r1.xyzw, g_mInvProj._m02_m12_m22_m32);
  r0.y = dot(r1.xyzw, g_mInvProj._m03_m13_m23_m33);
  r0.yzw = r2.xyz / r0.yyy;
  r0.x = cmp(r0.x == 1.000000);
  if (r0.x != 0) {
    r2.z = g_vAOScaleOffset2.x + g_vAOScaleOffset2.y;
  } else {
    r0.x = 1 / r0.w;
    r3.xyz = NormalSampler_texture.Sample(NormalSampler_sampler_s, v1.xy).xyz;
    r3.xyz = r3.xyz * float3(2,2,2) + float3(-1,-1,-1);
    r3.w = dot(r3.xyz, r3.xyz);
    r3.w = rsqrt(r3.w);
    r3.xyz = r3.xyz * r3.www;
    r4.x = dot(r3.xyz, g_mView._m00_m10_m20);
    r4.y = dot(r3.xyz, g_mView._m01_m11_m21);
    r4.z = dot(r3.xyz, g_mView._m02_m12_m22);
    r3.xy = g_vScreenSize.xy * v1.xy;  // texel position in the half-resolution map (also valid in the sub-rectangle)
    r3.xy = float2(0.015625,0.015625) * r3.xy;
    r3.xy = RandomRotationTexSampler_texture.SampleLevel(RandomRotationTexSampler_sampler_s, r3.xy, 0).xy;
    r3.xy = float2(-0.5,-0.5) + r3.xy;
    r3.xy = r3.xy + r3.xy;
    r3.w = g_vSampleRangeParam.x / r0.w;
    r3.w = max(g_vSampleRangeParam.y, r3.w);
    r3.w = min(g_vSampleRangeParam.z, r3.w);
    r3.z = -r3.x;
    r5.w = 1;
    r6.w = 1;
    r4.w = 0;
    r7.x = 0;
    while (true) {
      r7.y = cmp((int)r7.x >= 8);
      if (r7.y != 0) break;
      r8.x = dot(g_avSampleOffsets[r7.x].yx, r3.xy);
      r8.y = dot(g_avSampleOffsets[r7.x].yx, r3.yz);
      r7.yz = g_vScreenSize.zw * r8.xy;
      // scaled: the offset the game wants is in full-size UV units (same world distance as at native resolution); the
      // texture coordinate inside the sub-rectangle needs it times the scale.
      const float2 offset = r7.yz * r3.ww;
      r8.xy = offset * scale + v1.xy;
      r5.z = DepthTexSampler_texture.SampleLevel(DepthTexSampler_sampler_s, r8.xy, 0).x;
      r5.xy = (full_uv + offset) * float2(2,-2) + float2(-1,1);
      r8.x = dot(r5.xyzw, g_mInvProj._m00_m10_m20_m30);
      r8.y = dot(r5.xyzw, g_mInvProj._m01_m11_m21_m31);
      r8.z = dot(r5.xyzw, g_mInvProj._m02_m12_m22_m32);
      r5.x = dot(r5.xyzw, g_mInvProj._m03_m13_m23_m33);
      r8.xyz = r8.xyz / r5.xxx;
      r5.xy = -offset * scale + v1.xy;
      r6.z = DepthTexSampler_texture.SampleLevel(DepthTexSampler_sampler_s, r5.xy, 0).x;
      r6.xy = (full_uv - offset) * float2(2,-2) + float2(-1,1);
      r5.x = dot(r6.xyzw, g_mInvProj._m00_m10_m20_m30);
      r5.y = dot(r6.xyzw, g_mInvProj._m01_m11_m21_m31);
      r5.z = dot(r6.xyzw, g_mInvProj._m02_m12_m22_m32);
      r6.x = dot(r6.xyzw, g_mInvProj._m03_m13_m23_m33);
      r5.xyz = r5.xyz / r6.xxx;
      r6.xyz = r8.xyz + -r0.yzw;
      r7.y = dot(r6.xyz, r6.xyz);
      r7.y = rsqrt(r7.y);
      r6.xyz = r7.yyy * r6.xyz;
      r7.yzw = r5.xyz + -r0.yzw;
      r5.x = dot(r7.yzw, r7.yzw);
      r5.x = rsqrt(r5.x);
      r7.yzw = r7.yzw * r5.xxx;
      r8.w = r5.z;
      r5.xy = r0.ww * float2(0.899999976,0.899999976) + -r8.zw;
      r5.xy = r5.xy * r0.xx;
      r5.xy = saturate(float2(10,10) * r5.xy);
      r5.x = max(r5.x, r5.y);
      r5.y = dot(r6.xyz, r4.xyz);
      r5.z = dot(r7.yzw, r4.xyz);
      r5.yz = saturate(float2(1,1) + -r5.yz);
      r5.y = r5.y + r5.z;
      r5.y = 0.5 * r5.y;
      r5.z = r5.y * r5.y;
      r5.y = -r5.y * r5.y + 1;
      r5.x = r5.x * r5.y + r5.z;
      r4.w = g_avSampleOffsets[r7.x].z * r5.x + r4.w;
      r7.x = (int)r7.x + 1;
    }
    r0.x = r4.w * g_vAOScaleOffset2.x + g_vAOScaleOffset2.y;
    r1.xy = full_uv;  // scaled: the reprojection matrix works on full-size UVs
    r3.x = dot(r1.xyzw, g_mCurToPrevScreen._m00_m10_m20_m30);
    r3.y = dot(r1.xyzw, g_mCurToPrevScreen._m01_m11_m21_m31);
    r3.z = dot(r1.xyzw, g_mCurToPrevScreen._m02_m12_m22_m32);
    r0.y = dot(r1.xyzw, g_mCurToPrevScreen._m03_m13_m23_m33);
    r1.xyz = r3.xyz / r0.yyy;
    // scaled: the previous AO map also lives in the sub-rectangle (never read beyond its last valid texel)
    const float2 prev_uv = clamp(r1.xy * scale, 0.5 * g_vScreenSize.zw, scale - 0.5 * g_vScreenSize.zw);
    r0.y = g_vDepthComputeParam.x + -r1.z;
    r0.y = -g_vDepthComputeParam.y / r0.y;
    r0.y = g_vDepthComputeParam.z * r0.y;
    r0.z = PrevAOSampler_texture.SampleLevel(PrevAOSampler_sampler_s, prev_uv, 0).w;
    r1.xyz = PrevAOSampler_Point_texture.SampleLevel(PrevAOSampler_Point_sampler_s, prev_uv, 0).xyz;
    r1.x = dot(r1.xyz, float3(1.53787005e-005,0.00392156886,1));
    r1.x = -r1.x * g_vDepthComputeParam.z + r0.y;
    r0.y = g_vPrevFrameStrengh.z * r0.y;
    r0.y = saturate(abs(r1.x) / r0.y);
    r0.y = -r0.y * g_vPrevFrameStrengh.y + 1;
    r1.x = cmp(0 >= r3.z);
    r0.y = g_vPrevFrameStrengh.x * r0.y;
    r0.y = r1.x ? 0 : r0.y;
    r0.z = r0.z + -r0.x;
    r2.z = r0.y * r0.z + r0.x;
  }
  r0.x = r0.w / g_vDepthComputeParam.z;
  r2.xyw = float3(255,1,65025) * r0.xxx;
  r2.xw = frac(r2.xw);
  r2.xy = -r2.wx * float2(0.00392156886,0.00392156886) + r2.xy;
  o0.xyzw = r2.wxyz;
}

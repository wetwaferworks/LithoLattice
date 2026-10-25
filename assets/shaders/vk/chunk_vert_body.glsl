// Shared body of vk/chunk.vert (16-byte PackedVertex, stride 16) and vk/chunk12.vert (Packed12, stride 12,
// LITHO_PACKED12). Both decode to identical varyings.
// Vulkan chunk vertices: same outputs as ../chunk.vert (inputs: the 12-byte attribute prefix of the 16-byte
// PackedVertex, or the Packed12 vertex; 0.0.1.59-b F59-2 comment fix), with the
// frame values in a uniform block and the sub-chunk offset as a push constant. Clip space is
// converted from OpenGL conventions (y up, z in -w..w) to Vulkan (y down, z in 0..w).
// The 16-bit position is read as xy + z (R16G16 + R16 are guaranteed vertex formats; R16G16B16
// is optional in Vulkan).
#include "frame.glsl"
#if defined(LITHO_MEGA_SSBO)
// Mega-draw V2b (Direction JD, design Archie, implementation Cortana): per-draw records (5 vec4 each) in a storage
// buffer (set 0, binding 5) indexed by gl_InstanceIndex (= the indirect command's firstInstance). Only the draw
// index goes to the fragment stage; LITHO_MEGA_VARY (profiling variant) passes the 5 vec4s as flat varyings instead.
layout(std430, set = 0, binding = 5) readonly buffer MegaDraws { vec4 uMegaD[]; };
#define uOffset uMegaD[uint(gl_InstanceIndex) * 5u].xyz
#if defined(LITHO_MEGA_VARY)
layout(location = 8) flat out vec4 vDrawOP;
layout(location = 9) flat out vec4 vPulseP0; layout(location = 10) flat out vec4 vPulseP1;
layout(location = 11) flat out vec4 vTraceP0; layout(location = 12) flat out vec4 vTraceP1;
#else
layout(location = 8) flat out uint vDraw;
#endif
#elif defined(LITHO_MEGA)
// Mega-draw V2 (Direction JD, design Archie, implementation Cortana): the per-draw values come from the
// per-frame instance buffer (binding 1, one 80-byte record per draw, firstInstance = draw index) instead of
// push constants, and go on to the fragment stage as flat varyings (locations 8..12).
layout(location = 8) in vec4 aDrawOP;  // xyz offset, w pass
layout(location = 9) in vec4 aPulseP0; layout(location = 10) in vec4 aPulseP1;
layout(location = 11) in vec4 aTraceP0; layout(location = 12) in vec4 aTraceP1;
layout(location = 8) flat out vec4 vDrawOP;
layout(location = 9) flat out vec4 vPulseP0; layout(location = 10) flat out vec4 vPulseP1;
layout(location = 11) flat out vec4 vTraceP0; layout(location = 12) flat out vec4 vTraceP1;
#define uOffset aDrawOP.xyz
#else
layout(push_constant) uniform PulsePush { vec3 uOffset; float uPass; vec4 uPulseP0; vec4 uPulseP1; vec4 uTraceP0; vec4 uTraceP1; };
#endif
#if defined(LITHO_PULL)
// Mega-Pipeline Phase 2 (Direction JD, design Archie (Mega-Pipeline brief), implementation Cortana): vertex pulling.
// No vertex attributes: quad gl_VertexIndex >> 2, corner gl_VertexIndex & 3, read from the arena block (binding 6,
// picked by the push constant) as 64-bit packed quads; decoded into the same values the Packed12 attributes carry
// (render_vk/packed_quad64.h is the CPU mirror and the bit-exact check).
layout(std430, set = 0, binding = 6) readonly buffer ArenaQuads { uvec2 q[]; } uArena[64];
layout(push_constant) uniform PullPush { uint uBlock; };
uvec2 aPosXY; uvec4 aInfo; uvec2 aShade; uint aPosZ; uint aLayerHi;
int pullSign(uint c) { return c == 1u ? 1 : (c == 2u ? -1 : 0); }
void pullDecode() {
  const uint qi = uint(gl_VertexIndex) >> 2u, k = uint(gl_VertexIndex) & 3u;
  const uvec2 q = uArena[uBlock].q[qi];
  const uvec2 e = uArena[uBlock].q[qi - (q.y >> 17u)];
  uvec3 p = uvec3(q.x & 63u, (q.x >> 6u) & 63u, (q.x >> 12u) & 63u);
  const uint W = (q.x >> 18u) & 63u, H = (q.x >> 24u) & 63u, face = (q.x >> 30u) | ((q.y & 1u) << 2u);
  const uint ca = (q.y >> (1u + 2u * k)) & 1u, cb = (q.y >> (2u + 2u * k)) & 1u, ao = (q.y >> (9u + 2u * k)) & 3u;
  const uint n = face >> 1u, a = (n + 1u) % 3u, b = (n + 2u) % 3u;
  p[a] += ca * W; p[b] += cb * H;
  const uint m = e.y >> 16u;
  const int du = (pullSign(m & 3u) * int(ca * W) + pullSign((m >> 2u) & 3u) * int(cb * H)) * 16;
  const int dv = (pullSign((m >> 4u) & 3u) * int(ca * W) + pullSign((m >> 6u) & 3u) * int(cb * H)) * 16;
  aPosXY = p.xy * 16u; aPosZ = p.z * 16u;
  aInfo = uvec4(uint(int(e.y & 255u) + du) & 255u, uint(int((e.y >> 8u) & 255u) + dv) & 255u,
                face | (ao << 3u) | (((e.y >> 24u) & 7u) << 5u), e.x & 255u);
  aLayerHi = (e.x >> 8u) & 255u;
  aShade = uvec2((e.x >> 16u) & 255u, e.x >> 24u);
}
#else
layout(location = 0) in uvec2 aPosXY;  // 1/16 block units, local to the sub-chunk
layout(location = 1) in uvec4 aInfo;   // u, v (1/16), face | ao<<3 | rot<<5, layer (bits 0..7)
layout(location = 2) in uvec2 aShade;  // value, mode
#if defined(LITHO_PULL)
#elif defined(LITHO_PACKED12)
// 0.0.1.59 [P59-1] Packed12 (stride 12; Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization
// Operand, the Bitmaster and the Bitmasker; layout correction and build by Cortana): z[0..8] | layer[8..14] << 9.
layout(location = 3) in uint aPosZL;
#else
layout(location = 3) in uint aPosZ;
layout(location = 4) in uint aLayerHi;  // Cortana 2026-10-15 1628: CA-03 high layer byte (PackedVertex byte 15)
#endif
#endif  // LITHO_PULL
layout(location = 0) out vec3 vWorld; layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vAO; layout(location = 3) out vec2 vUV;
layout(location = 4) flat out int vLayer; layout(location = 5) flat out int vVal;
layout(location = 6) flat out int vMode; layout(location = 7) flat out int vRot;
const vec3 N[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
void main() {
#if defined(LITHO_MEGA_SSBO) && defined(LITHO_MEGA_VARY)
  {
    const uint d5 = uint(gl_InstanceIndex) * 5u;
    vDrawOP = uMegaD[d5]; vPulseP0 = uMegaD[d5 + 1u]; vPulseP1 = uMegaD[d5 + 2u];
    vTraceP0 = uMegaD[d5 + 3u]; vTraceP1 = uMegaD[d5 + 4u];
  }
#elif defined(LITHO_MEGA_SSBO)
  vDraw = uint(gl_InstanceIndex);
#elif defined(LITHO_MEGA)
  vDrawOP = aDrawOP; vPulseP0 = aPulseP0; vPulseP1 = aPulseP1; vTraceP0 = aTraceP0; vTraceP1 = aTraceP1;
#endif
#if defined(LITHO_PULL)
  pullDecode();
#elif defined(LITHO_PACKED12)
  uint aPosZ = aPosZL & 511u;
  uint aLayerHi = aPosZL >> 9u;
#endif
  vWorld = vec3(uvec3(aPosXY, aPosZ)) / 16.0 + uOffset;
  vNormal = N[aInfo.z & 7u];
  vAO = float((aInfo.z >> 3u) & 3u) / 3.0;
  vRot = int((aInfo.z >> 5u) & 3u) | (int(aInfo.z & 7u) << 2);  // 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): face index in bits 2..4 (LUT normal mapping)
  vUV = vec2(aInfo.xy) / 16.0;
  vLayer = int(aInfo.w | (aLayerHi << 8u)); vVal = int(aShade.x); vMode = int(aShade.y & 63u);
  vec4 p = uVP * vec4(vWorld, 1.0);
  gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
  // Cortana 2026-10-16 (JD foliage LOD, renderer.h kShadeLod*): LOD-tagged quads draw by the
  // sub-chunk's horizontal centre distance; the other set collapses outside the clip volume.
  uint lod = aShade.y & 192u;
  if (lod != 0u) {
    vec2 c = uOffset.xz + vec2(8.0) - uCam.xz;
    bool far = dot(c, c) > 32.0 * 32.0;
    if ((lod == 64u && far) || (lod == 128u && !far)) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  }
}

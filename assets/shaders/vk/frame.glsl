// Vulkan per-frame uniform block (set 0, binding 0), shared by the chunk stages. std140; the C++
// mirror is FrameUniforms in src/render_vk/vk_frame_uniforms.h (keep both in sync). Arrays of
// int use a 16-byte stride under std140, hence the ivec4-sized slots on the C++ side.
const int MAX_ANIMS = 8;
layout(std140, set = 0, binding = 0) uniform Frame {
  mat4 uVP;
  vec3 uCam; float uFogEnd;
  vec3 uSky; float uTime;
  int uTile; int uRepMask; int uMemMask; int uDebug;
  int uWirePal[2]; int uRing[2];
  int uLampPal[3]; int uLampMask[3]; int uLampRim[3];
  int uAnimStart[MAX_ANIMS]; int uAnimLen[MAX_ANIMS]; float uAnimMs[MAX_ANIMS];
  int uAnimSeq[64];
  int uDot[2]; int uDigits;
  vec4 uPulse;  // x = ground-pulse time (s), y = ground-pulse on (wafer ground pulse, JD 2026-10-11), z = heartbeat bloom gain (0.0.1.53 [C53-3] (Proposed by JD, Built by Cortana))
  vec4 uTrace;  // x = ground-trace time (s), y = ground-trace on (wafer ground traces, JD 2026-10-11)
  int uLeafGlow[4]; int uTwinkle;
  int uTintMask[32];
  // JD Vulkan push (Cortana 2026-10-16): effect toggles + material maps (C++ mirror: FrameUniforms).
  ivec4 uFx;        // x = effect bits (1 normal maps, 2 per-pixel light, 4 bloom, 8 pulse, 16 particles, 32 fog, 64 bulb blend), y = particles
  vec4 uFxP;        // x = bloom gain, y = fog density, z = pulse clock (sim seconds), w = pulse Hz
  ivec4 uFxMap[128];  // texture layer -> (glow-mask layer << 16) | normal-map layer (0 = none), 4 per slot  // 2026-10-16 kShadeTraceTint mask layers  // 2026-10-16 crystal twinkle (kShadeLeaf)
};

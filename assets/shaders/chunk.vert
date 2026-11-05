#version 330 core
// Chunk vertices: 12-byte PackedVertex (see render/renderer.h). Base pack shader; packs may
// override it with assets/packs/<pack>/shaders/chunk.vert.
layout(location = 0) in uvec3 aPos;    // 1/16 block units, local to the sub-chunk
layout(location = 1) in uvec4 aInfo;   // u, v (1/16), face | ao<<3 | rot<<5, layer
layout(location = 2) in uvec2 aShade;  // value, mode
layout(location = 3) in uint aLayerHi;  // Cortana 2026-10-15 1628: CA-03 high layer byte (PackedVertex byte 15)
#ifdef LK_MEGADRAW
// Mega-draw variant (GL33; the renderer injects LK_MEGADRAW after the
// version line): per-mesh offset/pulse/trace arrive as instanced
// attributes (locations 4..8, the renderer's bindArenaAttribs layout:
// offset(3) + pulseP0(4) + pulseP1(4) + traceP0(4) + traceP1(4)) and go
// to the fragment stage as flat varyings, the same pattern the Vulkan
// LITHO_MEGA variant uses. Classic path below is untouched.
layout(location = 4) in vec3 aInstOff;
layout(location = 5) in vec4 aInstP0;
layout(location = 6) in vec4 aInstP1;
layout(location = 7) in vec4 aInstT0;
layout(location = 8) in vec4 aInstT1;
flat out vec4 vPulseP0; flat out vec4 vPulseP1; flat out vec4 vTraceP0; flat out vec4 vTraceP1;
#endif
uniform mat4 uVP;
uniform vec3 uOffset;
uniform vec3 uCam;  // Cortana 2026-10-16: shared with chunk.frag (foliage LOD)
out vec3 vWorld; out vec3 vNormal; out float vAO; out vec2 vUV;
flat out int vLayer; flat out int vVal; flat out int vMode; flat out int vRot;
const vec3 N[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
void main() {
#ifdef LK_MEGADRAW
  vec3 meshOff = aInstOff;
  vPulseP0 = aInstP0; vPulseP1 = aInstP1; vTraceP0 = aInstT0; vTraceP1 = aInstT1;
#else
  vec3 meshOff = uOffset;
#endif
  vWorld = vec3(aPos) / 16.0 + meshOff;
  vNormal = N[aInfo.z & 7u];
  vAO = float((aInfo.z >> 3u) & 3u) / 3.0;
  vRot = int((aInfo.z >> 5u) & 3u);
  vUV = vec2(aInfo.xy) / 16.0;
  vLayer = int(aInfo.w | (aLayerHi << 8u)); vVal = int(aShade.x); vMode = int(aShade.y & 63u);
  // s2-PlayD: build 9 (PlayB, JD t4332u) readout drop restored: readouts sit 1/64 above the model top. Render only.
  if (vMode == 15 && vNormal.y > 0.5) vWorld.y -= 3.0 / 64.0;
  gl_Position = uVP * vec4(vWorld, 1.0);
  // Cortana 2026-10-16 (JD foliage LOD, renderer.h kShadeLod*): LOD-tagged quads draw by the
  // sub-chunk's horizontal centre distance; the other set collapses outside the clip volume.
  uint lod = aShade.y & 192u;
  if (lod != 0u) {
    vec2 c = meshOff.xz + vec2(8.0) - uCam.xz;
    bool far = dot(c, c) > 32.0 * 32.0;
    if ((lod == 64u && far) || (lod == 128u && !far)) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  }
}

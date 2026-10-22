#version 330 core
// Chunk vertices: 12-byte PackedVertex (see render/renderer.h). Base pack shader; packs may
// override it with assets/packs/<pack>/shaders/chunk.vert.
layout(location = 0) in uvec3 aPos;    // 1/16 block units, local to the sub-chunk
layout(location = 1) in uvec4 aInfo;   // u, v (1/16), face | ao<<3 | rot<<5, layer
layout(location = 2) in uvec2 aShade;  // value, mode
layout(location = 3) in uint aLayerHi;  // Cortana 2026-10-15 1628: CA-03 high layer byte (PackedVertex byte 15)
uniform mat4 uVP;
uniform vec3 uOffset;
uniform vec3 uCam;  // Cortana 2026-10-16: shared with chunk.frag (foliage LOD)
out vec3 vWorld; out vec3 vNormal; out float vAO; out vec2 vUV;
flat out int vLayer; flat out int vVal; flat out int vMode; flat out int vRot;
const vec3 N[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
void main() {
  vWorld = vec3(aPos) / 16.0 + uOffset;
  vNormal = N[aInfo.z & 7u];
  vAO = float((aInfo.z >> 3u) & 3u) / 3.0;
  vRot = int((aInfo.z >> 5u) & 3u);
  vUV = vec2(aInfo.xy) / 16.0;
  vLayer = int(aInfo.w | (aLayerHi << 8u)); vVal = int(aShade.x); vMode = int(aShade.y & 63u);
  gl_Position = uVP * vec4(vWorld, 1.0);
  // Cortana 2026-10-16 (JD foliage LOD, renderer.h kShadeLod*): LOD-tagged quads draw by the
  // sub-chunk's horizontal centre distance; the other set collapses outside the clip volume.
  uint lod = aShade.y & 192u;
  if (lod != 0u) {
    vec2 c = uOffset.xz + vec2(8.0) - uCam.xz;
    bool far = dot(c, c) > 32.0 * 32.0;
    if ((lod == 64u && far) || (lod == 128u && !far)) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
  }
}

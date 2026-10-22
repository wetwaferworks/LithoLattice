#version 450
#extension GL_GOOGLE_include_directive : require
// JD Vulkan push (Cortana 2026-10-16, clean-room): GPU "Trace spark" particles. No vertex buffer:
// each particle is 6 vertices of a camera-facing quad generated from gl_VertexIndex. Positions are
// a hash of the particle index wrapped into a cube around the camera, so the field follows the
// player without popping. Render-only (render clock uTime); the sim never sees any of it.
#include "frame.glsl"
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec2 vQ;
uint lithoHash(uint x) {  // integer avalanche hash (technique: Chris Wellons' hash-prospector, public domain)
  x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x;
}
float h01(uint x) { return float(lithoHash(x) & 0xffffffu) / 16777215.0; }
void main() {
  const vec2 C[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
  const int i = gl_VertexIndex / 6;
  const vec2 corner = C[gl_VertexIndex % 6];
  const float R = 28.0;
  const uint s = uint(i) * 4u;
  vec3 seed = vec3(h01(s), h01(s + 1u), h01(s + 2u)) * (2.0 * R);
  float sp = 0.15 + 0.45 * h01(s + 3u);
  float t = uTime;
  vec3 p = seed + vec3(0.6 * sin(t * 0.37 + float(i)), t * sp, 0.6 * cos(t * 0.29 + float(i) * 1.7));
  p = mod(p - uCam + vec3(R), vec3(2.0 * R)) - vec3(R) + uCam;
  vec3 toCam = uCam - p;
  float dist = length(toCam);
  vec3 f = toCam / max(dist, 1e-3);
  vec3 r = normalize(cross(abs(f.y) > 0.95 ? vec3(1, 0, 0) : vec3(0, 1, 0), f));
  vec3 u = cross(f, r);
  float size = 0.045 + 0.04 * h01(s + 5u);
  vec3 wp = p + (r * corner.x + u * corner.y) * size;
  float tw = 0.55 + 0.45 * sin(t * (2.0 + 3.0 * h01(s + 6u)) + float(i));
  float fade = smoothstep(R, R * 0.55, dist) * smoothstep(0.4, 1.5, dist);
  vCol = vec4(((i & 1) == 0) ? vec3(1.0, 0.16, 0.20) : vec3(0.12, 0.66, 1.0), fade * tw);
  vQ = corner;
  vec4 c = uVP * vec4(wp, 1.0);
  gl_Position = vec4(c.x, -c.y, (c.z + c.w) * 0.5, c.w);
}

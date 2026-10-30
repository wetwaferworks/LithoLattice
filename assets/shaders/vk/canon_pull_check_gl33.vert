#version 330 core
// Render pipeline M12: GL 3.3 compile check for canonical_pull_legacy.glsl (RG/RGBA32UI TBO pulling).
#extension GL_GOOGLE_include_directive : require
#include "canonical_pull_legacy.glsl"
uniform usamplerBuffer uCanonTbo;
uniform int uCanonBase; uniform int uCanonFb; uniform int uCanonLod;
flat out uvec4 vInfo;
void main() {
  int q = gl_VertexID / 6, k = gl_VertexID % 6;
  uint corner = uint(k < 3 ? k : (k == 3 ? 0 : k - 1));
  CanonLegacyVertex cv;
  if (q < uCanonFb) cv = canonPullFallback(texelFetch(uCanonTbo, uCanonBase + q * 4 + int(corner)));
  else cv = canonPullQuadLegacy(texelFetch(uCanonTbo, uCanonBase + uCanonFb * 4 + (q - uCanonFb)), corner, uint(uCanonLod));
  vInfo = cv.info + uvec4(cv.shade, cv.layerHi, 0u);
  gl_Position = vec4(vec3(cv.pos) / 16.0, 1.0);
}
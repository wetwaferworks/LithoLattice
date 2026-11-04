#version 330 core
// Render pipeline M4: compile check for canonical_quad_decode.glsl (GL 3.3, RG32UI TBO pulling, Q16).
#extension GL_GOOGLE_include_directive : require
#include "canonical_quad_decode.glsl"
uniform usamplerBuffer uQuads;
uniform usamplerBuffer uPalette;
uniform uvec2 uBase;  // (quadBaseOffset, paletteBaseOffset)
out vec3 vColor;
void main() {
  uvec2 raw = texelFetch(uQuads, int(uBase.x) + (gl_VertexID >> 2)).xy;
  CanonCorner c = canonPullCorner(raw, uint(gl_VertexID) & 3u);
  CanonQuad q = canonUnpackQuad(raw);
  CanonPalette p = canonUnpackPalette(texelFetch(uPalette, int(uBase.y + q.paletteIdx)).xy);
  vColor = canonTintRgb(p.rgb565Tint) * (float(c.ao) / 3.0);
  gl_Position = vec4(vec3(c.pos) * canonUnitScale(q.coordScale, 1.0), 1.0);
}

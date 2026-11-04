#version 450
// Render pipeline M4: compile check for canonical_quad_decode.glsl (Vulkan / SPIR-V).
#extension GL_GOOGLE_include_directive : require
#include "canonical_quad_decode.glsl"
layout(std430, set = 0, binding = 0) readonly buffer Quads { uvec2 quads[]; };
layout(std430, set = 0, binding = 1) readonly buffer Pal { uvec2 palette[]; };
layout(location = 0) out vec3 vColor;
void main() {
  uvec2 raw = quads[gl_VertexIndex >> 2];
  CanonCorner c = canonPullCorner(raw, uint(gl_VertexIndex) & 3u);
  CanonQuad q = canonUnpackQuad(raw);
  CanonPalette p = canonUnpackPalette(palette[q.paletteIdx]);
  vColor = canonTintRgb(p.rgb565Tint) * (float(c.ao) / 3.0) + canonFaceNormal(q.face) * 0.0 + vec3(c.uv, p.layerId) * 0.0;
  gl_Position = vec4(vec3(c.pos) * canonUnitScale(q.coordScale, 1.0), 1.0);
}

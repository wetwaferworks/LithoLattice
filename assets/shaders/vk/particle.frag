#version 450
// JD Vulkan push (Cortana 2026-10-16): soft additive spark; also feeds the bloom (glow) target.
layout(location = 0) in vec4 vCol;
layout(location = 1) in vec2 vQ;
layout(location = 0) out vec4 frag;
layout(location = 1) out vec4 fragGlow;
void main() {
  float k = exp(-4.0 * dot(vQ, vQ)) * vCol.a;
  if (k < 0.01) discard;
  frag = vec4(vCol.rgb * k, k);
  fragGlow = vec4(vCol.rgb * k * 1.5, k);
}

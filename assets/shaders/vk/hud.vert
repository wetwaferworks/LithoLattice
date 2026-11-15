#version 450
// Vulkan HUD: pixel-space quads (rects and font glyphs); Vulkan clip space is y down already.
// tex (textured icons, GUI draw-cost program 2026-11-10): 0 = font/solid, 1 = icon atlas.
layout(push_constant) uniform Push { vec2 uScreen; };
layout(location = 0) in vec2 aPos; layout(location = 1) in vec2 aUV; layout(location = 2) in vec4 aCol;
layout(location = 3) in float aTex;
layout(location = 0) out vec2 vUV; layout(location = 1) out vec4 vCol; layout(location = 2) out float vTex;
void main() {
  vUV = aUV; vCol = aCol; vTex = aTex;
  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, aPos.y / uScreen.y * 2.0 - 1.0, 0.0, 1.0);
}

#version 330 core
// HUD: pixel-space quads (rects and font glyphs).
// tex (textured icons, GUI draw-cost program 2026-11-10): 0 = font/solid, 1 = icon atlas.
layout(location = 0) in vec2 aPos; layout(location = 1) in vec2 aUV; layout(location = 2) in vec4 aCol;
layout(location = 3) in float aTex;
uniform vec2 uScreen;
out vec2 vUV; out vec4 vCol; out float vTex;
void main() {
  vUV = aUV; vCol = aCol; vTex = aTex;
  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}

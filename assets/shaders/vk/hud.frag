#version 450
layout(set = 0, binding = 3) uniform sampler2D uFont;
layout(set = 0, binding = 8) uniform sampler2D uHudIcons;
layout(location = 0) in vec2 vUV; layout(location = 1) in vec4 vCol; layout(location = 2) in float vTex;
layout(location = 0) out vec4 frag;
void main() {
  if (vTex > 0.5) {   // textured icon: NEAREST-sample the icon atlas, tint by vertex colour
    frag = texture(uHudIcons, vUV) * vCol;
    return;
  }
  float a = vUV.x < 0.0 ? 1.0 : texture(uFont, vUV).r;   // u < 0: solid rectangle
  frag = vec4(vCol.rgb, vCol.a * a);
}

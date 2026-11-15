#version 330 core
in vec2 vUV; in vec4 vCol; in float vTex;
uniform sampler2D uFont; uniform sampler2D uHudIcons; out vec4 frag;
void main() {
  if (vTex > 0.5) {   // textured icon: NEAREST-sample the icon atlas, tint by vertex colour
    frag = texture(uHudIcons, vUV) * vCol;
    return;
  }
  float a = vUV.x < 0.0 ? 1.0 : texture(uFont, vUV).r;   // u < 0: solid rectangle
  frag = vec4(vCol.rgb, vCol.a * a);
}

#version 450
// JD Vulkan push (Cortana 2026-10-16, clean-room): separable Gaussian blur of the HDR glow target
// at half resolution (mode 0), and the additive composite onto the frame with an exponential
// tone curve (mode 1). Technique credits: CHANGES.md "Credits / inspirations".
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(push_constant) uniform PostPush { vec4 uP; };  // xy = tap step (uv), z = strength, w = mode
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 frag;
void main() {
  if (uP.w < 0.5) {
    // 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): the same 9-tap Gaussian in 5 fetches:
    // the linear sampler blends texel pairs (1,2) and (3,4) at their weighted centres.
    const float O[2] = float[2](1.3846153846, 3.2307692308);
    const float W[3] = float[3](0.2270270270, 0.3162162162, 0.0702702703);
    vec3 c = texture(uSrc, vUV).rgb * W[0];
    for (int k = 0; k < 2; ++k) {
      c += texture(uSrc, vUV + uP.xy * O[k]).rgb * W[k + 1];
      c += texture(uSrc, vUV - uP.xy * O[k]).rgb * W[k + 1];
    }
    frag = vec4(c, 1.0);
  } else {
    vec3 g = texture(uSrc, vUV).rgb * uP.z;
    // 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): white-hot core: channels past 1 pull toward white.
    float mc = max(g.r, max(g.g, g.b));
    g = mix(g, vec3(mc), clamp((mc - 1.0) * 0.35, 0.0, 0.65));
    frag = vec4(vec3(1.0) - exp(-g), 1.0);
  }
}

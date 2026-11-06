// Litho GUI - Panel fill pass (theme slot "panel-fill").
// Fragment shader for FillRect fills of windows and panels.
//
// Contract (defined by the base layer, see README "Themes (JSON) and
// user shaders"):
//   - The back end draws one quad per fill and runs this shader. Only
//     the fragment stage is theme-supplied.
//   - Uniforms arrive in the std140 block below (set 0, binding 0):
//       uRect         = (x, y, w, h) of the fill, GUI pixels, origin
//                       top-left, y down.
//       uCornerRadius = corner radius carried on the FillRect command
//                       (0 = square corners).
//       uColor        = fill color; its ALPHA ALREADY INCLUDES the
//                       effective opacity (Style::uiOpacity x the
//                       per-window override chain), multiplied in by
//                       the base layer. The shader must not scale it
//                       again; uOpacity below is provided for themes
//                       that want the raw factor for effects.
//       uOpacity      = the effective opacity factor the base applied.
//       uViewport     = (width, height) of the target, for the
//                       gl_FragCoord y-flip into GUI coordinates.
//   - Compilation is the back end's job, e.g.:
//       glslangValidator -V panel-fill.glsl -o panel-fill.spv
//     A failed compile falls back to the built-in flat fill.
//
// Look: a flat fill with SDF-rounded corners (anti-aliased edge), plus
// an optional subtle vertical shade so panels read as surfaces over
// the scene. Themes may replace this file wholesale; the uniform
// contract above is the stable part.

#version 450

layout(set = 0, binding = 0) uniform PanelFill {
    vec4 uRect;         // x, y, w, h
    float uCornerRadius;
    vec4 uColor;        // alpha premultiplied with effective opacity
    float uOpacity;
    vec2 uViewport;     // target size in pixels
};

layout(location = 0) out vec4 outColor;

// Signed distance to a rounded box (negative inside), p centered.
float sdRoundBox(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - (halfSize - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2 frag = vec2(gl_FragCoord.x, uViewport.y - gl_FragCoord.y);
    vec2 p = frag - uRect.xy;
    vec2 halfSize = uRect.zw * 0.5;
    float r = min(uCornerRadius, min(halfSize.x, halfSize.y));
    float sd = sdRoundBox(p - halfSize, halfSize, r);

    // Coverage: 1 well inside, 0 well outside, anti-aliased edge.
    float cov = 1.0 - smoothstep(-0.75, 0.75, sd);
    if (cov <= 0.0) {
        outColor = vec4(0.0);
        return;
    }

    // Subtle top-light: 4% brighter at the top edge of the panel.
    // (uRect is (x, y, w, h), so the height is uRect.w.)
    float shade = 1.0 + 0.04 * (1.0 - clamp(p.y / max(uRect.w, 1.0), 0.0, 1.0));
    outColor = vec4(uColor.rgb * shade, uColor.a * cov);
}

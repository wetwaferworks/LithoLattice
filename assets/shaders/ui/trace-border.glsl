// Litho GUI - Trace border pass (theme slot "trace-border").
// Fragment shader for the CustomDraw tag 1 (CUSTOM_TRACE_BORDER) pass.
//
// Contract (defined by the base layer, see README "The shader-border
// interface"):
//   - The back end draws ONE quad over the border rect and runs this
//     shader. The vertex stage is the back end's standard fullscreen /
//     quad pass; only the fragment stage is theme-supplied.
//   - Uniforms arrive in the std140 block below (set 0, binding 0),
//     filled from the DrawCmd payload:
//       uRect         = (x, y, w, h) of the border rect, GUI pixels,
//                       origin top-left, y down.
//       uCornerRadius = the border's corner radius (SDF input).
//       uBorderWidth  = the trace band width in pixels.
//       uPhase        = caller-supplied animation phase for this frame
//                       (the GUI never reads a clock; the shell advances
//                       phase and hands it in).
//       uTraceA/B/C   = trace colors: A red, B blue, C purple.
//       uOpacity      = trace red's alpha from the command, already
//                       scaled by Style::traceOpacity by the base.
//       uViewport     = (width, height) of the target, for the
//                       gl_FragCoord y-flip into GUI coordinates.
//   - Compilation is the back end's job, e.g.:
//       glslangValidator -V trace-border.glsl -o trace-border.spv
//     A failed compile falls back to the built-in pass / the flat
//     Polyline the base emits immediately before the CustomDraw.
//
// Look (the software preview rasterizer is CANONICAL for this look):
//   - The perimeter coordinate s is arc length along the ROUNDED
//     rectangle, walked clockwise from the start of the top edge
//     (straights shortened by the radius, corner arcs contributing
//     their quarter circumferences), exactly the loop the preview
//     stamps along. A fragment takes the s of its nearest point on
//     that loop.
//   - Base color walks traceB -> traceC -> traceA around the loop
//     (with a hard wrap back to traceB at the loop start; there is
//     no return ramp).
//   - One bright "energy packet": head at fract(uPhase), quadratic
//     falloff tail 16% of the loop long, traveling with increasing s.
//     Its color is the base color lifted by 0.45 and CLAMPED to 1.0
//     per channel, as in the preview.
//   - Brightness ACCUMULATES the preview's stamp stack (round-2
//     review SUP2-3): the preview stamps overlapping discs every
//     ~1.5 px along the loop and ADDS their light, so a pixel near
//     the band receives the sum of every stamp whose falloff
//     reaches it (the old analytic single-stamp evaluation
//     deposited 1.6x-4.2x too little). This shader reproduces the
//     accumulation: for each fragment it sums the stamps centered
//     at arc offsets k * 1.5 px around the fragment's nearest loop
//     point, each with the preview's falloff ramp(R + 0.5 - dist):
//     a wide halo (0.10 at R = 2.6 * width), the base stroke
//     (0.35 at R = 1.1 * width), the packet halo (0.30 * packet at
//     R = 3.4 * width) and the packet core (0.85 * packet at
//     R = 0.9 * width), all scaled by uOpacity. outColor carries
//     the strength-weighted color and the total strength (clamped
//     to [0,1]) as alpha, so color * alpha is the light the preview
//     adds. Alignment is verified numerically against the ACTUAL
//     preview renderer (see FIXES-2026-11-01.md, "Round 2 fix pass").

#version 450

layout(set = 0, binding = 0) uniform TraceBorder {
    vec4 uRect;         // x, y, w, h
    float uCornerRadius;
    float uBorderWidth;
    float uPhase;
    vec4 uTraceA;       // red
    vec4 uTraceB;       // blue
    vec4 uTraceC;       // purple
    float uOpacity;
    vec2 uViewport;     // target size in pixels
};

layout(location = 0) out vec4 outColor;

// Signed distance to a rounded box (negative inside), p centered.
float sdRoundBox(vec2 p, vec2 halfSize, float r) {
    vec2 q = abs(p) - (halfSize - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Perimeter coordinate s in [0,1) for a point near the border: the
// arc length of the point's nearest point on the rounded-rect loop,
// divided by the loop's total length. The loop starts at the top
// edge just past the top-left corner and walks clockwise (y down):
// top straight, TR arc, right straight, BR arc, bottom straight
// (right to left), BL arc, left straight (bottom to top), TL arc.
// Candidates are the 4 straights (projection clamped to the segment,
// so a clamped endpoint agrees with the arc that starts there) and
// the 4 arcs (radial projection onto the corner circle). With
// r == 0 the arcs degenerate to the corners and this reduces to the
// sharp-rectangle walk.
// Total loop length (straights + quarter arcs), the same sum
// perimeterS walks.
float perimeterTotal(vec2 size, float r) {
    float T = max(size.x - 2.0 * r, 0.0);
    float R = max(size.y - 2.0 * r, 0.0);
    float A = 1.57079632679 * r;
    return 2.0 * T + 2.0 * R + 4.0 * A;
}

float perimeterS(vec2 p, vec2 size, float r) {
    float T = max(size.x - 2.0 * r, 0.0);   // top/bottom straight length
    float R = max(size.y - 2.0 * r, 0.0);   // left/right straight length
    float A = 1.57079632679 * r;            // quarter arc length
    float total = perimeterTotal(size, r);
    if (total <= 0.0) return 0.0;

    float bestD2 = 1e30;
    float bestS = 0.0;

    // Straight candidates: q is the nearest point on the segment,
    // s its arc length from the loop start.
    vec2 q = vec2(clamp(p.x, r, size.x - r), 0.0);              // top
    float d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = q.x - r; }
    q = vec2(size.x, clamp(p.y, r, size.y - r));                // right
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = T + A + (q.y - r); }
    q = vec2(clamp(p.x, r, size.x - r), size.y);                // bottom
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = T + A + R + A + (size.x - r - q.x); }
    q = vec2(0.0, clamp(p.y, r, size.y - r));                   // left
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = 2.0 * T + 3.0 * A + R + (size.y - r - q.y); }

    // Arc candidates: radial projection onto each corner circle;
    // the angle is clamped to the arc's span so s stays on the arc.
    vec2 c = vec2(size.x - r, r);                               // TR: -pi/2..0
    vec2 v = p - c;
    float th = clamp(atan(v.y, v.x), -1.57079632679, 0.0);
    q = c + r * vec2(cos(th), sin(th));
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = T + (th + 1.57079632679) * r; }
    c = vec2(size.x - r, size.y - r);                           // BR: 0..pi/2
    v = p - c;
    th = clamp(atan(v.y, v.x), 0.0, 1.57079632679);
    q = c + r * vec2(cos(th), sin(th));
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = T + A + R + th * r; }
    c = vec2(r, size.y - r);                                    // BL: pi/2..pi
    v = p - c;
    th = clamp(atan(v.y, v.x), 1.57079632679, 3.14159265359);
    q = c + r * vec2(cos(th), sin(th));
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = 2.0 * T + 2.0 * A + R + (th - 1.57079632679) * r; }
    c = vec2(r, r);                                             // TL: pi..3pi/2
    v = p - c;
    th = atan(v.y, v.x);
    if (th < 0.0) th += 6.28318530718;
    th = clamp(th, 3.14159265359, 4.71238898038);
    q = c + r * vec2(cos(th), sin(th));
    d2 = dot(p - q, p - q);
    if (d2 < bestD2) { bestD2 = d2; bestS = 2.0 * T + 3.0 * A + 2.0 * R + (th - 3.14159265359) * r; }

    return fract(bestS / total);
}

// Base stroke color around the loop: B -> C over the first half,
// C -> A over the second, then a hard wrap back to B at the loop
// start (no return ramp; same in the preview's mix3).
vec3 mixTrace(float s, vec3 a, vec3 b, vec3 c) {
    if (s < 0.5) {
        return mix(b, c, s * 2.0);
    }
    return mix(c, a, (s - 0.5) * 2.0);
}

// The preview's stamp falloff: full strength out to R - 0.5 px from
// the stamp center, ramping linearly to zero at R + 0.5 px.
float stampRamp(float d, float radius) {
    return clamp(radius + 0.5 - d, 0.0, 1.0);
}

void main() {
    // gl_FragCoord is bottom-left origin; GUI rects are top-left, y down.
    vec2 frag = vec2(gl_FragCoord.x, uViewport.y - gl_FragCoord.y);
    vec2 p = frag - uRect.xy;              // local point in the rect
    vec2 halfSize = uRect.zw * 0.5;
    float r = min(uCornerRadius, min(halfSize.x, halfSize.y));
    float sd = sdRoundBox(p - halfSize, halfSize, r);
    float d = abs(sd);

    float bw = max(uBorderWidth, 1.0);
    // The widest layer is the packet halo (R = 3.4 * bw); past its
    // falloff nothing contributes.
    if (d > 3.4 * bw + 0.5) {
        outColor = vec4(0.0);
        return;
    }

    float s = perimeterS(p, uRect.zw, r);
    float total = perimeterTotal(uRect.zw, r);
    if (total <= 0.0) {
        outColor = vec4(0.0);
        return;
    }

    // Stamp accumulation (review SUP2-3): sum the preview's stamps
    // centered at arc offsets k * 1.5 px around this fragment's
    // nearest loop point. A stamp at arc offset du sits at distance
    // sqrt(d^2 + du^2) from the fragment (exact on straights, a
    // chord approximation on arcs). K covers the widest layer's
    // full reach (3.4 * bw + 0.5); it is capped at 48 stamps each
    // way, which covers border widths up to ~21 px exactly and
    // truncates only the faintest halo tail beyond that.
    float spacing = 1.5;
    int K = int((3.4 * bw + 0.5) / spacing) + 1;
    if (K > 48) K = 48;
    float head = fract(uPhase);
    vec3 light = vec3(0.0);
    float wsum = 0.0;
    for (int k = -K; k <= K; ++k) {
        float du = float(k) * spacing;
        float dist = sqrt(d * d + du * du);
        float sk = fract(s + du / total);
        vec3 baseK = mixTrace(sk, uTraceA.rgb, uTraceB.rgb, uTraceC.rgb);
        // Energy packet at this stamp: head at fract(phase),
        // quadratic tail behind it (16% of the loop).
        float behind = head - sk;
        if (behind < 0.0) behind += 1.0;
        float packet = (behind < 0.16) ? (1.0 - behind / 0.16) : 0.0;
        packet *= packet;
        // Packet color: the base lifted by 0.45, clamped to 1.0
        // per channel (the preview's hot color).
        vec3 hotK = min(vec3(1.0), baseK + 0.45);
        float wBase = 0.10 * stampRamp(dist, 2.6 * bw) +
                      0.35 * stampRamp(dist, 1.1 * bw);
        float wPacket = packet * (0.30 * stampRamp(dist, 3.4 * bw) +
                                  0.85 * stampRamp(dist, 0.9 * bw));
        light += baseK * wBase + hotK * wPacket;
        wsum += wBase + wPacket;
    }
    float alpha = clamp(uOpacity * wsum, 0.0, 1.0);
    if (alpha <= 0.0) {
        outColor = vec4(0.0);
        return;
    }
    // Strength-weighted color: color * alpha is the added light.
    vec3 col = light / max(wsum, 1e-6);
    outColor = vec4(col, alpha);
}

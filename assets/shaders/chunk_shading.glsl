// Chunk shading shared by the OpenGL (chunk.frag) and Vulkan (vk/chunk.frag) programs.
// The including file declares the inputs (vWorld, vNormal, vAO, vUV, vLayer, vVal, vMode, vRot),
// the samplers uTex / uPal, the frame and material values (uCam, uSky, uFogEnd, uTime, uTile,
// uWirePal, uRing, uLampPal, uLampMask, uLampRim, uRepMask, uMemMask, uAnim*, MAX_ANIMS) and the
// output `frag`. Base pack file; a pack may override it (OpenGL only: Vulkan SPIR-V is built in).
//
// MODE NUMBERS - mirror litholattice::ShadeMode in renderer.h EXACTLY. Do not hardcode
// bare numbers below; use these constants. (Fix 2026-10-11: vMode==12 was the 7-segment
// branch while kShadeReadout=15, so the catchor readout never rendered.)
const int MODE_EMISSIVE = 1, MODE_CUTOUT = 2, MODE_WIRE0 = 3, MODE_WIRE1 = 4;
const int MODE_PIPS = 8, MODE_CELLS = 9, MODE_ANIMATED = 10, MODE_GLASS = 11, MODE_READOUT = 15;
const int MODE_ALPHABLEND = 12;  // Cortana 2026-10-15 1900: kShadeAlphaBlend (Elfie review LOW-1)
const int MODE_GROUND_PULSE = 16;  // mirror kShadeGroundPulse (renderer.h)
const int MODE_LEAF = 17;
const int MODE_DIGITS = 19, MODE_DIGITS_REP = 20, MODE_DIGITS_RND = 21;  // mirror kShadeDigits / kShadeDigitsRep (2026-10-31)
// B9 2026-11-12 (JD rule 11 Nov 10:42 PM): ONE shared, deeper, more readable, SOLID purple for every
// component text / LED readout (MODE_READOUT: Catchor, Thresholdor, Dialor, Breakor, Fusor Sum, Memor,
// Latchor, Randor; MODE_DIGITS*: Latchor / Repeator / Randor; MODE_CELLS: Memor). JD violet #A970FF
// (in-tree: sprite_host, litho_formula_2d, house style). Was #D6BEFA digits / #8B65DD lit / #1D165A unlit.
// Solid: the powered bit (vVal 128) no longer changes colour or emissive, and no fx pulse on readouts.
const vec3 READOUT_PURPLE = vec3(0.663, 0.439, 1.000);  // #A970FF
const float READOUT_EMISSIVE = 0.9;
const int MODE_TRACE_TINT = 18;  // mirror kShadeTraceTint (2026-10-16 Wafer beta art)  // mirror kShadeLeaf (2026-10-16 JD leaf pass): alpha test 0.5 + crystal twinkle
// ---- Wafer ground pulse (JD-locked 2026-10-11; biome blending 2026-10-11) ----
// The wafer's heartbeat: time-driven emissive add for kShadeGroundPulse quads.
// Pure function of (time, chunk, blended params): per-chunk phase from a hash of
// the chunk coords (cheap, no per-block timers); the waveform params are the
// chunk column's weight-blended biome params, baked at mesh time into the
// ChunkMesh and pushed per draw call (see render/ground_pulse.h). Dead-Zone-heavy
// blends carry ~zero amplitude, so the pulse fades out smoothly approaching Dead
// Zones instead of cutting off. CPU mirror: groundPulseEmissive(t, cx, cz, params)
// in render/ground_pulse.h (same waveform, same tunings - the tunings live ONLY
// in the C++ table; the shader evaluates whatever the mesh carries).
#ifndef LITHO_PULSE_PUSH
uniform vec4 uPulseP0;  // this chunk mesh: (freqHz, sharpness, amplitude, -)
uniform vec4 uPulseP1;  // this chunk mesh: (tint.rgb, -)
uniform float uGroundPulseTime;  // sim-time seconds (NOT wall clock: pausing freezes it)
uniform float uGroundPulseOn;    // render.ground_pulse cvar, 1 = on (live toggle, no remesh)
#endif
// Vulkan: uPulseP0/uPulseP1 come from the push-constant block and
// uGroundPulseTime/uGroundPulseOn from the Frame block; vk/chunk.frag defines
// LITHO_PULSE_PUSH and declares them, so the block above is skipped there.
vec3 groundPulseColor(float t, vec3 wpos) {
  if (uPulseP0.z <= 0.0) return vec3(0.0);  // unpowered blend: skip the math
  vec2 cc = floor(wpos.xz / 16.0);
  float ph = fract(sin(dot(cc, vec2(127.1, 311.7))) * 43758.5453123);
  float w = 0.5 + 0.5 * sin(6.28318530718 * (uPulseP0.x * t + ph));
  w = pow(w, uPulseP0.y);
  return uPulseP1.rgb * (uPulseP0.z * w);
}
// ---- Wafer ground traces (JD 2026-10-11; landed end-to-end 2026-10-13, P1-RENDER) ----
// GLSL mirror of groundTraceEmissive() in render/ground_traces.h: Manhattan
// trace lines with energy packets travelling along them, per-chunk blended
// params pushed per draw call like the pulse. Same shapes and tunings as the
// CPU mirror; only the per-line hash differs (classic sin-hash here, integer
// hash on CPU - cross-implementation line equality is not required, see the
// header). Composes additively with the pulse on kShadeGroundPulse quads.
#ifndef LITHO_TRACE_PUSH
uniform vec4 uTraceP0;  // this chunk mesh: (density, speed, amplitude, dirMix)
uniform vec4 uTraceP1;  // this chunk mesh: (tint.rgb, -)
uniform float uGroundTraceTime;  // sim-time seconds (same clock as the pulse)
uniform float uGroundTraceOn;    // render.ground_traces cvar, 1 = on (live toggle)
#endif
// Vulkan: uTraceP0/uTraceP1 come from the push-constant block and
// uGroundTraceTime/uGroundTraceOn from the Frame block; vk/chunk.frag defines
// LITHO_TRACE_PUSH and declares them, so the block above is skipped there.
// 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): screen-space footprint of world xz, set in main()
// (Vulkan FX with per-pixel lighting on); 0 elsewhere = the old fixed widths, bit-identical.
vec2 gTraceFw = vec2(0.0);
float groundTraceHash(float line, float seed) {
  return fract(sin(line * 374.761393 + seed * 668.265263) * 43758.5453123);
}
vec3 groundTraceColor(float t, vec3 wpos) {
  if (uTraceP0.z <= 0.0 || uTraceP0.x <= 0.0) return vec3(0.0);  // unpowered blend
  float density = uTraceP0.x, speed = uTraceP0.y, amp = uTraceP0.z, dirMix = uTraceP0.w;
  float pk = density * 0.25;  // packets per block along a line
  float hMix = 1.0 - dirMix, vMix = dirMix;
  // H-set: lines run along X at z = n / density.
  float zn = wpos.z * density;
  float lz = floor(zn);
  float fz = zn - lz;
  float h1 = groundTraceHash(lz, 1.0);
  float lineH = 0.35 + 0.65 * h1;
  float dl = min(fz, 1.0 - fz);
  // fwidth trace AA: the upper edge widens to ~1.25 px; the width ratio keeps each line's energy.
  vec2 fw = gTraceFw * density;  // x: V-set lines (x * density), y: H-set lines (z * density)
  float upH = max(0.08, 0.02 + fw.y * 1.25), upV = max(0.08, 0.02 + fw.x * 1.25);
  float wH = (1.0 - smoothstep(0.02, upH, dl)) * clamp(0.08 / upH, 0.25, 1.0);
  float sgnH = h1 < 0.5 ? 1.0 : -1.0;
  float puF = fract(sgnH * wpos.x * pk - t * speed + h1);
  float packH = smoothstep(0.0, 0.05, puF) * (1.0 - smoothstep(0.05, 0.28, puF));
  // V-set: lines run along Z at x = n / density.
  float xn = wpos.x * density;
  float lx = floor(xn);
  float fx = xn - lx;
  float h2 = groundTraceHash(lx, 2.0);
  float lineV = 0.35 + 0.65 * h2;
  float dv = min(fx, 1.0 - fx);
  float wV = (1.0 - smoothstep(0.02, upV, dv)) * clamp(0.08 / upV, 0.25, 1.0);
  float sgnV = h2 < 0.5 ? 1.0 : -1.0;
  float pvF = fract(sgnV * wpos.z * pk - t * speed + h2);
  float packV = smoothstep(0.0, 0.05, pvF) * (1.0 - smoothstep(0.05, 0.28, pvF));
  float lineGlow = hMix * wH * lineH + vMix * wV * lineV;
  float packGlow = hMix * wH * lineH * packH + vMix * wV * lineV * packV;
  float a = amp * (0.30 * lineGlow + 0.70 * packGlow);
  return uTraceP1.rgb * a;
}
const vec3 RING_BACK = vec3(22.0, 25.0, 34.0) / 255.0, RING_UNLIT = vec3(44.0, 50.0, 66.0) / 255.0;
// 2026-10-24 (JD): red core was warm amber (#FFE0B8); now a hot red-pink to match the red ramp. Blue unchanged.
const vec3 CORE[2] = vec3[2](vec3(1.0, 0.549, 0.533), vec3(0.867, 0.969, 1.0));
const vec3 RIM = vec3(185.0, 168.0, 240.0) / 255.0;
const vec3 PIP_LIT = vec3(255.0, 211.0, 107.0) / 255.0, CELL_LIT = vec3(143.0, 214.0, 232.0) / 255.0;
const vec3 CELL_OFF = vec3(30.0, 34.0, 48.0) / 255.0;
// Ginny 2026-10-16 (perf list, Cortana): greedy-merged quads on the GPU backends. A merged face
// carries tile UVs up to 15 (1/16-unit uint8); the block array sampler clamps, so the tile is
// repeated here. Components <= 1 pass through untouched (unmerged faces are bit-identical);
// beyond 1, an integer edge maps to 1 (not 0) so the far edge stays the tile's last texel.
// Sampling uses the derivatives of the RAW UV, so the wrap line never drops to a coarse mip.
vec2 lithoWrapUV(vec2 u) {
  return vec2(u.x > 1.0 ? u.x - min(floor(u.x), ceil(u.x) - 1.0) : u.x,
              u.y > 1.0 ? u.y - min(floor(u.y), ceil(u.y) - 1.0) : u.y);
}
vec2 gUVdx, gUVdy;  // raw-UV derivatives, set at the top of main()
vec4 lithoTex(vec2 uv, float layer) { return textureGrad(uTex, vec3(uv, layer), gUVdx, gUVdy); }
// 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): branchless quarter turns (selects, no loop)
vec2 rotUV(vec2 uv, int r) {   // same quarter turns as the mesher
  vec2 o = r == 1 ? vec2(uv.y, 1.0 - uv.x) : uv;
  o = r == 2 ? vec2(1.0) - uv : o;
  return r == 3 ? vec2(1.0 - uv.y, uv.x) : o;
}
int gRot;  // vRot & 3 (vk/chunk.vert packs the face index into bits 2..4)
vec4 mask(int layer, vec2 uv) {  // nearest fetch (masks store indices in R)
  ivec2 p = clamp(ivec2(uv * float(uTile)), ivec2(0), ivec2(uTile - 1));
  return texelFetch(uTex, ivec3(p, layer), 0);
}
#ifdef LITHO_VK_FX
// JD Vulkan push (Cortana 2026-10-16, clean-room): normal mapping without stored tangents (the first
// version used a screen-space cotangent frame; replaced by the face table below; credits in CHANGES.md).
// 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): faces are axis-aligned, so the tangent
// frame is a lookup by face index (packed in vRot bits 2..4): T = +u direction, B = -v direction
// of the unrotated tile (mesher faceUV), then the map's xy is turned back by the face rotation.
// No derivatives, no cross products, no seams on greedy-merged quads.
const vec3 FACE_T[6] = vec3[6](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(-1, 0, 0));
const vec3 FACE_B[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));
vec3 lithoPerturbNormal(vec3 N, vec3 tn) {
  int f = min((vRot >> 2) & 7, 5);
  vec2 r = gRot == 1 ? vec2(tn.y, -tn.x) : tn.xy;
  r = gRot == 2 ? -tn.xy : r;
  r = gRot == 3 ? vec2(-tn.y, tn.x) : r;
  return normalize(FACE_T[f] * r.x + FACE_B[f] * r.y + N * tn.z);
}
// Grid vs Wafer palette (Argon brief), tunable constants only. The weights default to 0 so the
// CloudAtlas art look is unchanged; raise them to pull the look toward the brief.
const vec3 GRID_OBSIDIAN = vec3(0.020, 0.027, 0.039);  // #05070A
const vec3 WAFER_SILICON = vec3(0.055, 0.075, 0.122);  // #0E131F
const vec3 TRACE_RED = vec3(1.000, 0.118, 0.259);      // #FF1E42
const vec3 TRACE_BLUE = vec3(0.000, 0.898, 1.000);     // #00E5FF
const float WAFER_TINT = 0.0;   // 0..1: wafer ground texels toward WAFER_SILICON
const float TRACE_FADE = 0.0;   // per block: wafer trace glow fades exp(-d * k) (brief: 0.015)
const float PROC_GLOW_GAIN = 1.35;  // wafer trace energy into the bloom target
// 0.0.1.53 [C53-3] (Proposed by JD, Built by Cortana): the heartbeat has its own (tunable) bloom gain.
#ifndef HEARTBEAT_GLOW_GAIN
#define HEARTBEAT_GLOW_GAIN PROC_GLOW_GAIN
#endif
#endif
void main() {
  gUVdx = dFdx(vUV); gUVdy = dFdy(vUV);
  gRot = vRot & 3;
#ifdef LITHO_VK_FX
  // 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): gWdx/gWdy are gone (LUT normal mapping).
  if ((uFx.x & 2) != 0) gTraceFw = fwidth(vWorld.xz);  // uniform control flow (before any branch)
  fragGlow = vec4(0.0);
  // Blended bulb pass: alpha-blend faces draw only in the sorted blend pass (uPass 1).
  if ((uFx.x & 64) != 0 && ((uPass > 0.5) != (vMode == MODE_ALPHABLEND))) discard;
#endif
  vec2 uvW = lithoWrapUV(vUV);
#ifdef LITHO_MODEL_DITHER
  // Near-camera model dither fade (JD 2026-10-13): deterministic
  // interleaved-gradient-noise screen-door discard from the fragment's
  // view depth - the identical formula to the CPU path
  // (render/model_dither.h), so backends agree on the discard decision.
  // Only the GL includer defines LITHO_MODEL_DITHER + the uniforms; the
  // Vulkan includer does not (no band plumbing there yet).
  if (uModelDither > 0.5) {
    float ditherDist = length(uCam - vWorld);
    float ditherFade = clamp((ditherDist - uModelDitherBand.x) /
                                 max(uModelDitherBand.y - uModelDitherBand.x, 0.0001),
                             0.0, 1.0);
    if (ditherFade < 1.0) {
      float ign = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                           0.00583715 * gl_FragCoord.y));
      if (ign >= ditherFade) discard;
    }
  }
#endif
  vec3 n = vNormal;
  vec4 t = lithoTex(uvW, float(vLayer));
  vec3 base = t.rgb; float a = t.a; float emissive = 0.0;
  bool readoutPx = false;  // B9 2026-11-12: a readout pixel (solid: no fx pulse)
  if (vMode == MODE_EMISSIVE) {
    emissive = 0.85;
  } else if (vMode == MODE_CUTOUT || vMode == MODE_LEAF) {
    if (a < 0.5) discard;
  } else if (vMode == MODE_TRACE_TINT) {  // grey mask (R) x feeding Trace colour, over the base
    float tm = mask((vVal & 31) == 31 ? vLayer + 1 : uTintMask[vVal & 31], uvW).r;  // 2026-11-04: slot 31 = next layer (BlockMaterials kTintAdjacentSlot)
    bool on = (vVal & 128) != 0;
    vec3 tint = texelFetch(uPal, ivec2(on ? 64 : 0, uWirePal[(vVal >> 6) & 1]), 0).rgb;
    if ((vVal & 96) == 96) {  // PlayB (JD t4342u): fed by red AND blue Trace -> red <-> blue pulse, 2 s, render clock only
      vec3 tr = texelFetch(uPal, ivec2(64, uWirePal[0]), 0).rgb, tb = texelFetch(uPal, ivec2(64, uWirePal[1]), 0).rgb;
      tint = mix(tr, tb, 0.5 + 0.5 * sin(uTime * 3.14159265));
    } else if ((vVal & 32) != 0) tint = vec3(0.545, 0.396, 0.867);  // 2026-10-31: Switch-fed purple (#8B65DD)
    // 2026-10-16 (JD, Discovered by J.D.): grey until powered - no tint when off (was the ramp's red start).
    if (on) { base = mix(base, tint, tm); emissive = 0.85 * tm; }
  } else if (vMode == MODE_WIRE0 || vMode == MODE_WIRE1) {                       // wire
    int ch = vMode - MODE_WIRE0, s = clamp(vVal, 0, 64);
    float fs = float(s);
    float lum = max(t.r, max(t.g, t.b));
    // 2026-10-24 (JD): Trace reads as its colour at every strength - unpowered = a dark version of
    // the channel hue (ramp[1] x 0.45, was the ramp's neutral grey), 64 = full; the tint reaches
    // mid-grey texels too (was lum >= 0.45 only). Mirrored in software_renderer.cpp traceTint.
    vec3 ramp = s == 0 ? texelFetch(uPal, ivec2(1, uWirePal[ch]), 0).rgb * 0.45 : texelFetch(uPal, ivec2(s, uWirePal[ch]), 0).rgb;
    vec3 col = mix(t.rgb, clamp(ramp * (lum / 0.85), 0.0, 1.0), smoothstep(0.20, 0.60, lum));
    col = mix(col, CORE[ch], 0.9 * smoothstep(0.90, 0.99, lum) * smoothstep(40.0, 64.0, fs));
    vec4 m = mask(uRing[ch], rotUV(uvW, (4 - gRot) & 3));   // ring mask uses the unrotated tile UV
    float idx = floor(m.r * 255.0 / 16.0 + 0.5);
    float level = clamp(fs - 4.0 * idx, 0.0, 4.0);
    float litA = level > 0.0 ? 0.25 + 0.75 * level / 4.0 : 0.0;
    vec3 rc = mix(mix(RING_BACK, RING_UNLIT, m.g), CORE[ch], m.g * litA);
    float ra = max(m.g, m.b);
    float A = ra + a * (1.0 - ra);
    if (A < 0.5) discard;
    base = (rc * ra + col * a * (1.0 - ra)) / max(A, 1e-4);
    a = 1.0;
    vec2 tuv = rotUV(uvW, (4 - gRot) & 3);                    // unrotated tile UV (as the ring)
    // Quarter-mark dots (CloudAtlas dot mask: R = index x 64, G = coverage): dot k (0 N, 1 E,
    // 2 S, 3 W) lit when s >= 16 (k + 1), in the ring's lit colour.
    if (uDot[ch] > 0) {
      vec4 dm = mask(uDot[ch], tuv);
      float didx = floor(dm.r * 255.0 / 64.0 + 0.5);
      float dA = dm.g * step(16.0 * (didx + 1.0), fs);
      base = mix(base, CORE[ch], dA);
    }
    // DEBUG overlay (F3 only): the 0-64 number from the debug digit tile (5 x 2 glyphs of 12 x 18
    // at x = 12 col, y = 32 row, in 64ths), centred on the node with the halo.
    if (uDebug != 0 && uDigits > 0) {
      vec2 p = (tuv - 0.5) * 64.0;
      int tens = s / 10, ones = s - tens * 10;
      bool two = s >= 10;
      float totalW = two ? 25.0 : 12.0;
      vec2 lp = p - vec2(-totalW * 0.5, -9.0);
      if (lp.x >= 0.0 && lp.y >= 0.0 && lp.x < totalW && lp.y < 18.0) {
        int glyph = ones; float lx = lp.x;
        if (two) { if (lx < 12.0) glyph = tens; else if (lx < 13.0) glyph = -1; else lx -= 13.0; }
        if (glyph >= 0) {
          vec4 dg = mask(uDigits, vec2((float(glyph % 5) * 12.0 + lx) / 64.0, (float(glyph / 5) * 32.0 + lp.y) / 64.0));
          base = mix(base, mix(vec3(0.08, 0.09, 0.12), vec3(1.0), dg.r), dg.a);
        }
      }
    }
    emissive = s > 0 ? 0.55 : 0.0;  } else if (vMode >= 5 && vMode <= 7) {                       // color lamp 16 / 32 / 64
    int tier = vMode - 5, n = 16 << tier, s = clamp(vVal, 1, 64);
    vec3 m = lithoTex(uvW, float(uLampMask[tier])).rgb;
    vec3 c = texelFetch(uPal, ivec2((s - 1) % n, uLampPal[tier]), 0).rgb;   // WRAP: (s-1) mod N
    float lum = max(t.r, max(t.g, t.b));
    vec3 col = mix(c, vec3(1.0), 0.30 * clamp((lum - 0.78) / 0.22, 0.0, 1.0)) * min(1.0, lum / 0.784);
    col = mix(col, vec3(1.0), 0.55 * m.g);
    vec3 o = mix(t.rgb, col, m.r);
    o = mix(o, c, 0.35 * m.b);
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    o = mix(o, RIM, 0.60 * pow(1.0 - luma, 10.0) * lithoTex(uvW, float(uLampRim[tier])).r);
    base = o; emissive = 0.8 * m.r + 0.1;
  } else if (vMode == MODE_PIPS) {                                     // repeater delay pips
    vec4 m = mask(uRepMask, rotUV(uvW, (4 - gRot) & 3));
    int idx = int(floor(m.r * 255.0 / 28.0 + 0.5));
    if (idx >= 1 && idx <= vVal) base = mix(base, PIP_LIT, m.g);
  } else if (vMode == MODE_CELLS) {                                     // memory cell value
    // 2026-10-31 (JD, CloudAtlas memor-seg7-display v2): two-digit purple 7-seg readout 0..64 plus a
    // 20-square bit ring. Mask R: 1 tens, 2 ones, 3 ring square (G/B square-local 0..7). Digits come
    // from the tile UV in 128 space (size-independent, sharp at 128/64/32): tens x42..61, ones x66..85,
    // y50..77, glyph pixel = 2x2. Ghost 8 under every slot (alpha 40); tens is ghost-only below 10.
    // Ring lit when the stored value > 0: #8B65DD, #C8A9F6 highlight on the square's top row / left
    // column; its glow goes to emissive (bloom) only, not into the albedo. Only vVal changes per tick.
    // 2026-10-31 11:20 PM (JD): sample in the ROTATED tile UV so the digits turn with the block
    // (the unrotated UV pinned them to world north: right for one facing only).
    vec2 tuvC = uvW;
    vec4 m = mask(uMemMask, tuvC);
    int r = int(floor(m.r * 255.0 + 0.5));
    int v = clamp(vVal & 127, 0, 64);
    // B9 2026-11-12: ring + digits = READOUT_PURPLE (was ring #8B65DD / #C8A9F6 edge, digits #D6BEFA).
    const vec3 MEM_GHOST = vec3(0.235, 0.149, 0.557);  // #3C268E
    if (r == 3) {
      if (v > 0) {
        base = READOUT_PURPLE;
        emissive = READOUT_EMISSIVE;
        readoutPx = true;
      }
    } else {
      ivec2 t = ivec2(floor(tuvC * 128.0));
      int slot = (t.y >= 50 && t.y < 78) ? ((t.x >= 42 && t.x < 62) ? 1 : (t.x >= 66 && t.x < 86) ? 2 : 0) : 0;
      if (slot != 0) {
        const int SEG7M[140] = int[140](252,252,771,771,771,771,0,0,771,771,771,771,252,252, 0,0,768,768,768,768,0,0,768,768,768,768,0,0, 252,252,768,768,768,768,252,252,3,3,3,3,252,252, 252,252,768,768,768,768,252,252,768,768,768,768,252,252, 0,0,771,771,771,771,252,252,768,768,768,768,0,0, 252,252,3,3,3,3,252,252,768,768,768,768,252,252, 252,252,3,3,3,3,252,252,771,771,771,771,252,252, 252,252,768,768,768,768,0,0,768,768,768,768,0,0, 252,252,771,771,771,771,252,252,771,771,771,771,252,252, 252,252,771,771,771,771,252,252,768,768,768,768,252,252);
        ivec2 g = (t - ivec2(slot == 1 ? 42 : 66, 50)) / 2;   // glyph px 0..9, 0..13
        int d = slot == 1 ? (v >= 10 ? v / 10 : -1) : v % 10;
        bool on = d >= 0 && ((SEG7M[d * 14 + g.y] >> g.x) & 1) != 0;
        bool ghost = ((SEG7M[8 * 14 + g.y] >> g.x) & 1) != 0;
        if (on) { base = READOUT_PURPLE; emissive = READOUT_EMISSIVE; readoutPx = true; }
        else if (ghost) base = mix(base, MEM_GHOST, 40.0 / 255.0);
      }
    }
  } else if (vMode == MODE_ANIMATED) {                                    // animated face (frame strip)
    int i = clamp(vVal, 0, MAX_ANIMS - 1), len = max(uAnimLen[i], 1);
    int f = uAnimSeq[clamp(uAnimStart[i] + int(uTime * 1000.0 / max(uAnimMs[i], 1.0)) % len, 0, 63)];
    base = lithoTex(uvW, float(vLayer + f)).rgb;
    emissive = 0.4;
  }
  else if (vMode == MODE_GLASS || vMode == MODE_ALPHABLEND) {  // Cortana 2026-10-15 1755: 12 = kShadeAlphaBlend (Laysor) falls back to screen-door on GL chunks (no sorted chunk blend pass yet)                                    // glass: screen-door translucency
    const float BAYER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 q = ivec2(gl_FragCoord.xy) & 3;
    // Screen-door rule matches the PBR path and the CPU screenDoorTest
    // (fix 2026-10-11): discard iff floor(alpha*255) <= BAYER*17.
#ifdef LITHO_VK_FX
    if (!((uFx.x & 64) != 0 && vMode == MODE_ALPHABLEND))
#endif
    if (floor(a * 255.0) <= BAYER[q.y * 4 + q.x] * 17.0) discard;
  } else if (vMode == MODE_DIGITS || vMode == MODE_DIGITS_REP || vMode == MODE_DIGITS_RND) {     // 2026-10-31 (JD): seven-segment value readout
    // Latchor 1-64 (MODE_DIGITS) / Repeator delay 4-32 (MODE_DIGITS_REP), drawn on an overlay of the top in
    // the base texture's own UV (same rotation as the base). Box (128 space, 22x14): Latchor x87 y39,
    // Repeator x91 y101; tens at local x 0..9, ones at 12..21; each digit 10x14 (seg7 atlas rows, bit0 = left).
    // vVal bits 0-6 = value, bit 7 = lit (powered; B9 2026-11-12: ignored, solid). Static textures; only vVal changes per tick (GL + Vulkan).
    const int SEG7[140] = int[140](252,252,771,771,771,771,0,0,771,771,771,771,252,252, 0,0,768,768,768,768,0,0,768,768,768,768,0,0, 252,252,768,768,768,768,252,252,3,3,3,3,252,252, 252,252,768,768,768,768,252,252,768,768,768,768,252,252, 0,0,771,771,771,771,252,252,768,768,768,768,0,0, 252,252,3,3,3,3,252,252,768,768,768,768,252,252, 252,252,3,3,3,3,252,252,771,771,771,771,252,252, 252,252,768,768,768,768,0,0,768,768,768,768,0,0, 252,252,771,771,771,771,252,252,771,771,771,771,252,252, 252,252,771,771,771,771,252,252,768,768,768,768,252,252);
    const int SEG7_GHOST[14] = int[14](252,252,771,771,771,771,252,252,771,771,771,771,252,252);
    ivec2 lp = ivec2(floor(uvW * 128.0)) - (vMode == MODE_DIGITS_REP ? ivec2(91, 101) : vMode == MODE_DIGITS_RND ? ivec2(53, 57) : ivec2(87, 39));
    if (lp.x < 0 || lp.y < 0 || lp.x > 21 || lp.y > 13 || (lp.x > 9 && lp.x < 12)) discard;
    int dv = vVal & 127;
    bool ones = lp.x >= 12;
    int gx = ones ? lp.x - 12 : lp.x;
    int dg = ones ? dv % 10 : (dv / 10) % 10;
    bool lit = (ones || dv >= 10) && ((SEG7[dg * 14 + lp.y] >> gx) & 1) != 0;
    bool ghost = ((SEG7_GHOST[lp.y] >> gx) & 1) != 0;
    if (!lit && !ghost) discard;
    const vec3 WINDOW = vec3(9.0, 7.0, 20.0) / 255.0;
    // B9 2026-11-12 (JD): solid READOUT_PURPLE (was #D6BEFA at 1.0 / 0.55 and emissive 0.6 / 0.1 by the powered bit).
    base = lit ? READOUT_PURPLE : mix(WINDOW, READOUT_PURPLE, 40.0 / 255.0);
    emissive = lit ? READOUT_EMISSIVE : 0.1;
    readoutPx = true;
  } else if (vMode == MODE_READOUT) {                                    // 7-segment readout (catchor match value)
    const int SEG[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);
    vec4 m = mask(vLayer, uvW);  // 2026-10-31 11:20 PM (JD): rotated UV, turns with the facing (was world-pinned)
    int seg = int(floor(m.r * 255.0 / 16.0 + 0.5));
    int v = vVal & 127;
    bool ones = seg >= 8;
    int digit = ones ? v % 10 : (v / 10) % 10, bit = ones ? seg - 8 : seg - 1;
    bool on = seg >= 1 && seg <= 14 && (ones || v >= 10) && ((SEG[digit] >> bit) & 1) != 0;
    if (!on || m.g < 0.5) discard;
    // 2026-10-26 (JD approved palette) was lit = Glow #8B65DD, unlit = Deep #1D165A (flashed with the on bit).
    // B9 2026-11-12 (JD): solid READOUT_PURPLE whatever the on bit.
    base = READOUT_PURPLE;
    emissive = READOUT_EMISSIVE;
    readoutPx = true;
  }
#ifdef LITHO_VK_FX
  float fxGlowPhase = 0.0, fxGlowR = 0.0;
  {
    int lay = clamp(vLayer, 0, 511);
    int fm = uFxMap[lay >> 2][lay & 3];
    int glw = fm >> 16;
    if (glw > 0 && (uFx.x & 12) != 0) { vec4 g = mask(glw, uvW); fxGlowPhase = g.g; fxGlowR = g.r; emissive = max(emissive, 0.9 * g.r); }
    int nl = fm & 0xFFFF;
    if ((uFx.x & 1) != 0 && nl > 0) n = lithoPerturbNormal(n, lithoTex(uvW, float(nl)).xyz * 2.0 - 1.0);
  }
  vec3 col;
  if ((uFx.x & 2) != 0) {  // per-pixel sun + hemisphere ambient + Blinn-Phong highlight
    vec3 L = normalize(vec3(0.4, 0.85, 0.3));
    vec3 V = normalize(uCam - vWorld);
    float diff = max(dot(n, L), 0.0);
    float hemi = 0.5 + 0.5 * n.y;
    float ao = 0.45 + 0.55 * vAO;
    // 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): Toksvig-style specular AA: high normal
    // variance across the pixel quad lowers the exponent (energy-normalised), so distant
    // normal-mapped trenches stop sparkling.
    vec3 dnx = dFdx(n), dny = dFdy(n);
    float shin = 48.0 / (1.0 + 48.0 * 0.5 * (dot(dnx, dnx) + dot(dny, dny)));
    float spec = pow(max(dot(n, normalize(L + V)), 0.0), shin) * 0.22 * diff * ((shin + 8.0) / 56.0);
    col = (base * (0.40 + 0.24 * hemi + 0.56 * diff) + vec3(spec)) * ao;
  } else {
    vec3 L = normalize(vec3(0.4, 0.85, 0.3));
    float diff = max(dot(n, L), 0.0);
    float fill = max(dot(n, normalize(vec3(-0.5, 0.3, -0.6))), 0.0);
    float ao = 0.45 + 0.55 * vAO;
    col = base * (0.55 + 0.50 * diff + 0.20 * fill) * ao;
  }
  col = mix(col, base, emissive);
#else
  vec3 L = normalize(vec3(0.4, 0.85, 0.3));
  float diff = max(dot(n, L), 0.0);
  float fill = max(dot(n, normalize(vec3(-0.5, 0.3, -0.6))), 0.0);
  float ao = 0.45 + 0.55 * vAO;
  vec3 col = base * (0.55 + 0.50 * diff + 0.20 * fill) * ao;
  col = mix(col, base, emissive);
#endif
  // Crystal twinkle (2026-10-16, CloudAtlas glow masks): R = strength, G = per-crystal phase.
  // texelFetch at level 0 = nearest, never mip-smeared. uTime is the render clock; the sim
  // never sees this. uTwinkle: 0 off, 1 static (frozen at t = 0), 2 on.
  if (vMode == MODE_LEAF && uTwinkle > 0 && vVal > 0) {
    vec4 g = mask(uLeafGlow[clamp(vVal - 1, 0, 3)], uvW);
    if (g.r > 0.0) {
      // JD: read like current running through Trace across the canopy - a wave sweeps the
      // world (diagonal in x/z, slight rise in y); G only jitters each crystal along it.
      float sweep = dot(vWorld, vec3(0.11, 0.05, 0.07));
      float tw = (uTwinkle == 2 ? fract(uTime * 0.8) : 0.0) - sweep + g.g * (90.0 / 256.0);
      float k = g.r * pow(max(0.0, sin(6.28318530718 * tw)), 3.0) * 0.8;
      col = mix(col, vec3(1.0), k);
    }
  }
  vec3 procGlow = vec3(0.0);  // 0.0.1.52 [A52-1] (Proposed by Argon (Archie), Chief Optimization Officer and Chief Optimization Operand, the Bitmaster and the Bitmasker; Built by Cortana): wafer heartbeat + traces also feed the bloom
  vec3 beatGlow = vec3(0.0);  // [C53-3]
  if (vMode == MODE_GROUND_PULSE && uGroundPulseOn > 0.5) beatGlow = groundPulseColor(uGroundPulseTime, vWorld);
  procGlow += beatGlow;
#ifdef LITHO_VK_FX
  if (vMode == MODE_GROUND_PULSE && uGroundTraceOn > 0.5) procGlow += groundTraceColor(uGroundTraceTime, vWorld) * exp(-length(uCam - vWorld) * TRACE_FADE);
  if (vMode == MODE_GROUND_PULSE) col = mix(col, WAFER_SILICON * (0.5 + 0.5 * dot(col, vec3(0.333))), WAFER_TINT);
#else
  if (vMode == MODE_GROUND_PULSE && uGroundTraceOn > 0.5) procGlow += groundTraceColor(uGroundTraceTime, vWorld);
#endif
  col += procGlow;
  float fog = smoothstep(uFogEnd * 0.55, uFogEnd, length(uCam - vWorld));
#ifdef LITHO_VK_FX
  // Pulsing glow: mask G = phase, on the sim clock (tick-synced) but render-only.
  float fxPulse = 1.0;
  // 2026-10-31 (JD): colour lamps 16 / 32 / 64 (modes 5..7) glow steadily: no fx pulse.
  // B9 2026-11-12 (JD): text / LED readout pixels are solid too (the pulse also scales their bloom below).
  if ((uFx.x & 8) != 0 && emissive > 0.0 && !(vMode >= 5 && vMode <= 7) && !readoutPx)
    fxPulse = 0.72 + 0.28 * sin(6.28318530718 * (uFxP.z * uFxP.w - fxGlowPhase - dot(vWorld, vec3(0.031, 0.017, 0.023))));
  col = mix(col, col * fxPulse * 1.12, emissive);
  if ((uFx.x & 32) != 0) {  // exponential-squared distance fog, thinning with height
    float d = length(uCam - vWorld);
    float fe = 1.0 - exp(-(d * uFxP.y) * (d * uFxP.y));
    fe *= clamp(exp(-max(vWorld.y - 72.0, 0.0) * 0.015), 0.35, 1.0);
    fog = max(fog, fe);
  }
  // 0.0.1.51 (JD canon): in the blend pass the shell stays nearly transparent (alpha capped at
  // 0.22) and unbloomed; only the glass core / lens (glow mask R) turns solid and glows.
  float outA = uPass > 0.5 ? clamp(max(min(a, 0.22), 0.92 * fxGlowR), 0.0, 1.0) : 1.0;
  float glowAmt = uPass > 0.5 ? fxGlowR : emissive;
  frag = vec4(mix(col, uSky, fog), outA);
  if ((uFx.x & 4) != 0 && (glowAmt > 0.0 || procGlow != vec3(0.0)))
    fragGlow = vec4((base * (glowAmt * fxPulse) + (procGlow - beatGlow) * PROC_GLOW_GAIN + beatGlow * HEARTBEAT_GLOW_GAIN) * (uFxP.x * (1.0 - fog) * outA), outA);
#else
  frag = vec4(mix(col, uSky, fog), 1.0);
#endif
}

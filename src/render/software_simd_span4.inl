// Ginny (Argon) 2026-10-16 Pass 2 step 5 (Cortana): 4-wide basic span shading, shared body
// for the SSE2 and SSE4.1 translation units. Included by software_simd_sse2.cpp and
// software_simd_sse41.cpp only, each defining LL_SPAN4_FN (kernel name), LL_SPAN4_TARGET
// (per-function target attribute or empty) and LL_SPAN4_BLEND_PS(a, b, m) (m ? b : a).
// Same op order as the scalar loop and the AVX2 kernel (no FMA, truncating conversions),
// so the output is bit-identical. Texels: 4 scalar L1 loads packed with _mm_set_epi32
// (no gather on these ISAs; the indices are always 0..255 inside the layer).
// Argon point 3 (2026-10-18): AO is interpolated in-kernel (attAo, the scalar loop's
// ((w0*ao0 + w1*ao1) + w2*ao2)) and applied as its post-shader scale
// (uint32_t(float(channel) * a) == cvttps(cvtepi32(channel) * attAo)), so AO tris take
// the SIMD path too. Trap B: full-pass blocks (passBits == 0xF) store depth + colour
// with single wide stores instead of the LZ/C stack round trip + scalar scatter, and
// texel indices are extracted from the register file instead of a stack store/reload.
static const uint8_t kSpan4Pop[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
LL_SPAN4_TARGET void LL_SPAN4_FN(ShadeSpanBasic& s) {
  float e0 = s.sw0, e1 = s.sw1, e2 = s.sw2, ez = s.sdepth;
  const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), k15 = _mm_set1_ps(s.texScale);  // Cortana 2026-10-16: runtime tile
  const __m128 iw0 = _mm_set1_ps(s.iw0), iw1 = _mm_set1_ps(s.iw1), iw2 = _mm_set1_ps(s.iw2);
  __m128 at[3][5];
  for (int v = 0; v < 3; ++v) for (int c = 0; c < 5; ++c) at[v][c] = _mm_set1_ps(s.a[v][c]);
  const __m128 ao0v = _mm_set1_ps(s.ao[0]), ao1v = _mm_set1_ps(s.ao[1]), ao2v = _mm_set1_ps(s.ao[2]);
  const __m128 cx = _mm_set1_ps(0.44f), cy = _mm_set1_ps(0.88f), cz = _mm_set1_ps(0.26f);
  const __m128 amb = _mm_set1_ps(0.35f), dif = _mm_set1_ps(0.65f);
  const __m128i m15 = _mm_set1_epi32((1 << s.texShift) - 1), m255 = _mm_set1_epi32(255), alpha = _mm_set1_epi32(int(0xFF000000u));
  const __m128i m33 = _mm_set1_epi32(0x33), m55 = _mm_set1_epi32(0x55);
  int wxLo = 1 << 30, wxHi = -1;
  uint64_t lc = 0, ls = 0;
  alignas(16) float L0[4], L1[4], L2[4], LZ[4];
  alignas(16) uint16_t DQ[4];  // PT4-A: gathered stored depths (quantized)
  alignas(16) uint32_t C[4];
  for (int base = 0; base < s.n; base += 4) {
    const int cnt = s.n - base < 4 ? s.n - base : 4;
    for (int j = 0; j < 4; ++j) {  // sequential stepping == scalar loop's running sums
      L0[j] = e0; L1[j] = e1; L2[j] = e2; LZ[j] = ez;
      e0 += s.dw0dx; e1 += s.dw1dx; e2 += s.dw2dx; ez += s.dzdx;
    }
    const __m128 w0 = _mm_load_ps(L0), w1 = _mm_load_ps(L1), w2 = _mm_load_ps(L2), z = _mm_load_ps(LZ);
    const __m128 cov4 = _mm_and_ps(_mm_and_ps(_mm_cmpge_ps(w0, zero), _mm_cmpge_ps(w1, zero)), _mm_cmpge_ps(w2, zero));
    const int covBits = _mm_movemask_ps(cov4) & ((1 << cnt) - 1);
    if (!covBits) continue;
    lc += kSpan4Pop[covBits];
    uint16_t* drow = s.depth + base;
    for (int j = 0; j < 4; ++j) DQ[j] = ((covBits >> j) & 1) ? drow[j] : uint16_t(0);
    // PT4-A: widen the stored depths, quantize the fragment depths with
    // the shared rule (cvtps == nearbyintf), test in the integer domain.
    const __m128i dstored = _mm_unpacklo_epi16(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(DQ)), _mm_setzero_si128());
    __m128 zc4 = _mm_min_ps(_mm_max_ps(z, zero), one);
    if (swsimd::g_depthRemapMode == 1) zc4 = _mm_sub_ps(one, _mm_sqrt_ps(_mm_sub_ps(one, zc4)));  // LL_SW_DEPTH_REMAP
    else if (swsimd::g_depthRemapMode == 2) {  // item 7 reciprocal depth (= swsimd::depthRecip)
      const __m128 u = _mm_max_ps(_mm_sub_ps(one, zc4), _mm_set1_ps(3.0517578125e-05f));
      zc4 = _mm_mul_ps(_mm_cvtepi32_ps(_mm_sub_epi32(_mm_set1_epi32(0x3F800000), _mm_castps_si128(u))), _mm_set1_ps(1.0f / (15.0f * 8388608.0f)));
    }
    const __m128i zq = _mm_cvtps_epi32(_mm_mul_ps(zc4, _mm_set1_ps(65535.0f)));
    const int passBits = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(dstored, zq))) & covBits;
    if (!passBits) continue;
    ls += kSpan4Pop[passBits];
    const __m128 pw0 = _mm_mul_ps(w0, iw0), pw1 = _mm_mul_ps(w1, iw1), pw2 = _mm_mul_ps(w2, iw2);
    const __m128 vd = _mm_div_ps(one, _mm_add_ps(_mm_add_ps(pw0, pw1), pw2));
    const __m128 q0 = _mm_mul_ps(pw0, vd), q1 = _mm_mul_ps(pw1, vd), q2 = _mm_mul_ps(pw2, vd);
    __m128 att[5];
    for (int c = 0; c < 5; ++c)
      att[c] = _mm_add_ps(_mm_add_ps(_mm_mul_ps(q0, at[0][c]), _mm_mul_ps(q1, at[1][c])), _mm_mul_ps(q2, at[2][c]));
    // Argon point 3: perspective AO with the scalar loop's association order.
    const __m128 attAo = s.hasAo ? _mm_add_ps(_mm_add_ps(_mm_mul_ps(q0, ao0v), _mm_mul_ps(q1, ao1v)),
                                              _mm_mul_ps(q2, ao2v))
                                 : zero;
    // swTile: t > 1 ? t - float(int(t)) : t
    __m128 tu = att[3], tv = att[4];
    tu = LL_SPAN4_BLEND_PS(tu, _mm_sub_ps(tu, _mm_cvtepi32_ps(_mm_cvttps_epi32(tu))), _mm_cmpgt_ps(tu, one));
    tv = LL_SPAN4_BLEND_PS(tv, _mm_sub_ps(tv, _mm_cvtepi32_ps(_mm_cvttps_epi32(tv))), _mm_cmpgt_ps(tv, one));
    const __m128i tx = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(tu, k15)), m15);
    const __m128i ty = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(tv, k15)), m15);
    __m128i ti;
    if (s.morton) {  // Pass 2 step 4: spread(tx) | spread(ty) << 1
      __m128i a = _mm_and_si128(_mm_or_si128(tx, _mm_slli_epi32(tx, 2)), m33);
      a = _mm_and_si128(_mm_or_si128(a, _mm_slli_epi32(a, 1)), m55);
      __m128i b = _mm_and_si128(_mm_or_si128(ty, _mm_slli_epi32(ty, 2)), m33);
      b = _mm_and_si128(_mm_or_si128(b, _mm_slli_epi32(b, 1)), m55);
      ti = _mm_or_si128(a, _mm_slli_epi32(b, 1));
    } else {
      ti = _mm_add_epi32(_mm_sll_epi32(ty, _mm_cvtsi32_si128(s.texShift)), tx);
    }
    // Argon point 3 (trap B): texel indices extracted from the register file —
    // the old _mm_store_si128 to a stack array + 4 scalar reloads paid a
    // store-forwarding stall on every block. Same integer indices, same loads.
    const __m128i texel = _mm_set_epi32(int(s.tex[_mm_cvtsi128_si32(_mm_shuffle_epi32(ti, 3))]),
                                        int(s.tex[_mm_cvtsi128_si32(_mm_shuffle_epi32(ti, 2))]),
                                        int(s.tex[_mm_cvtsi128_si32(_mm_shuffle_epi32(ti, 1))]),
                                        int(s.tex[_mm_cvtsi128_si32(ti)]));
    __m128 nl = _mm_add_ps(_mm_add_ps(_mm_mul_ps(att[0], cx), _mm_mul_ps(att[1], cy)), _mm_mul_ps(att[2], cz));
    nl = _mm_andnot_ps(_mm_cmplt_ps(nl, zero), nl);  // if (nl < 0) nl = 0
    const __m128 light = _mm_add_ps(amb, _mm_mul_ps(dif, nl));
    __m128i r = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(texel, 16), m255)), light)), m255);
    __m128i g = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(texel, 8), m255)), light)), m255);
    __m128i bch = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(_mm_and_si128(texel, m255)), light)), m255);
    if (s.hasAo) {  // scalar loop's post-shader AO scale, op for op
      r = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(r), attAo)), m255);
      g = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(g), attAo)), m255);
      bch = _mm_and_si128(_mm_cvttps_epi32(_mm_mul_ps(_mm_cvtepi32_ps(bch), attAo)), m255);
    }
    const __m128i color = _mm_or_si128(_mm_or_si128(alpha, _mm_slli_epi32(r, 16)), _mm_or_si128(_mm_slli_epi32(g, 8), bch));
    if (passBits == 0xF) {  // Argon point 3 (trap B): every lane passed (implies
      {  // PT4-A: 4x u16 store of the quantized depths. Scalar extract:
         // _mm_packus_epi32 (PACKUSDW) is SSE4.1-only and this body is
         // shared with the SSE2 TU; the colour store below stays wide.
        alignas(16) int32_t zqArr[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(zqArr), zq);
        for (int j = 0; j < 4; ++j) drow[j] = uint16_t(zqArr[j]);
      }
      _mm_storeu_si128(reinterpret_cast<__m128i*>(s.pixels + base), color);  // trip, no scalar scatter
      if (base < wxLo) wxLo = base;
      wxHi = base + 3;
    } else {
      _mm_store_si128(reinterpret_cast<__m128i*>(C), color);
      _mm_store_ps(LZ, z);
      for (int j = 0; j < 4; ++j)
        if ((passBits >> j) & 1) { drow[j] = depthQ(LZ[j]); s.pixels[base + j] = C[j]; }
      int lo = 0; while (!((passBits >> lo) & 1)) ++lo;
      int hi = 3; while (!((passBits >> hi) & 1)) --hi;
      if (base + lo < wxLo) wxLo = base + lo;
      wxHi = base + hi;
    }
  }
  s.wxLo = wxHi >= 0 ? wxLo : 0; s.wxHi = wxHi; s.lc = lc; s.ls = ls;
}

// 4-wide quad vertex transform (one greedy quad = 4 lanes, zero wasted lanes). Same op order
// as the scalar transformVertex: ((m0*x + m4*y) + m8*z) + m12, |w| < 1e-6 -> copysign,
// one IEEE divide for 1/w, viewport in the same order: bit-identical screen positions.
LL_SPAN4_TARGET void LL_QUAD4_FN(const uint16_t* xyz, size_t strideU16, const float* m, float* out, float W, float H) {
  const __m128 x = _mm_div_ps(_mm_set_ps(float(xyz[3 * strideU16]), float(xyz[2 * strideU16]), float(xyz[strideU16]), float(xyz[0])), _mm_set1_ps(16.0f));
  const __m128 y = _mm_div_ps(_mm_set_ps(float(xyz[3 * strideU16 + 1]), float(xyz[2 * strideU16 + 1]), float(xyz[strideU16 + 1]), float(xyz[1])), _mm_set1_ps(16.0f));
  const __m128 z = _mm_div_ps(_mm_set_ps(float(xyz[3 * strideU16 + 2]), float(xyz[2 * strideU16 + 2]), float(xyz[strideU16 + 2]), float(xyz[2])), _mm_set1_ps(16.0f));
  auto row = [&](int r) LL_SPAN4_LAMBDA_TARGET {
    return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(m[r]), x), _mm_mul_ps(_mm_set1_ps(m[4 + r]), y)),
                                 _mm_mul_ps(_mm_set1_ps(m[8 + r]), z)), _mm_set1_ps(m[12 + r]));
  };
  const __m128 cx = row(0), cy = row(1), cz = row(2);
  __m128 cw = row(3);
  const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)));
  const __m128 tiny = _mm_set1_ps(1e-6f);
  const __m128 small = _mm_cmplt_ps(_mm_andnot_ps(sign, cw), tiny);
  cw = LL_SPAN4_BLEND_PS(cw, _mm_or_ps(_mm_and_ps(cw, sign), tiny), small);
  const __m128 invW = _mm_div_ps(_mm_set1_ps(1.0f), cw);
  const __m128 half = _mm_set1_ps(0.5f);
  __m128 o0 = _mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_mul_ps(cx, invW), half), half), _mm_set1_ps(W));
  __m128 o1 = _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(1.0f), _mm_add_ps(_mm_mul_ps(_mm_mul_ps(cy, invW), half), half)), _mm_set1_ps(H));
  __m128 o2 = _mm_mul_ps(cz, invW);
  __m128 o3 = cw;
  _MM_TRANSPOSE4_PS(o0, o1, o2, o3);
  _mm_storeu_ps(out, o0); _mm_storeu_ps(out + 4, o1); _mm_storeu_ps(out + 8, o2); _mm_storeu_ps(out + 12, o3);
}

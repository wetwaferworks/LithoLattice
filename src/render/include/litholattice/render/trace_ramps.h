#pragma once
// Cortana 2026-10-15 1345 (GAME art data, engine default): Trace power ramps,
// CloudAtlas trace_power_2026-10-15 r3 (trace_ramps.csv). Index = strength
// 0..64; index 0 is the r3 OFF tint (near-neutral gray: red #5A5556, blue
// #535659); 64 is exactly #FF514B / #2FB4FF. 0xRRGGBB, sRGB.
#include <cstdint>
namespace litholattice {
inline constexpr uint32_t kRedTraceRamp[65] = {0x5A5556u, 0x80201Eu, 0x81211Eu, 0x83211Fu, 0x852220u, 0x872320u, 0x892421u, 0x8B2422u, 0x8D2522u, 0x8F2623u, 0x912624u, 0x932724u, 0x952825u, 0x972926u, 0x992926u, 0x9A2A27u, 0x9C2B28u, 0x9E2C29u, 0xA02C29u, 0xA22D2Au, 0xA42E2Bu, 0xA62F2Bu, 0xA8302Cu, 0xAA302Du, 0xAC312Du, 0xAE322Eu, 0xB0332Fu, 0xB23330u, 0xB43430u, 0xB63531u, 0xB83632u, 0xBA3632u, 0xBC3733u, 0xBE3834u, 0xC03935u, 0xC23A35u, 0xC43A36u, 0xC63B37u, 0xC83C38u, 0xCB3D38u, 0xCD3E39u, 0xCF3E3Au, 0xD13F3Au, 0xD3403Bu, 0xD5413Cu, 0xD7423Du, 0xD9423Du, 0xDB433Eu, 0xDD443Fu, 0xDF4540u, 0xE14640u, 0xE34641u, 0xE64742u, 0xE84843u, 0xEA4943u, 0xEC4A44u, 0xEE4A45u, 0xF04B46u, 0xF24C46u, 0xF44D47u, 0xF64E48u, 0xF94F49u, 0xFB4F49u, 0xFD504Au, 0xFF514Bu};
inline constexpr uint32_t kBlueTraceRamp[65] = {0x535659u, 0x034D73u, 0x044E75u, 0x055077u, 0x055179u, 0x00537Cu, 0x01547Eu, 0x015680u, 0x025882u, 0x035984u, 0x045B86u, 0x055C88u, 0x065E8Au, 0x075F8Cu, 0x08618Eu, 0x096290u, 0x006494u, 0x016696u, 0x026798u, 0x03699Bu, 0x046A9Du, 0x056C9Fu, 0x066EA1u, 0x076FA3u, 0x0871A5u, 0x0972A7u, 0x0B74AAu, 0x0C76ACu, 0x0D77AEu, 0x0E79B0u, 0x0F7AB2u, 0x107CB4u, 0x117EB7u, 0x127FB9u, 0x1381BBu, 0x1483BDu, 0x1584BFu, 0x1686C2u, 0x1788C4u, 0x1889C6u, 0x198BC8u, 0x1A8DCAu, 0x1B8ECDu, 0x1C90CFu, 0x1D92D1u, 0x1E93D3u, 0x1F95D6u, 0x2097D8u, 0x2198DAu, 0x219ADCu, 0x229CDFu, 0x239EE1u, 0x249FE3u, 0x25A1E6u, 0x26A3E8u, 0x27A4EAu, 0x28A6ECu, 0x29A8EFu, 0x2AAAF1u, 0x2BABF3u, 0x2BADF6u, 0x2CAFF8u, 0x2DB1FAu, 0x2EB2FDu, 0x2FB4FFu};
/// Cortana 2026-10-15 1600 (MIXED; credit: JD idea): pulse colour from the
/// red/blue Trace mix. With no Trace it returns `biome` unchanged. Otherwise
/// it gives the count-weighted mean of the ramp peaks (#FF514B red, #2FB4FF
/// blue), rescaled so the peak channel matches the biome colour's peak.
/// Pure and deterministic. The GLSL/VK backends get it for free through pulseP1.
inline void traceMixPulseColor(uint32_t nRed, uint32_t nBlue, const float biome[3], float out[4]) {
  out[0] = biome[0]; out[1] = biome[1]; out[2] = biome[2];
  const uint32_t n = nRed + nBlue;
  if (n == 0) return;
  auto ch = [](uint32_t c, int s) { return float((c >> s) & 0xFFu) / 255.f; };
  const uint32_t R = kRedTraceRamp[64], B = kBlueTraceRamp[64];
  const float wr = float(nRed) / float(n), wb = 1.f - wr;
  float m[3] = {wr * ch(R, 16) + wb * ch(B, 16), wr * ch(R, 8) + wb * ch(B, 8), wr * ch(R, 0) + wb * ch(B, 0)};
  float peakB = biome[0] > biome[1] ? biome[0] : biome[1]; peakB = peakB > biome[2] ? peakB : biome[2];
  float peakM = m[0] > m[1] ? m[0] : m[1]; peakM = peakM > m[2] ? peakM : m[2];
  const float k = (peakB > 0.f && peakM > 0.f) ? peakB / peakM : 1.f;
  out[0] = m[0] * k; out[1] = m[1] * k; out[2] = m[2] * k;
}
}  // namespace litholattice
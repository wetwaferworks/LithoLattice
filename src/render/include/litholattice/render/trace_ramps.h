#pragma once
// Cortana 2026-10-15 1345 (GAME art data, engine default): Trace power ramps,
// CloudAtlas trace_power_2026-10-15 r3 (trace_ramps.csv). Index = strength
// 0..64; index 0 is the r3 OFF tint (near-neutral gray: red #5A5556, blue
// #535659); 64 is exactly #FF514B / #2FB4FF. 0xRRGGBB, sRGB.
#include <cstdint>
namespace litholattice {
// 2026-10-24 (JD: red read as orange): green / blue scaled 0.35 / 0.4 for a clear red; blue ramp unchanged.
inline constexpr uint32_t kRedTraceRamp[65] = {0x5A5556u, 0x800B0Cu, 0x810C0Cu, 0x830C0Cu, 0x850C0Du, 0x870C0Du, 0x890D0Du, 0x8B0D0Eu, 0x8D0D0Eu, 0x8F0D0Eu, 0x910D0Eu, 0x930E0Eu, 0x950E0Fu, 0x970E0Fu, 0x990E0Fu, 0x9A0F10u, 0x9C0F10u, 0x9E0F10u, 0xA00F10u, 0xA21011u, 0xA41011u, 0xA61011u, 0xA81112u, 0xAA1112u, 0xAC1112u, 0xAE1212u, 0xB01213u, 0xB21213u, 0xB41213u, 0xB61314u, 0xB81314u, 0xBA1314u, 0xBC1314u, 0xBE1415u, 0xC01415u, 0xC21415u, 0xC41416u, 0xC61516u, 0xC81516u, 0xCB1516u, 0xCD1617u, 0xCF1617u, 0xD11617u, 0xD31618u, 0xD51718u, 0xD71718u, 0xD91718u, 0xDB1719u, 0xDD1819u, 0xDF181Au, 0xE1181Au, 0xE3181Au, 0xE6191Au, 0xE8191Bu, 0xEA1A1Bu, 0xEC1A1Bu, 0xEE1A1Cu, 0xF01A1Cu, 0xF21B1Cu, 0xF41B1Cu, 0xF61B1Du, 0xF91C1Du, 0xFB1C1Du, 0xFD1C1Eu, 0xFF1C1Eu};
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
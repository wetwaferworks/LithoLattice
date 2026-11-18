// audio_output_web_stub.cpp ... WEB-ONLY FAIL-SAFE STUB (S1, 2026-11-16).
// miniaudio is out of the LithoRoot web build (JD 12:41 ET: "replace miniaudio with our own
// AudioWorklet output"). Until that stage lands, openAudioOutput reports "no device" and the game
// runs muted, exactly as on a machine with no sound card (main.cpp's existing fallback).
// Replace this file with the AudioWorklet output (plan section 4.6); do not grow it.
#include "litholattice/audio/audio_output.h"

namespace litholattice {

std::unique_ptr<IAudioOutput> openAudioOutput(cAudioEngine&, std::string* err) {
  if (err) *err = "web audio output not built yet (in-house AudioWorklet stage pending)";
  return nullptr;
}

}  // namespace litholattice

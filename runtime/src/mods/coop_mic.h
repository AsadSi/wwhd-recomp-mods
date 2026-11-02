// The microphone for online co-op's voice chat (coop_voice.h): 16 kHz mono PCM16. AVAudioEngine in the Mac build
// (coop_mic.mm), SDL3's recording stream in the others (coop_mic_sdl.cpp). Opened only while voice chat is on, so the
// system asks for the microphone then and not before.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace mods::coop::mic {

constexpr int kRate = 16000;
bool open(std::string& why);  // (again: nothing)
void close();
size_t take(int16_t* out, size_t max);  // what has been recorded since, oldest first

}  // namespace mods::coop::mic

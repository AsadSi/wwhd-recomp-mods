// Online co-op's proximity voice chat (docs/multiplayer.md, Asad's decision: built-in proximity voice). While it is on,
// this game's microphone (coop_mic.h) is sent in 20 ms frames whenever someone speaks (a level gate with 300 ms of
// hold), each frame IMA ADPCM on its own (4 bits a sample at 16 kHz: 64 kbit/s while talking, nothing in silence), with
// the unreliable states (a lost packet is 50 ms of silence, never a delay). The others play it at that player's
// puppet, through the game's own output (sfx.h streams): louder near, quiet far, panned from the camera.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mods::coop::voice {

// every step while a session runs: `on` the setting; `live` this game plays in the host's world (sync::arrived())
void step(bool on, bool live);
// another player's frames (the kVoice state); `at` their puppet's place here, null when it is not shown here. Out: the
// frames played
int received(int from, const uint8_t* d, size_t n, const float* at);
const char* status();  // for the menu: "Voice chat: on", "Microphone: not allowed (...)"

}  // namespace mods::coop::voice

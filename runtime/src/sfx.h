// Host-side sound effects mixed into the game's output (hle/ax.cpp, after its final mix): clips from WAV files, played
// at a place in the world and panned/attenuated from the camera. Used for sounds the game has none of (mods: the
// Shadow stalker's Twilight Princess sounds, mods/shadow_beast.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace sfx {

int load(const std::string& wav_path);  // a clip id (-1: missing or unreadable); mono or stereo PCM16, any rate
// starts clip `id` at world (x, y, z) (follow: update it with move()), for at most `seconds` of it (0: all); returns
// a voice handle (0: none)
uint32_t play(int id, float x, float y, float z, float volume = 1, bool loop = false, float seconds = 0);
void move(uint32_t voice, float x, float y, float z);
void stop(uint32_t voice);
bool playing(uint32_t voice);
void listener(const float eye[3], const float right[3]);  // the camera: where it is and its right-hand axis
void mix(int32_t* left, int32_t* right, int frames);      // AX frame thread: adds the voices at 48 kHz

}  // namespace sfx

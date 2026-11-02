// The microphone, SDL host builds (coop_mic.h): SDL3's default recording device as a 16 kHz mono PCM16 stream (SDL
// converts). The Mac build's is coop_mic.mm.
#include "coop_mic.h"

#if !(defined(__APPLE__) && !defined(WWHD_SDL_HOST))
#include <SDL3/SDL.h>

namespace mods::coop::mic {
namespace {
SDL_AudioStream* g_stream = nullptr;
}

bool open(std::string& why) {
    if (g_stream) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { why = SDL_GetError(); return false; }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, kRate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (!g_stream) {
        why = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

void close() {
    if (!g_stream) return;
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

size_t take(int16_t* out, size_t max) {
    if (!g_stream) return 0;
    const int got = SDL_GetAudioStreamData(g_stream, out, (int)(max * sizeof(int16_t)));
    return got > 0 ? (size_t)got / sizeof(int16_t) : 0;
}

}  // namespace mods::coop::mic
#endif

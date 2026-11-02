// Online co-op's proximity voice chat (coop_voice.h).
//
// A frame is 320 samples (20 ms at 16 kHz): {s16 first sample, u8 step index} and 160 bytes of 4-bit IMA ADPCM codes
// (the standard tables), two to a byte, low nibble first. Each frame starts its own predictor from its header, so a
// lost one costs only itself.
#include "coop_voice.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "coop_mic.h"
#include "coop_net.h"
#include "coop_sync.h"
#include "mods.h"
#include "../sfx.h"

namespace mods::coop::voice {
namespace {

constexpr int kFrame = 320, kFrameBytes = 3 + kFrame / 2;
constexpr int kGateHold = 15;          // frames the gate stays open after the last loud one (300 ms)
constexpr double kGateLevel = 600;     // RMS of a frame (of 32767) that opens it

const int16_t kSteps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
const int8_t kIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// one ADPCM step: the code for `want` (encoding) or the sample for `code` (decoding), the predictor and index moved on
int adpcm(int& pred, int& index, int want, bool encode, int code = 0) {
    const int step = kSteps[index];
    if (encode) {
        int diff = want - pred;
        code = diff < 0 ? 8 : 0;
        diff = std::abs(diff);
        for (int bit = 4, s = step; bit; bit >>= 1, s >>= 1)
            if (diff >= s) code |= bit, diff -= s;
    }
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    pred = std::clamp(pred + (code & 8 ? -delta : delta), -32768, 32767);
    index = std::clamp(index + kIndex[code & 7], 0, 88);
    return encode ? code : pred;
}

std::string encode(const int16_t* pcm) {
    std::string f(kFrameBytes, '\0');
    int pred = pcm[0], index = 0;
    // the first index from the frame's loudness, so a loud start is not clipped while the steps catch up
    double rms = 0;
    for (int i = 0; i < kFrame; i++) rms += (double)pcm[i] * pcm[i];
    rms = std::sqrt(rms / kFrame);
    while (index < 60 && kSteps[index] < rms / 4) index++;
    memcpy(&f[0], &pcm[0], 2), f[2] = (char)index;
    for (int i = 0; i < kFrame; i++) {
        const int code = adpcm(pred, index, pcm[i], true);
        f[3 + i / 2] = (char)((uint8_t)f[3 + i / 2] | (i & 1 ? code << 4 : code));
    }
    return f;
}

void decode(const uint8_t* f, int16_t* pcm) {
    int16_t first;
    memcpy(&first, f, 2);
    int pred = first, index = std::min<int>(f[2], 88);
    for (int i = 0; i < kFrame; i++) pcm[i] = (int16_t)adpcm(pred, index, 0, false, i & 1 ? f[3 + i / 2] >> 4 : f[3 + i / 2] & 15);
}

bool g_open = false;
int g_hold = 0;
std::string g_status = "Voice chat: off";
int16_t g_rec[kFrame];
int g_rec_n = 0;

}  // namespace

void step(bool on, bool live) {
    static std::string failed;  // (not tried again while the setting stays on)
    if (!on || net::role() == net::Role::kOff) {
        if (g_open) mic::close(), g_open = false;
        failed.clear(), g_status = "Voice chat: off";
        return;
    }
    // a frame recorded: through the gate, out while someone speaks
    const auto frame = [&] {
        double rms = 0;
        for (const int16_t s : g_rec) rms += (double)s * s;
        g_hold = std::sqrt(rms / kFrame) > kGateLevel ? kGateHold : std::max(0, g_hold - 1);
        if (g_hold && live) net::add_state(sync::kVoice, encode(g_rec));
    };
    // test aid: WWHD_COOP_VOICE_TONE=1, a tone in place of the microphone (440 Hz, a second in every three)
    if (static const bool tone = getenv("WWHD_COOP_VOICE_TONE"); tone) {
        static const uint32_t t0 = net::now_ms();
        static uint64_t made = 0;
        for (const uint64_t due = (uint64_t)(net::now_ms() - t0) * mic::kRate / 1000; made + kFrame <= due; made += kFrame) {
            for (int i = 0; i < kFrame; i++)
                g_rec[i] = (made + i) / mic::kRate % 3 ? 0 : (int16_t)(8000 * std::sin(2 * M_PI * 440 * (double)(made + i) / mic::kRate));
            frame();
        }
        g_status = "Voice chat: a test tone";
        return;
    }
    if (!g_open && failed.empty()) {
        std::string why;
        if (mic::open(why)) g_open = true, g_status = "Voice chat: on", trace("coop: voice chat on");
        else failed = why, g_status = "Microphone: " + why, trace("coop: voice chat: %s", why.c_str());
    }
    if (!g_open) return;
    int16_t got[kFrame];
    for (size_t n; (n = mic::take(got, kFrame - g_rec_n)) > 0;) {
        memcpy(g_rec + g_rec_n, got, n * sizeof(int16_t));
        if ((g_rec_n += (int)n) < kFrame) continue;
        g_rec_n = 0;
        frame();
    }
}

int received(int from, const uint8_t* d, size_t n, const float* at) {
    if (!at) return 0;
    int16_t pcm[kFrame];
    int played = 0;
    for (size_t o = 0; o + kFrameBytes <= n; o += kFrameBytes, played++) {
        decode(d + o, pcm);
        sfx::stream(from, pcm, kFrame, mic::kRate, at[0], at[1] + 80, at[2]);  // (at his head)
    }
    return played;
}

const char* status() { return g_status.c_str(); }

}  // namespace mods::coop::voice

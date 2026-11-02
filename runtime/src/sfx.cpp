// Host-side sound effects (sfx.h).
#include "sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace sfx {
namespace {

struct Clip { std::vector<float> mono; int rate = 48000; };
struct Voice { uint32_t handle; int clip; double pos, end; float x, y, z, volume; bool loop; };  // end: last sample

std::mutex g_mu;
std::vector<Clip> g_clips;
std::vector<Voice> g_voices;
uint32_t g_next = 1;
struct Stream { std::deque<float> q; int rate = 16000; double pos = 0; bool playing = false; float x = 0, y = 0, z = 0; };
std::map<int, Stream> g_streams;
float g_eye[3] = {}, g_right[3] = {1, 0, 0};
constexpr float kNear = 600, kFar = 5000;  // full volume within kNear, silent beyond kFar
// a cut-short clip fades out over its last 10 ms (no click)
float fade(const Voice& v, const Clip& c) { const double left = v.end - v.pos, n = c.rate * 0.01; return left < n ? (float)(left / n) : 1.f; }

}  // namespace

int load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return -1;
    int channels = 0, rate = 0, bits = 0;
    for (size_t o = 12; o + 8 <= d.size();) {
        uint32_t size;
        memcpy(&size, d.data() + o + 4, 4);
        if (!memcmp(d.data() + o, "fmt ", 4) && size >= 16) {
            uint16_t ch, bps;
            memcpy(&ch, d.data() + o + 10, 2), memcpy(&rate, d.data() + o + 12, 4), memcpy(&bps, d.data() + o + 22, 2);
            channels = ch, bits = bps;
        } else if (!memcmp(d.data() + o, "data", 4) && channels > 0 && bits == 16) {
            Clip c;
            c.rate = rate;
            const size_t frames = std::min<size_t>(size, d.size() - o - 8) / (2 * channels);
            c.mono.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                float s = 0;
                for (int k = 0; k < channels; k++) {
                    int16_t v;
                    memcpy(&v, d.data() + o + 8 + (i * channels + k) * 2, 2);
                    s += v;
                }
                c.mono[i] = s / channels;
            }
            std::lock_guard<std::mutex> lk(g_mu);
            g_clips.push_back(std::move(c));
            return (int)g_clips.size() - 1;
        }
        o += 8 + size + (size & 1);
    }
    return -1;
}

uint32_t play(int id, float x, float y, float z, float volume, bool loop, float seconds) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (id < 0 || id >= (int)g_clips.size()) return 0;
    const double n = (double)g_clips[id].mono.size() - 1;
    g_voices.push_back({g_next, id, 0, seconds > 0 ? std::min(n, (double)seconds * g_clips[id].rate) : n, x, y, z, volume, loop});
    return g_next++;
}

void move(uint32_t h, float x, float y, float z) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& v : g_voices)
        if (v.handle == h) v.x = x, v.y = y, v.z = z;
}

void stop(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::erase_if(g_voices, [h](const Voice& v) { return v.handle == h; });
}

bool playing(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    return std::any_of(g_voices.begin(), g_voices.end(), [h](const Voice& v) { return v.handle == h; });
}

void stream(int key, const int16_t* pcm, int n, int rate, float x, float y, float z) {
    std::lock_guard<std::mutex> lk(g_mu);
    Stream& st = g_streams[key];
    st.rate = rate, st.x = x, st.y = y, st.z = z;
    for (int i = 0; i < n; i++) st.q.push_back(pcm[i]);
    while (st.q.size() > (size_t)rate * 2 / 5) st.q.pop_front();
    if (!st.playing && st.q.size() >= (size_t)rate * 3 / 50) st.playing = true, st.pos = 0;
}

void listener(const float eye[3], const float right[3]) {
    std::lock_guard<std::mutex> lk(g_mu);
    memcpy(g_eye, eye, sizeof g_eye), memcpy(g_right, right, sizeof g_right);
}

void mix(int32_t* left, int32_t* right, int frames) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& v : g_voices) {
        const Clip& c = g_clips[v.clip];
        const float dx = v.x - g_eye[0], dy = v.y - g_eye[1], dz = v.z - g_eye[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float gain = v.volume * std::clamp((kFar - d) / (kFar - kNear), 0.f, 1.f);
        const float pan = d > 1 ? std::clamp((dx * g_right[0] + dy * g_right[1] + dz * g_right[2]) / d, -1.f, 1.f) : 0;
        const float gl = gain * std::sqrt(0.5f * (1 - pan)), gr = gain * std::sqrt(0.5f * (1 + pan));
        const double step = c.rate / 48000.0;
        for (int i = 0; i < frames; i++) {
            size_t k = (size_t)v.pos;
            if (v.pos >= v.end) {
                if (!v.loop) { v.pos = -1; break; }
                v.pos = 0, k = 0;
            }
            const float t = (float)(v.pos - k), s = (c.mono[k] + (c.mono[k + 1] - c.mono[k]) * t) * fade(v, c);
            left[i] += (int32_t)(s * gl), right[i] += (int32_t)(s * gr);
            v.pos += step;
        }
    }
    std::erase_if(g_voices, [](const Voice& v) { return v.pos < 0; });
    for (auto& [key, st] : g_streams) {
        if (!st.playing) continue;
        const float dx = st.x - g_eye[0], dy = st.y - g_eye[1], dz = st.z - g_eye[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float gain = std::clamp((kFar - d) / (kFar - kNear), 0.f, 1.f);
        const float pan = d > 1 ? std::clamp((dx * g_right[0] + dy * g_right[1] + dz * g_right[2]) / d, -1.f, 1.f) : 0;
        const float gl = gain * std::sqrt(0.5f * (1 - pan)), gr = gain * std::sqrt(0.5f * (1 + pan));
        const double step = st.rate / 48000.0;
        for (int i = 0; i < frames; i++) {
            if (st.pos + 1 >= st.q.size()) { st.playing = false; break; }  // (ran dry: queues again)
            const size_t k = (size_t)st.pos;
            const float t = (float)(st.pos - k), v = st.q[k] + (st.q[k + 1] - st.q[k]) * t;
            left[i] += (int32_t)(v * gl), right[i] += (int32_t)(v * gr);
            st.pos += step;
        }
        const size_t used = (size_t)st.pos;
        st.q.erase(st.q.begin(), st.q.begin() + std::min(used, st.q.size()));
        st.pos -= (double)used;
    }
}

}  // namespace sfx

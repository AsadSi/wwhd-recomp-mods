// The microphone, Mac build (coop_mic.h): AVAudioEngine's input node, its first channel brought to 16 kHz (linear) and
// kept for take() (at most a second).
#include "coop_mic.h"

#if !defined(WWHD_SDL_HOST)
#import <AVFoundation/AVFoundation.h>

#include <deque>
#include <mutex>

namespace mods::coop::mic {
namespace {
AVAudioEngine* g_engine = nil;
std::mutex g_mu;
std::deque<int16_t> g_got;
double g_at = 0;   // where the next 16 kHz sample falls in the input, in input samples from this buffer's start
float g_last = 0;  // the previous buffer's last input sample
}  // namespace

bool open(std::string& why) {
    if (g_engine) return true;
    if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusDenied) {
        why = "the microphone is not allowed (System Settings > Privacy & Security > Microphone)";
        return false;
    }
    AVAudioEngine* engine = [[AVAudioEngine alloc] init];
    AVAudioInputNode* input = engine.inputNode;
    AVAudioFormat* format = [input outputFormatForBus:0];
    if (format.sampleRate <= 0 || format.channelCount == 0) { why = "no microphone"; return false; }
    const double step = format.sampleRate / kRate;
    // (deprecated in macOS 27, whose replacement with an error: is not in macOS 14, which the game supports)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer* buf, AVAudioTime*) {
        const float* s = buf.floatChannelData ? buf.floatChannelData[0] : nullptr;
        const int n = (int)buf.frameLength;
        if (!s || n <= 0) return;
        std::lock_guard<std::mutex> lk(g_mu);
        for (; g_at < n; g_at += step) {
            const int k = (int)g_at;
            const float a = k > 0 ? s[k - 1] : g_last, b = s[k], t = (float)(g_at - k);
            const float v = (a + (b - a) * t) * 32767.f;
            g_got.push_back((int16_t)std::max(-32768.f, std::min(32767.f, v)));
        }
        g_at -= n, g_last = s[n - 1];
        while (g_got.size() > (size_t)kRate) g_got.pop_front();
    }];
#pragma clang diagnostic pop
    NSError* err = nil;
    if (![engine startAndReturnError:&err]) {
        why = err ? std::string(err.localizedDescription.UTF8String) : "the microphone does not start";
        [input removeTapOnBus:0];
        return false;
    }
    g_engine = engine;
    return true;
}

void close() {
    if (!g_engine) return;
    [g_engine.inputNode removeTapOnBus:0];
    [g_engine stop];
    g_engine = nil;
    std::lock_guard<std::mutex> lk(g_mu);
    g_got.clear(), g_at = 0, g_last = 0;
}

size_t take(int16_t* out, size_t max) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t n = 0;
    for (; n < max && !g_got.empty(); n++) out[n] = g_got.front(), g_got.pop_front();
    return n;
}

}  // namespace mods::coop::mic
#endif

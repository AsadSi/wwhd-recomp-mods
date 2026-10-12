// Shadow stalker (shadow_beast.h): the Shadow Beast's asset, its pose from what its Bokoblin body does, and the
// hooks that keep the body and its weapon from being drawn.
#include "shadow_beast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "guest_addr.h"
#include "mods.h"
#include "../gx2/gx2.h"
#include "../gx2/gx2_cmd.h"
#include "runtime.h"

namespace interp { float pass_t(); }

namespace mods::beast {
namespace {

constexpr uint32_t kPos = 0x314, kAngleY = 0x32A, kHealth = 0x3A1, kMaxHealth = 0x3A0, kWeaponId = 0x1360;
constexpr float kScale = 2.2f;      // TP's beast is smaller than WW's Link; this makes it stand over him
constexpr int kBodyHealth = 24;     // a Bokoblin has 5: about a dozen sword hits
constexpr float kFadeSteps = 30;    // a second to come out of the fog or fade back into it

Asset g_asset;
bool g_loaded = false, g_ok = false;

bool load() {
    g_loaded = true;
    const std::string path = config::game_dir + "/shadow_beast.bin";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { trace("beast: %s missing (tools/shadow_beast.py makes it from the TP HD dump)", path.c_str()); return false; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t o = 0;
    bool bad = false;
    auto take = [&](void* out, size_t n) {
        if (o + n > d.size()) { bad = true; memset(out, 0, n); return; }
        memcpy(out, d.data() + o, n), o += n;
    };
    auto u32 = [&] { uint32_t v; take(&v, 4); return v; };
    char magic[4];
    take(magic, 4);
    if (memcmp(magic, "SBST", 4) != 0 || u32() != 1) { trace("beast: %s is not a shadow_beast.bin v1", path.c_str()); return false; }
    Asset& a = g_asset;
    const uint32_t joints = u32();
    a.parent.resize(joints), a.inv_bind.resize(joints * 12);
    for (uint32_t j = 0; j < joints && !bad; j++) take(&a.parent[j], 4), take(&a.inv_bind[j * 12], 48);
    const uint32_t verts = u32();
    a.verts.resize((size_t)verts * kVertexSize);
    take(a.verts.data(), a.verts.size());
    a.indices.resize(u32());
    take(a.indices.data(), a.indices.size() * 2);
    a.groups.resize(u32());
    take(a.groups.data(), a.groups.size() * sizeof(Asset::Group));
    a.textures.resize(u32());
    for (auto& t : a.textures) t.w = u32(), t.h = u32(), t.rgba.resize((size_t)t.w * t.h * 4), take(t.rgba.data(), t.rgba.size());
    a.anims.resize(u32());
    for (auto& an : a.anims) {
        char name[32];
        take(name, 32);
        an.name.assign(name, strnlen(name, 32));
        an.frames = u32(), an.loop = u32();
        an.keys.resize((size_t)an.frames * joints * 10);
        take(an.keys.data(), an.keys.size() * 4);
    }
    if (bad || joints == 0 || a.anims.empty()) { trace("beast: %s is cut short", path.c_str()); return false; }
    trace("beast: %u joints, %u vertices, %zu triangles, %zu animations", joints, verts, a.indices.size() / 3, a.anims.size());
    return true;
}

int anim(const char* name) {
    for (size_t i = 0; i < g_asset.anims.size(); i++)
        if (g_asset.anims[i].name == name) return (int)i;
    return 0;
}

// what is played: an animation at a frame (fractional), at a rate in frames per logic step, and the one it blends
// in from (its weight falling from 1 to 0 over kBlendSteps)
struct Play { int anim = -1; float frame = 0, rate = 1; bool once = false; };  // once: a strike, hit or death plays through once
constexpr float kBlendSteps = 6;

struct State {
    bool out = false;          // drawn
    uint32_t body = 0;         // the Bokoblin (0: gone)
    Play cur, from;
    float blend = 0;           // weight of `from`
    bool locked = false;       // a one-shot (hit, strike, death) plays to its end
    bool dying = false;
    float fade = 0;            // 0 unseen .. 1 fully there
    float fade_to = 1;
    int health = 0;
    double x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;  // this step's place and the last's
    float yaw = 0, pyaw = 0;
    float pfade = 0;
    std::vector<uint32_t> weapons;  // the Boko weapons it has held: never drawn, deleted with it
} g;

uint32_t g_body_draw = 0;          // for the draw hooks (they must not call into the game)
std::vector<uint32_t> g_hidden;    // weapon actors

void play(int a, float rate, bool lock) {
    if (g.cur.anim == a && !lock) { g.cur.rate = rate; return; }
    g.from = g.cur, g.blend = g.cur.anim >= 0 ? 1.f : 0.f;
    g.cur = {a, 0, rate, lock}, g.locked = lock;
}

void advance(Play& p) {
    if (p.anim < 0) return;
    const auto& an = g_asset.anims[p.anim];
    p.frame += p.rate;
    if (an.loop && !p.once) p.frame = std::fmod(p.frame, (float)an.frames);
    else p.frame = std::min(p.frame, (float)(an.frames - 1));
}

bool finished(const Play& p) { return p.anim >= 0 && (p.once || !g_asset.anims[p.anim].loop) && p.frame >= g_asset.anims[p.anim].frames - 1; }

// joint j of animation `p` at `frame`: scale, rotation (quaternion), translation
void sample(const Play& p, float frame, int j, float s[3], float q[4], float t[3]) {
    const auto& an = g_asset.anims[p.anim];
    const int joints = (int)g_asset.parent.size();
    const bool loop = an.loop && !p.once;
    frame = loop ? std::fmod(frame + an.frames, (float)an.frames) : std::clamp(frame, 0.f, (float)(an.frames - 1));
    const int f0 = (int)frame, f1 = loop ? (f0 + 1) % an.frames : std::min<int>(f0 + 1, an.frames - 1);
    const float k = frame - f0;
    const float* a = &an.keys[((size_t)f0 * joints + j) * 10];
    const float* b = &an.keys[((size_t)f1 * joints + j) * 10];
    const float sign = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6] < 0 ? -1.f : 1.f;
    for (int i = 0; i < 3; i++) s[i] = a[i] + (b[i] - a[i]) * k, t[i] = a[7 + i] + (b[7 + i] - a[7 + i]) * k;
    for (int i = 0; i < 4; i++) q[i] = a[3 + i] + (sign * b[3 + i] - a[3 + i]) * k;
}

// row-major 3x4: c = a * b
void mul34(const float* a, const float* b, float* c) {
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++)
            c[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + (k == 3 ? a[r * 4 + 3] : 0);
}

// the skinning matrices (model space) of the pose `back` logic steps before this one (0..1: between the last step and
// this one)
void pose(float back, float* out) {
    const int joints = (int)g_asset.parent.size();
    std::vector<float> world(joints * 12);
    for (int j = 0; j < joints; j++) {
        float s[3], q[4], t[3];
        sample(g.cur, g.cur.frame - back * g.cur.rate, j, s, q, t);
        const float w = std::clamp(g.blend + back / kBlendSteps, 0.f, 1.f);
        if (w > 0 && g.from.anim >= 0) {
            float s2[3], q2[4], t2[3];
            sample(g.from, g.from.frame - back * g.from.rate, j, s2, q2, t2);
            const float sign = q[0] * q2[0] + q[1] * q2[1] + q[2] * q2[2] + q[3] * q2[3] < 0 ? -1.f : 1.f;
            for (int i = 0; i < 3; i++) s[i] += (s2[i] - s[i]) * w, t[i] += (t2[i] - t[i]) * w;
            for (int i = 0; i < 4; i++) q[i] += (sign * q2[i] - q[i]) * w;
        }
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        const float x = q[0] / n, y = q[1] / n, z = q[2] / n, ww = q[3] / n;
        const float local[12] = {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y - z * ww) * s[1], 2 * (x * z + y * ww) * s[2], t[0],
                                 2 * (x * y + z * ww) * s[0], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z - x * ww) * s[2], t[1],
                                 2 * (x * z - y * ww) * s[0], 2 * (y * z + x * ww) * s[1], (1 - 2 * (x * x + y * y)) * s[2], t[2]};
        const int p = g_asset.parent[j];
        if (p >= 0) mul34(&world[p * 12], local, &world[j * 12]);
        else memcpy(&world[j * 12], local, sizeof local);
        mul34(&world[j * 12], &g_asset.inv_bind[j * 12], out + j * 12);
    }
}

}  // namespace

const Asset* asset() {
    if (!g_loaded) g_ok = load();
    return g_ok ? &g_asset : nullptr;
}

void set_body(uint32_t body) {
    if (body && body != g.body) {  // a new one comes out of the fog behind Link
        g = State{};
        g.out = true, g.body = body, g.fade = 0, g.fade_to = 1;
        g.x = g.px = ldf32(body + kPos), g.y = g.py = ldf32(body + kPos + 4), g.z = g.pz = ldf32(body + kPos + 8);
        g.yaw = g.pyaw = (int16_t)ld16(body + kAngleY) * (float)M_PI / 32768;
        st8(body + kMaxHealth, kBodyHealth), st8(body + kHealth, kBodyHealth);
        g.health = kBodyHealth;
        play(anim("s1_wait01"), 1, false);
        trace("beast: out, its body the Bokoblin at %08X", body);
    } else if (!body && g.body) {
        // gone: slain (its last health 0) falls dead; the event's end (time, Link at sea) fades it out where it is
        g.body = 0;
        if (g.health <= 0) g.dying = true, play(anim("s1_dead01"), 1, true), trace("beast: slain");
        else g.fade_to = 0, trace("beast: fades back into the fog");
    }
    g_body_draw = g.body;
}

void step(Cpu* c, uint32_t link, double) {
    if (!g.out) return;
    g.pfade = g.fade;
    g.px = g.x, g.py = g.y, g.pz = g.z, g.pyaw = g.yaw;
    if (const uint32_t body = g.body) {
        g.x = ldf32(body + kPos), g.y = ldf32(body + kPos + 4), g.z = ldf32(body + kPos + 8);
        float yaw = (int16_t)ld16(body + kAngleY) * (float)M_PI / 32768;
        while (yaw - g.pyaw > M_PI) yaw -= 2 * M_PI;
        while (yaw - g.pyaw < -M_PI) yaw += 2 * M_PI;
        g.yaw = yaw;
        // its weapons: one in its hands now, and any it held before (dropped when hit, or picked up)
        const uint32_t wid = ld32(body + kWeaponId);
        if (wid != 0xFFFFFFFF && std::find(g.weapons.begin(), g.weapons.end(), wid) == g.weapons.end()) g.weapons.push_back(wid);
        const int health = (int8_t)ld8(body + kHealth);
        const double moved = std::hypot(g.x - g.px, g.z - g.pz);
        const double to_link = std::hypot(ldf32(link + kPos) - g.x, ldf32(link + kPos + 8) - g.z);
        if (health < g.health && health > 0) play(anim("s1_damaged"), 1.3f, true);
        else if (g.locked && finished(g.cur)) g.locked = false;
        if (!g.locked) {
            if (to_link < 260 && moved < 6) play(anim(rand() % 2 ? "s1_attack" : "s1_attack02"), 1.2f, true);
            else if (moved > 9) play(anim("s1_dash01"), std::clamp((float)moved / 14.f, 0.8f, 1.6f), false);
            else if (moved > 1.5) play(anim("s1_walk"), std::clamp((float)moved / 2.f, 1.f, 3.f), false);
            else play(anim("s1_wait01"), 1, false);
        }
        g.health = health;
        static int n = 0;
        if (++n % 60 == 0)
            trace("beast: at %.0f %.0f %.0f, %s frame %.0f, health %d, %.0f from Link", g.x, g.y, g.z,
                  g_asset.anims[g.cur.anim].name.c_str(), g.cur.frame, health, to_link);
    }
    g_hidden.clear();
    for (uint32_t id : g.weapons)
        if (const uint32_t w = guest_call(c, GC(0x025DE50C), {id})) g_hidden.push_back(w);  // fopAcM_SearchByID
    if (!g.body && !g_hidden.empty()) {  // no Bokoblin's machete left lying about
        for (uint32_t id : g.weapons) guest_call(c, GC(0x025D57E4), {id});  // fopAcM_delete_id
        g.weapons.clear(), g_hidden.clear();
    }
    advance(g.cur), advance(g.from);
    g.blend = std::max(0.f, g.blend - 1 / kBlendSteps);
    if (g.dying && finished(g.cur)) g.fade_to = 0;
    g.fade = g.fade_to > g.fade ? std::min(g.fade_to, g.fade + 1 / kFadeSteps) : std::max(g.fade_to, g.fade - 1 / kFadeSteps);
    if (g.fade <= 0 && g.fade_to <= 0) g.out = false, trace("beast: gone");
}

void camera_drawn(uint32_t cam) {
    if (!g.out || !g_ok) return;
    const int joints = (int)g_asset.parent.size();
    static std::vector<uint32_t> words;
    words.assign(kPoseHeader + joints * 12, 0);
    float* f = reinterpret_cast<float*>(words.data());
    const float t = interp::pass_t(), back = 1 - t;
    f[0] = g.pfade + (g.fade - g.pfade) * t;
    const float yaw = g.pyaw + (g.yaw - g.pyaw) * t, cs = std::cos(yaw) * kScale, sn = std::sin(yaw) * kScale;
    const float world[12] = {cs, 0, sn, (float)(g.px + (g.x - g.px) * t),
                             0, kScale, 0, (float)(g.py + (g.y - g.py) * t),
                             -sn, 0, cs, (float)(g.pz + (g.z - g.pz) * t)};
    memcpy(f + 1, world, sizeof world);
    // camera_draw's matrices (HD view_class): the projection (GL style, 4x4) at +0x104, the view (3x4) at +0x144
    for (int i = 0; i < 12; i++) f[13 + i] = (float)ldf32(cam + 0x144 + 4 * i);
    for (int i = 0; i < 16; i++) f[25 + i] = (float)ldf32(cam + 0x104 + 4 * i);
    pose(back, f + kPoseHeader);
    gx2::emit_host(gx2::OP_BEAST, words.data(), (uint32_t)words.size());
}

}  // namespace mods::beast

// daBk_Draw (0209971C): the Shadow stalker's Bokoblin body is not drawn (the beast is, in its place)
extern "C" void f_0209971C_orig(Cpu* c);
extern "C" void hook_0209971C(Cpu* c) {
    if (mods::beast::g_body_draw && c->r[3] == mods::beast::g_body_draw) { c->r[3] = 1; return; }
    f_0209971C_orig(c);
}
// daBoko draw (020C0CE0): nor its weapon
extern "C" void f_020C0CE0_orig(Cpu* c);
extern "C" void hook_020C0CE0(Cpu* c) {
    for (uint32_t w : mods::beast::g_hidden)
        if (c->r[3] == w) { c->r[3] = 1; return; }
    f_020C0CE0_orig(c);
}

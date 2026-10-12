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
constexpr float kScale = 1.3f;      // daE_S1's base size (its HIO mBaseSize)
constexpr int kBodyHealth = 24;     // a Bokoblin has 5: about a dozen sword hits
constexpr float kFadeSteps = 30;    // a second to fade back into the fog
constexpr int kHead = 4;            // the head joint (daE_S1 hangs its hair from getAnmMtx(4))
constexpr float kDropFrom = 1500;   // it drops from the sky as TP's do from a twilight portal (e_s1_warpappear)

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

// what is played: an animation at a frame (fractional), at a rate in frames per logic step; `from` blends out over
// `morf` steps (J3D's morf, the frames daE_S1's anm_init gives each switch)
struct Play { int anim = -1; float frame = 0, rate = 1; bool once = false; };  // once: a strike, hit or death plays through once

// a hair strand (daE_S1's s1_ke_s): its points, their last move, and the way it grows from the head
struct Ke { float p[kHairPoints][3], v[kHairPoints][3], dir[3], size[kHairPoints]; };

struct State {
    bool out = false;          // drawn
    uint32_t body = 0, id = 0; // the Bokoblin (0: gone) and its process id (the hair's random seed, as TP's)
    Play cur, from;
    float blend = 0, morf = 1; // weight of `from`, and the steps it takes to blend out
    bool locked = false;       // a one-shot (landing, hit, strike, death) plays to its end
    int dying = 0;             // 1 falling dead, 2 lying (deadwait), then it fades
    int lying = 0;
    float fade = 1, pfade = 1, fade_to = 1;
    float drop = 0, pdrop = 0, drop_v = 0;  // height above its body while it falls from the sky
    int health = 0;
    double x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;  // this step's place and the last's
    float yaw = 0, pyaw = 0;
    Ke ke[kHairStrands];
    float pke[kHairStrands][kHairPoints][3];  // the hair as the last step left it
    float light[14] = {};      // the scene's light where it stands (kLight in shadow_beast.h)
    std::vector<uint32_t> weapons;  // the Boko weapons it has held: never drawn, deleted with it
} g;

// the light the game gives an actor where it stands: its body's dKy_tevstr_c (+0x110, settingTevStruct each
// execute; HD layout: light position in the world +0x84, ambient C0 s16 +0x90, light colour K0 +0x98, fog colour s16
// +0xA0, fog start/end +0xA8/+0xAC), as WW's own models get it through setLightTevColorType
void read_light(uint32_t body) {
    const uint32_t tv = body + 0x110;
    float* l = g.light;
    for (int i = 0; i < 3; i++)
        l[i] = (int16_t)ld16(tv + 0x90 + 2 * i) / 255.f, l[3 + i] = ld8(tv + 0x98 + i) / 255.f, l[6 + i] = (float)ldf32(tv + 0x84 + 4 * i),
        l[9 + i] = (int16_t)ld16(tv + 0xA0 + 2 * i) / 255.f;
    l[12] = (float)ldf32(tv + 0xA8), l[13] = (float)ldf32(tv + 0xAC);
}

uint32_t g_body_draw = 0;          // for the draw hooks (they must not call into the game)
std::vector<uint32_t> g_hidden;    // weapon actors

void play(const char* name, float rate, bool lock, float morf) {
    const int a = anim(name);
    if (g.cur.anim == a && !lock) { g.cur.rate = rate; return; }
    g.from = g.cur, g.blend = g.cur.anim >= 0 ? 1.f : 0.f, g.morf = std::max(1.f, morf);
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

// the pose `back` logic steps before this one (0..1: between the last step and this one): each joint's matrix in
// model space (`world`) and its skinning matrix (`skin`, may be null)
void pose(float back, float* world, float* skin) {
    const int joints = (int)g_asset.parent.size();
    for (int j = 0; j < joints; j++) {
        float s[3], q[4], t[3];
        sample(g.cur, g.cur.frame - back * g.cur.rate, j, s, q, t);
        const float w = std::clamp(g.blend + back / g.morf, 0.f, 1.f);
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
        if (skin) mul34(&world[j * 12], &g_asset.inv_bind[j * 12], skin + j * 12);
    }
}

// its model's place in the world (TP's base matrix: translation, yaw, the base size), `t` of the way from the last step
void base(float t, float* m) {
    const float yaw = g.pyaw + (g.yaw - g.pyaw) * t, cs = std::cos(yaw) * kScale, sn = std::sin(yaw) * kScale;
    const float w[12] = {cs, 0, sn, (float)(g.px + (g.x - g.px) * t),
                         0, kScale, 0, (float)(g.py + (g.y - g.py) * t + g.pdrop + (g.drop - g.pdrop) * t),
                         -sn, 0, cs, (float)(g.pz + (g.z - g.pz) * t)};
    memcpy(m, w, sizeof w);
}

// ---- the hair: daE_S1's ke_set / ke_move / ke_control (d_a_e_s1.cpp), with its own random numbers (cM_rnd2)
int g_r0, g_r1, g_r2;
float rnd2(float max) {
    g_r0 = g_r0 * 171 % 30269, g_r1 = g_r1 * 172 % 30307, g_r2 = g_r2 * 170 % 30323;
    return std::fabs(std::fmod(g_r0 / 30269.f + g_r1 / 30307.f + g_r2 / 30323.f, 1.f)) * max;
}
float ssin(int16_t a) { return std::sin(a * (float)M_PI / 32768); }
int16_t atan2s(float y, float x) { return (int16_t)(int)(std::atan2(y, x) * 32768 / (float)M_PI); }
void rot_x(float* m, int16_t a) { const float c = ssin((int16_t)(a + 0x4000)), s = ssin(a); const float r[12] = {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}; float o[12]; mul34(m, r, o); memcpy(m, o, 48); }
void rot_y(float* m, int16_t a) { const float c = ssin((int16_t)(a + 0x4000)), s = ssin(a); const float r[12] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}; float o[12]; mul34(m, r, o); memcpy(m, o, 48); }
void rot_z(float* m, int16_t a) { const float c = ssin((int16_t)(a + 0x4000)), s = ssin(a); const float r[12] = {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}; float o[12]; mul34(m, r, o); memcpy(m, o, 48); }
void xf(const float* m, const float* v, float* o) {
    for (int r = 0; r < 3; r++) o[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3];
}

void ke_control(Ke& k, float len, float ground) {
    const int16_t phase = (int16_t)(int)rnd2(65536);
    for (int i = 1; i < kHairPoints; i++) {
        const float sx = 3 * ssin((int16_t)(phase + i * 7000)), sz = 3 * ssin((int16_t)(phase + 10000 + i * 6000));
        const float f = (16 - i) * 0.1f;
        const float ax = k.v[i][0] + sx + k.dir[0] * f, ay = k.v[i][1] + k.dir[1] * f, az = k.v[i][2] + sz + k.dir[2] * f;
        const float x = ax + (k.p[i][0] - k.p[i - 1][0]), z = az + (k.p[i][2] - k.p[i - 1][2]);
        const float y = std::max(ground, -12 + k.p[i][1] + ay) - k.p[i - 1][1];
        float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        rot_x(m, (int16_t)-atan2s(y, z));
        rot_y(m, atan2s(x, std::sqrt(y * y + z * z)));
        const float step[3] = {m[2] * len, m[6] * len, m[10] * len};
        const float was[3] = {k.p[i][0], k.p[i][1], k.p[i][2]};
        for (int c = 0; c < 3; c++) k.p[i][c] = k.p[i - 1][c] + step[c], k.v[i][c] = 0.8f * (k.p[i][c] - was[c]);
    }
}

void ke_set(const float* head) {  // head: the head joint's world matrix
    static const int16_t ke_za[kHairStrands] = {-0x320, -0x190, 0, 0x190, 0x320, -0x320, -0x190, 0, 0x190, 0x320, -0x320,
                                                -0x190, 0, 0x190, 0x320, 0x4B0, -0x320, -0x190, 0, 0x190, 0x320, 0x4B0};
    g_r0 = 12, g_r1 = 123, g_r2 = (int)g.id * 2 + 50;
    for (int i = 0; i < kHairStrands; i++) {
        Ke& k = g.ke[i];
        float m[12];
        memcpy(m, head, 48);
        rot_y(m, 0x4000);
        rot_x(m, (int16_t)((int)rnd2(2000) + 3000));
        rot_z(m, (int16_t)(5 * ke_za[i] + (i < 10 ? 0 : 0x8000)));
        float len, ox, oz;
        if (i < 10) len = rnd2(2.25f) + 7.5f, ox = rnd2(10) - 5, oz = rnd2(20) - 10;
        else len = rnd2(3) + 8, ox = rnd2(20) - 10, oz = rnd2(30) - 15;
        const float root[3] = {ox, 15, oz}, tip[3] = {ox, 35, oz};
        float r[3];
        xf(m, root, k.p[0]);
        xf(m, tip, r);
        for (int c = 0; c < 3; c++) k.dir[c] = r[c] - k.p[0][c];
        ke_control(k, len, 3 + (float)g.y);
        for (int j = 0; j < kHairPoints; j++) k.size[j] = j == 14 ? 0.6f : j < 5 ? 3.5f : rnd2(1) + 1.8f;
    }
}

}  // namespace

const Asset* asset() {
    if (!g_loaded) g_ok = load();
    return g_ok ? &g_asset : nullptr;
}

void set_body(uint32_t body) {
    if (body && body != g.body) {  // a new one drops from the sky behind Link
        g = State{};
        g.out = true, g.body = body, g.id = ld32(body + 0x4);  // base_process_class::mBsPcId
        g.x = g.px = ldf32(body + kPos), g.y = g.py = ldf32(body + kPos + 4), g.z = g.pz = ldf32(body + kPos + 8);
        g.yaw = g.pyaw = (int16_t)ld16(body + kAngleY) * (float)M_PI / 32768;
        g.drop = g.pdrop = kDropFrom;
        st8(body + kMaxHealth, kBodyHealth), st8(body + kHealth, kBodyHealth);
        g.health = kBodyHealth;
        read_light(body);
        play("s2_down", 0, true, 1);  // held at its first frame as it falls; played on landing (e_s1_warpappear)
        std::vector<float> world(g_asset.parent.size() * 12);
        float m[12], head[12];
        pose(0, world.data(), nullptr), base(1, m), mul34(m, &world[kHead * 12], head);
        for (auto& k : g.ke)  // the hair starts hanging straight down from the head
            for (int j = 0; j < kHairPoints; j++)
                k.p[j][0] = head[3], k.p[j][1] = head[7] - j * 8, k.p[j][2] = head[11], k.v[j][0] = k.v[j][1] = k.v[j][2] = 0;
        trace("beast: out, its body the Bokoblin at %08X", body);
    } else if (!body && g.body) {
        // gone: slain (its last health 0) falls dead as TP's do (one of dead02-04, then it lies there) and fades;
        // the event's end (time, Link at sea) fades it out where it is
        g.body = 0;
        if (g.health <= 0) {
            static const char* kDead[] = {"s2_dead02", "s2_dead03", "s2_dead04"};
            const int k = rand() % 3;
            g.dying = 1 + k, play(kDead[k], 1, true, 2);
            trace("beast: slain");
        } else g.fade_to = 0, trace("beast: fades back into the fog");
    }
    g_body_draw = g.body;
}

void step(Cpu* c, uint32_t link, double) {
    if (!g.out) return;
    g.pfade = g.fade, g.pdrop = g.drop;
    g.px = g.x, g.py = g.y, g.pz = g.z, g.pyaw = g.yaw;
    for (int i = 0; i < kHairStrands; i++) memcpy(g.pke[i], g.ke[i].p, sizeof g.pke[i]);
    if (g.drop > 0) {  // falling from the sky
        g.drop_v -= 6, g.drop = std::max(0.f, g.drop + g.drop_v);
        if (g.drop == 0) play("s2_down", 1, true, 1);  // lands
    }
    if (const uint32_t body = g.body) {
        g.x = ldf32(body + kPos), g.y = ldf32(body + kPos + 4), g.z = ldf32(body + kPos + 8);
        float yaw = (int16_t)ld16(body + kAngleY) * (float)M_PI / 32768;
        while (yaw - g.pyaw > M_PI) yaw -= 2 * M_PI;
        while (yaw - g.pyaw < -M_PI) yaw += 2 * M_PI;
        g.yaw = yaw;
        read_light(body);
        // its weapons: one in its hands now, and any it held before (dropped when hit, or picked up)
        const uint32_t wid = ld32(body + kWeaponId);
        if (wid != 0xFFFFFFFF && std::find(g.weapons.begin(), g.weapons.end(), wid) == g.weapons.end()) g.weapons.push_back(wid);
        const int health = (int8_t)ld8(body + kHealth);
        const double moved = std::hypot(g.x - g.px, g.z - g.pz);
        const double to_link = std::hypot(ldf32(link + kPos) - g.x, ldf32(link + kPos + 8) - g.z);
        // what it does, as daE_S1's actions animate it (anm_init's animations, rates and morf frames): hit
        // (e_s1_damage), strikes when close (e_s1_fight), runs at Link (e_s1_fight_run), walks (e_s1_wait), waits
        if (health < g.health && health > 0 && g.drop == 0) play("s2_damaged", 1, true, 1);
        else if (g.locked && finished(g.cur)) g.locked = false;
        if (!g.locked) {
            constexpr float kDash = 20 * kScale, kWalk = 4 * kScale;  // its HIO dash and walk speeds, per step
            if (to_link < 260 && moved < 6) play(rand() % 2 ? "s2_attack" : "s2_attack02", 1, true, 3);
            else if (moved > 9) play("s2_dash01", std::clamp((float)moved / kDash, 0.7f, 1.5f), false, 3);
            else if (moved > 1.5) play("s2_walk", std::clamp((float)moved / kWalk, 0.7f, 2.f), false, 10);
            else play("s2_wait01", 1, false, 10);
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
    g.blend = std::max(0.f, g.blend - 1 / g.morf);
    if (g.dying && g.locked && finished(g.cur)) {  // fallen: it lies there (deadwait) a moment, then fades
        static const char* kWait[] = {"s2_deadwait02", "s2_deadwait03", "s2_deadwait04"};
        play(kWait[g.dying - 1], 1, false, 2), g.locked = false, g.lying = 0;
    }
    if (g.dying && !g.locked && ++g.lying > 45) g.fade_to = 0;
    g.fade = g.fade_to > g.fade ? std::min(g.fade_to, g.fade + 1 / kFadeSteps) : std::max(g.fade_to, g.fade - 1 / kFadeSteps);
    if (g.fade <= 0 && g.fade_to <= 0) { g.out = false, trace("beast: gone"); return; }
    // the hair follows the head as it now is
    std::vector<float> world(g_asset.parent.size() * 12);
    float m[12], head[12];
    pose(0, world.data(), nullptr), base(1, m), mul34(m, &world[kHead * 12], head);
    ke_set(head);
}

void camera_drawn(uint32_t cam) {
    if (!g.out || !g_ok) return;
    const int joints = (int)g_asset.parent.size();
    static std::vector<uint32_t> words;
    words.assign(kPoseHeader + joints * 12 + kHairStrands * kHairPoints * 4, 0);
    float* f = reinterpret_cast<float*>(words.data());
    const float t = interp::pass_t(), back = 1 - t;
    f[0] = g.pfade + (g.fade - g.pfade) * t;
    base(t, f + 1);
    // camera_draw's matrices (HD view_class): the projection (GL style, 4x4) at +0x104, the view (3x4) at +0x144
    for (int i = 0; i < 12; i++) f[13 + i] = (float)ldf32(cam + 0x144 + 4 * i);
    for (int i = 0; i < 16; i++) f[25 + i] = (float)ldf32(cam + 0x104 + 4 * i);
    f[41] = (float)(g.py + (g.y - g.py) * t);
    memcpy(f + kLight, g.light, sizeof g.light);
    std::vector<float> world(joints * 12);
    pose(back, world.data(), f + kPoseHeader);
    float* h = f + kPoseHeader + joints * 12;
    for (int i = 0; i < kHairStrands; i++)
        for (int j = 0; j < kHairPoints; j++, h += 4) {
            for (int c = 0; c < 3; c++) h[c] = g.pke[i][j][c] + (g.ke[i].p[j][c] - g.pke[i][j][c]) * t;
            h[3] = g.ke[i].size[j];
        }
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

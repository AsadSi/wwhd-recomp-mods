// Shadow stalker (shadow_beast.h): the Shadow Beast's hair, sounds and mind (daE_S1's, d_a_e_s1.cpp), and the hooks
// that keep its Bokoblin body and weapon from being drawn.
#include "shadow_beast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "guest_addr.h"
#include "mods.h"
#include "runtime.h"
#include "tp_fx.h"
#include "tp_monsters.h"

static uint32_t u32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

namespace mods::beast {
namespace {
using tpm::mul34;

constexpr uint32_t kPos = 0x314, kAngleY = 0x32A, kHealth = 0x3A1, kMaxHealth = 0x3A0, kWeaponId = 0x1360;
constexpr int kBodyHealth = 24;     // a Bokoblin has 5: about a dozen sword hits
constexpr float kFadeSteps = 30;    // a second to fade back into the fog
constexpr int kHead = 4;            // the head joint (daE_S1 hangs its hair from getAnmMtx(4))
constexpr float kDropFrom = 1500;   // it drops from the sky as TP's do from a twilight portal (e_s1_warpappear)
constexpr int kHairStrands = 22, kHairPoints = 16;  // daE_S1's mKe: 22 strands, 16 points each

// a hair strand (daE_S1's s1_ke_s): its points, their last move, and the way it grows from the head
struct Ke { float p[kHairPoints][3], v[kHairPoints][3], dir[3], size[kHairPoints]; };

struct State {
    tpm::Monster m;
    uint32_t body = 0, id = 0;  // the Bokoblin (0: gone) and its process id (the hair's random seed, as TP's)
    int dying = 0;              // 1-3: fallen dead with dead02-04, then lying (deadwait) before it dissolves
    int lying = 0, steps = 0;
    float drop_v = 0;
    int health = 0;
    Ke ke[kHairStrands];
    uint32_t body_fx = 0;       // TP's 0x387 on it (body_eff_set: dark wisps from its middle, joint 2)
    std::vector<uint32_t> weapons;  // the Boko weapons it has held: never drawn, deleted with it
} g;

uint32_t g_body_draw = 0;          // for the draw hooks (they must not call into the game)
uint32_t g_link = 0;               // Link, as the last step saw him
std::vector<uint32_t> g_hidden;    // weapon actors

void play(const char* name, float rate, bool lock, float morf) { g.m.anim.play(name, rate, lock, morf); }
bool finished() { return g.m.anim.finished(); }
void sound(uint32_t id, float seconds = 0) { tpm::sound(id, (float)g.m.x, (float)g.m.y + 80, (float)g.m.z, seconds); }

// ---- the hair: daE_S1's ke_set / ke_move / ke_control (d_a_e_s1.cpp), with its own random numbers (cM_rnd2)
int g_r0, g_r1, g_r2;
float rnd2(float max) {
    g_r0 = g_r0 * 171 % 30269, g_r1 = g_r1 * 172 % 30307, g_r2 = g_r2 * 170 % 30323;
    return std::fabs(std::fmod(g_r0 / 30269.f + g_r1 / 30307.f + g_r2 / 30323.f, 1.f)) * max;
}
float ssin(int16_t a) { return std::sin(a * (float)M_PI / 32768); }
int16_t atan2s(float y, float x) { return (int16_t)(int)(std::atan2(y, x) * 32768 / (float)M_PI); }
void rot_x(float* m, int16_t a) { const float c = ssin((int16_t)(a + 0x4000)), s = ssin(a); const float r[12] = {1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}; mul34(m, r, m); }
void rot_y(float* m, int16_t a) { const float c = ssin((int16_t)(a + 0x4000)), s = ssin(a); const float r[12] = {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}; mul34(m, r, m); }
void rot_z(float* m, int16_t a) { const float c = ssin((int16_t)(a + 0x4000)), s = ssin(a); const float r[12] = {c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}; mul34(m, r, m); }
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
        ke_control(k, len, 3 + (float)g.m.y);
        for (int j = 0; j < kHairPoints; j++) k.size[j] = j == 14 ? 0.6f : j < 5 ? 3.5f : rnd2(1) + 1.8f;
    }
    // to the monster, for drawing
    g.m.hair.resize(kHairStrands * kHairPoints * 4);
    float* h = g.m.hair.data();
    for (auto& k : g.ke)
        for (int j = 0; j < kHairPoints; j++, h += 4) h[0] = k.p[j][0], h[1] = k.p[j][1], h[2] = k.p[j][2], h[3] = k.size[j];
}

// anm_se_set (d_a_e_s1.cpp): sounds on animation frames, as checkFrame sees the frame pass
enum : uint32_t { kUdehuri = 0x7000D, kDosa = 0x7000F, kVDamage = 0x70010, kVDeath = 0x70011, kFnL = 0x700FA, kFnR = 0x700FB,
                  kHandL = 0x700FC, kHandR = 0x700FD, kVAttack = 0x70115, kVSearch = 0x70116, kVWait = 0x70118, kVAttack2 = 0x70119 };
void anm_se(int anim_was, float from, float to) {
    struct Cue { const char* anim; float frame; uint32_t id; float seconds; };  // steps and hand slaps: half a second
    static const Cue kCues[] = {
        {"s2_walk", 27, kFnL, 0.5f}, {"s2_walk", 57, kFnR, 0.5f},
        {"s2_dash01", 1, kHandR, 0.5f}, {"s2_dash01", 3, kFnR, 0.5f}, {"s2_dash01", 4, kHandL, 0.5f}, {"s2_dash01", 7, kFnL, 0.5f},
        {"s2_dash02", 1, kHandR, 0.5f}, {"s2_dash02", 6, kFnR, 0.5f}, {"s2_dash02", 8, kHandL, 0.5f}, {"s2_dash02", 14, kFnL, 0.5f},
        {"s2_wait01", 0, kVWait, 0}, {"s2_wait01", 45, kVWait, 0}, {"s2_wait02", 0, kVSearch, 0}, {"s2_wait02", 45, kVSearch, 0},
        {"s2_attack", 21, kUdehuri, 0}, {"s2_attack", 25, kHandL, 0.5f}, {"s2_attack02", 25, kUdehuri, 0},
    };
    if (g.m.anim.cur.anim != anim_was) from = -1;  // a new animation: its frame 0 counts
    for (const Cue& c : kCues)
        if (g.m.anim.is(c.anim) && (to >= from ? c.frame > from && c.frame <= to : c.frame > from || c.frame <= to))
            sound(c.id, c.seconds);
}

// ---- its mind: daE_S1's fight actions (d_a_e_s1.cpp: e_s1_fight_run, e_s1_fight, e_s1_damage), run in place of the
// Bokoblin's own (Bk_move, hooked below) on the Bokoblin's body, whose execute then moves it with its ground and wall
// checks, takes Link's hits (its health) and, once that runs out, dies as the Bokoblin does (Bk_move again, unseen)
constexpr uint32_t kSpeedF = 0x370, kSpeed = 0x33C, kAtSph = 0x11A0;  // the body's speedF, speed, weapon AT sphere
constexpr int kHandJoint = 15;  // its strikes hit with the right hand
enum Act { kAppear, kRun, kFight, kDamage };
struct Mind {
    Act act = kAppear;
    int mode = 0, timer = 0, health = kBodyHealth;
    bool slow = false, at_was = false;
    float speed = 0, push = 0;  // speedF (TP units, x the base size when moved) and the push back from a hit
    int16_t push_angle = 0;
} g_mind;

float add_calc2(float v, float target, float max) { return v + std::clamp(target - v, -max, max); }  // cLib_addCalc2, scale 1
int16_t add_angle(int16_t a, int16_t target, int scale, int max) {                                 // cLib_addCalcAngleS2
    return (int16_t)(a + std::clamp((int16_t)(target - a) / scale, -max, max));
}

void think(Cpu* c, uint32_t body) {
    Mind& m = g_mind;
    const uint32_t pl = g_link;
    if (!pl) return;
    const float dx = (float)(ldf32(pl + kPos) - ldf32(body + kPos)), dz = (float)(ldf32(pl + kPos + 8) - ldf32(body + kPos + 8));
    const float dist = std::sqrt(dx * dx + dz * dz);
    const int16_t to_link = atan2s(dx, dz);
    int16_t angle = (int16_t)ld16(body + kAngleY);
    const int health = (int8_t)ld8(body + kHealth);
    if (health < m.health && health > 0) m.act = kDamage, m.mode = 0, m.speed = 0, m.push = 30, m.push_angle = to_link, sound(kVDamage);
    m.health = health;
    if (m.timer > 0) m.timer--;
    bool at = false;
    tpm::Animator& an = g.m.anim;
    const int frame = (int)an.cur.frame;
    switch (m.act) {
    case kAppear:  // e_s1_warpappear: it lands, gets up (s2_down), then hunts
        if (g.m.lift == 0 && finished()) an.locked = false, m.act = kRun, m.mode = 0;
        break;
    case kRun:  // e_s1_fight_run: dash at Link, turning hard, up to its dash speed
        if (m.mode == 0) play("s2_dash01", 1, false, 3), an.cur.frame = rnd2(10), m.mode = 1;
        angle = add_angle(angle, to_link, 4, 0x1000);
        m.speed = add_calc2(m.speed, 20, 20 * 0.333f);
        if (dist < 300) m.act = kFight, m.mode = 0;
        break;
    case kFight:  // e_s1_fight: wait a moment facing Link, then one of its two strikes
        if (m.mode == 0) play("s2_wait01", 1, false, 10), m.timer = 30 + (int)(rand() % 30), m.mode = 1;
        else if (m.mode == 1 && m.timer == 0) {
            m.slow = rand() % 3 != 0;
            play(m.slow ? "s2_attack02" : "s2_attack", 1, true, 3), m.mode = 2, sound(m.slow ? kVAttack2 : kVAttack);
        } else if (m.mode == 2) {
            at = m.slow ? frame >= 25 && frame <= 33 : frame >= 21 && frame <= 30;
            if (finished()) {
                an.locked = false;
                if (dist > 310) m.act = kRun;
                m.mode = 0;
            }
        }
        angle = add_angle(angle, to_link, 4, 0x800);
        m.speed = add_calc2(m.speed, 0, 10);
        break;
    case kDamage:  // e_s1_damage: reels, then runs at Link again
        if (m.mode == 0) play("s2_damaged", 1, true, 1), m.mode = 1;
        else if (finished()) an.locked = false, m.act = kRun, m.mode = 0;
        m.speed = 0;
        break;
    }
    // moved as daE_S1's action() moves it: forward at speedF x the base size; pushed back from a hit. TP's stages wall its
    // beasts in; WW's islands end in sea: it drops down any ledge after Link but stops where the ground 120 ahead is the
    // sea or none (dGrass's checkGroundY 02549F4C, as the spawns use), and waits there
    const float sn = ssin(angle), cs = ssin((int16_t)(angle + 0x4000));
    if (m.speed > 0) {
        static const uint32_t probe = mem::host_alloc(16);
        const float y = (float)ldf32(body + kPos + 4);
        st32(probe, u32_bits((float)ldf32(body + kPos) + sn * 120)), st32(probe + 4, u32_bits(y + 100));
        st32(probe + 8, u32_bits((float)ldf32(body + kPos + 8) + cs * 120));
        guest_call(c, GC(0x02549F4C), {probe});
        const float ground = (float)c->f[1].ps0;
        if (ground == y + 100 || ground < 5) {
            m.speed = 0;
            if (m.act == kRun) play("s2_wait01", 1, false, 10), m.mode = 0;
        }
    }
    const float sp = m.speed * g.m.scale;
    st16(body + kAngleY, (uint16_t)angle);
    st32(body + kSpeedF, u32_bits(sp));
    st32(body + kSpeed, u32_bits(sn * sp)), st32(body + kSpeed + 8, u32_bits(cs * sp));
    if (m.push > 0.01f) {
        for (int i : {0, 2})
            st32(body + kPos + 4 * i, u32_bits((float)(ldf32(body + kPos + 4 * i) - m.push * (i == 0 ? ssin(m.push_angle) : ssin((int16_t)(m.push_angle + 0x4000))))));
        m.push -= std::min(m.push, 7.f);
    }
    // its strike: the Bokoblin's weapon sphere (attack power 2) on its right hand, as daE_S1's mCcAtSph (40 x base size)
    const uint32_t sph = body + kAtSph;
    if (at) {
        static const uint32_t pos = mem::host_alloc(16);
        float h[12];
        g.m.joint(kHandJoint, h);
        for (int i = 0; i < 3; i++) st32(pos + 4 * i, u32_bits(h[i * 4 + 3]));
        guest_call(c, GC(m.at_was ? 0x025167E4 : 0x025167C0), {sph, pos});  // dCcD_Sph::MoveCAt / StartCAt
        c->f[1].ps0 = 40 * g.m.scale;
        guest_call(c, GC(0x02018C8C), {sph});                                 // SetR
        guest_call(c, GC(0x0200E240), {guest_call(c, GC(0x025200D4)) + 0x26A4, sph});  // dComIfG_Ccsp()->Set
    }
    m.at_was = at;
}

}  // namespace

bool available() { return tpm::asset(tpm::kShadowBeast) != nullptr; }

void set_body(uint32_t body) {
    if (body && body != g.body) {  // a new one drops from the sky behind Link
        g = State{};
        g.m.begin(tpm::kShadowBeast);
        g.body = body, g.id = ld32(body + 0x4);  // base_process_class::mBsPcId
        g_mind = Mind{};
        tpm::Monster& m = g.m;
        m.x = m.px = ldf32(body + kPos), m.y = m.py = ldf32(body + kPos + 4), m.z = m.pz = ldf32(body + kPos + 8);
        m.yaw = m.pyaw = (int16_t)ld16(body + kAngleY) * (float)M_PI / 32768;
        m.lift = m.plift = kDropFrom;
        static const int health = getenv("WWHD_TEST_BEAST_HEALTH") ? atoi(getenv("WWHD_TEST_BEAST_HEALTH")) : kBodyHealth;  // test aid
        st8(body + kMaxHealth, health), st8(body + kHealth, health);
        g.health = g_mind.health = health;
        tpm::read_light(body, m.light);
        play("s2_down", 0, true, 1);  // held at its first frame as it falls; played on landing (e_s1_warpappear)
        float head[12];
        m.joint(kHead, head);
        for (auto& k : g.ke)  // the hair starts hanging straight down from the head
            for (int j = 0; j < kHairPoints; j++)
                k.p[j][0] = head[3], k.p[j][1] = head[7] - j * 8, k.p[j][2] = head[11], k.v[j][0] = k.v[j][1] = k.v[j][2] = 0;
        ke_set(head);
        m.phair = m.hair;
        tpm::add(&g.m);
        trace("beast: out, its body the Bokoblin at %08X", body);
    } else if (!body && g.body) {
        g.body = 0;  // gone: slain (it fell when its health ran out, below), or the event's end fades it out where it is
        if (!g.dying) g.m.fade_to = 0, trace("beast: fades back into the fog");
    }
    g_body_draw = g.body;
}

void step(Cpu* c, uint32_t link, double) {
    g_link = link;
    tpfx::step();
    tpm::Monster& m = g.m;
    if (!m.out) return;
    m.step_start();
    if (m.lift > 0) {  // falling from the sky
        g.drop_v -= 6, m.lift = std::max(0.f, m.lift + g.drop_v);
        if (m.lift == 0) play("s2_down", 1, true, 1), sound(kDosa);  // lands (DOSA)
    }
    if (const uint32_t body = g.body) {
        m.x = ldf32(body + kPos), m.y = ldf32(body + kPos + 4), m.z = ldf32(body + kPos + 8);
        float yaw = (int16_t)ld16(body + kAngleY) * (float)M_PI / 32768;
        while (yaw - m.pyaw > M_PI) yaw -= 2 * M_PI;
        while (yaw - m.pyaw < -M_PI) yaw += 2 * M_PI;
        m.yaw = yaw;
        tpm::read_light(body, m.light);
        // its weapons: one in its hands now, and any it held before (dropped when hit, or picked up)
        const uint32_t wid = ld32(body + kWeaponId);
        if (wid != 0xFFFFFFFF && std::find(g.weapons.begin(), g.weapons.end(), wid) == g.weapons.end()) g.weapons.push_back(wid);
        int health = (int8_t)ld8(body + kHealth);
        static const double die_at = getenv("WWHD_TEST_BEAST_DIE") ? atof(getenv("WWHD_TEST_BEAST_DIE")) : -1;  // test aid
        if (die_at >= 0 && ++g.steps > die_at * 30 && !g.dying)  // (the body taken away, as its own death would)
            guest_call(c, GC(0x025D57E0), {body}), g.body = g_body_draw = 0, health = 0;
        if (health <= 0 && !g.dying) {  // slain: falls dead as TP's do (one of dead02-04), lies there, dissolves
            static const char* kDead[] = {"s2_dead02", "s2_dead03", "s2_dead04"};
            const int k = rand() % 3;
            g.dying = 1 + k, play(kDead[k], 1, true, 2), sound(kVDeath);
            trace("beast: slain");
        }
        static int n = 0;
        if (++n % 60 == 0)
            trace("beast: at %.0f %.0f %.0f, %s frame %.0f, health %d", m.x, m.y, m.z,
                  m.anim.a->anims[m.anim.cur.anim].name.c_str(), m.anim.cur.frame, health);
    }
    g_hidden.clear();
    for (uint32_t id : g.weapons)
        if (const uint32_t w = guest_call(c, GC(0x025DE50C), {id})) g_hidden.push_back(w);  // fopAcM_SearchByID
    if (!g.body && !g_hidden.empty()) {  // no Bokoblin's machete left lying about
        for (uint32_t id : g.weapons) guest_call(c, GC(0x025D57E4), {id});  // fopAcM_delete_id
        g.weapons.clear(), g_hidden.clear();
    }
    const int anim_was = m.anim.cur.anim;
    const float frame_was = m.anim.cur.frame;
    m.anim.advance();
    if (!g.dying) anm_se(anim_was, frame_was, m.anim.cur.frame);
    if (g.dying && m.anim.locked && finished()) {  // fallen: it lies there (deadwait) a moment, then dissolves
        static const char* kWait[] = {"s2_deadwait02", "s2_deadwait03", "s2_deadwait04"};
        play(kWait[g.dying - 1], 1, false, 2), m.anim.locked = false, g.lying = 0;
    }
    if (g.dying && !m.anim.locked && ++g.lying > 45 && m.fade_to > 0) {
        // it dissolves into twilight: fopAcM_createDisappear type 1 (d_a_disappear.cpp: 0x826C teal shards, 0x826D/E
        // the twilight's ribbons, at 1.2 x 1.666 its size), and is gone at once
        const float at[3] = {(float)m.x, (float)m.y, (float)m.z};
        for (uint32_t id : {0x826Cu, 0x826Du, 0x826Eu}) tpfx::spawn(id, at, 1.2f * 1.666f);
        tpm::sound(0x60028, at[0], at[1] + 80, at[2]);  // Z2SE_DARK_VANISH
        if (g.body) guest_call(c, GC(0x025D57E0), {g.body}), g.body = g_body_draw = 0;  // its body goes with it (fopAcM_delete)
        m.fade = m.pfade = m.fade_to = 0;
        trace("beast: dissolves");
    }
    m.fade = m.fade_to > m.fade ? std::min(m.fade_to, m.fade + 1 / kFadeSteps) : std::max(m.fade_to, m.fade - 1 / kFadeSteps);
    if (m.fade <= 0 && m.fade_to <= 0) {
        tpfx::stop(g.body_fx), g.body_fx = 0;
        m.out = false, tpm::remove(&g.m), trace("beast: gone");
        return;
    }
    // the hair follows the head as it now is
    float head[12], mid[12];
    m.joint(kHead, head);
    ke_set(head);
    // its wisps, from its middle as it moves (TP's setGlobalRTMatrix with joint 2's matrix); they stop as it dies
    if (!g.body_fx && !g.dying) {
        const float at[3] = {(float)m.x, (float)m.y, (float)m.z};
        g.body_fx = tpfx::spawn(0x387, at);
    }
    if (g.body_fx && g.dying) tpfx::stop(g.body_fx), g.body_fx = 0;
    if (g.body_fx) m.joint(2, mid), tpfx::follow(g.body_fx, mid);
}

}  // namespace mods::beast

// daBk_Draw (0209971C): the Shadow stalker's Bokoblin body is not drawn (the beast is, in its place)
extern "C" void f_0209971C_orig(Cpu* c);
extern "C" void hook_0209971C(Cpu* c) {
    if (mods::beast::g_body_draw && c->r[3] == mods::beast::g_body_draw) { c->r[3] = 1; return; }
    f_0209971C_orig(c);
}
// Bk_move (020A5EF4): the Bokoblin's own mind, replaced on the Shadow stalker's body by daE_S1's until its health runs out
// or it falls in the sea (where it drowns as the Bokoblin does: Bk_move's water_fail)
extern "C" void f_020A5EF4_orig(Cpu* c);
extern "C" void hook_020A5EF4(Cpu* c) {
    if (mods::beast::g_body_draw && c->r[3] == mods::beast::g_body_draw && (int8_t)ld8(c->r[3] + 0x3A1) > 0 &&
        ldf32(c->r[3] + 0x318) > -30) {
        mods::beast::think(c, c->r[3]);
        return;
    }
    f_020A5EF4_orig(c);
}
// daBoko draw (020C0CE0): nor its weapon (nor the Darknuts' swords: darknut.cpp)
namespace mods::darknut { bool hides_weapon(uint32_t actor); }
extern "C" void f_020C0CE0_orig(Cpu* c);
extern "C" void hook_020C0CE0(Cpu* c) {
    for (uint32_t w : mods::beast::g_hidden)
        if (c->r[3] == w) { c->r[3] = 1; return; }
    if (mods::darknut::hides_weapon(c->r[3])) { c->r[3] = 1; return; }
    f_020C0CE0_orig(c);
}

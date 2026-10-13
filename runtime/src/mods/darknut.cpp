// Twilight Princess's Darknuts in WW (darknut.h): daB_TN's "zako" (d_a_b_tn.cpp, type 1) mind, armour and weapons
// on WW Darknut bodies.
#include "darknut.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "guest_addr.h"
#include "mods.h"
#include "runtime.h"
#include "tp_fx.h"
#include "tp_monsters.h"

static uint32_t u32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

namespace mods::darknut {
namespace {
using tpm::mul34;
using Piece = tpm::Monster::Piece;

// the WW Darknut (daTn, HD): current.pos, angle.y, speed (xyz), speedF, health, its damage reaction's mode and action
// (s16), the armour it still has (bit 0 the body's, 1 the helmet, 2 the shield), its colour, its weapon AT spheres,
// and the process ids of its sword (daBoko) and cape (daMant)
constexpr uint32_t kPos = 0x314, kAngleY = 0x32A, kSpeed = 0x33C, kSpeedF = 0x370, kHealth = 0x3A1, kMode = 0x594,
                   kAction = 0x596, kArmour = 0x3EC, kColour = 0x4DC, kWeponSph = 0x127C, kWepon2Sph = 0x13A8,
                   kBokoId = 0x1560, kMantId = 0x1598;
// WW's actions (d_a_tn.cpp): the ones WW keeps (stunned, knocked down, recovering, weapon lost, dying, frozen, its
// cutscene entrance) run as WW's; the rest (patrol, chase, fight, guard) are TP's here
enum WwAction { kWwFight = 5, kWwDefence = 10, kWwHukki = 11, kWwWeponSearch = 12, kWwAiteMiru = 14, kWwDSit = 17,
                kWwDMahi = 18, kWwFail = 20, kWwYoganFail = 21, kWwDDozou = 23, kWwSDemo = 25 };
bool ww_keeps(int a) {
    return a == kWwHukki || a == kWwWeponSearch || a == kWwAiteMiru || a == kWwDSit || a == kWwDMahi || a == kWwFail ||
           a == kWwYoganFail || a == kWwDDozou || a == kWwSDemo;
}
// its pieces, tools/tp_monsters.py's order (tn_part_model_id, then sword_a)
enum Part { kArmL, kArmR, kChestB, kChestF, kHeadA, kHeadB, kShoulderL, kShoulderR, kWaistB, kWaistF, kWaistL, kWaistR,
            kShield, kMace, kSaya, kSwordB, kSwordA, kParts };
// setBreakPart's order (d_a_b_tn.cpp break_part_no)
constexpr int kBreakOrder[13] = {kShoulderL, kWaistR, kWaistB, kArmR, kWaistL, kArmL, kWaistF, kShoulderR, kHeadA, kHeadB, kShield, kChestB, kChestF};
constexpr int kHandJoint = 15, kShieldJoint = 9, kMidJoint = 2;
// its sounds (Z2SE_EN_TN_*, game/tp_sounds via tools/tp_sounds.py)
enum : uint32_t { kVKiaiOp = 0x70399, kVKiai, kVKiaiAge, kVDmg, kVDmg2, kVThrow, kVPullOut, kVKick, kVJump, kVZeizei, kVKurusi,
                  kVDead, kVDead2, kFootM1, kFootM2, kFootL, kSwdL, kSwdImpct, kAtkNoDmg, kPullOut, kSwdSTrst, kSwdS, kHiza,
                  kBrkArmor, kArmorBnd, kShieldBnd, kWeaponFly, kWeaponStop, kSayaBnd1, kSayaBnd2, kSwdDrop,
                  kVAtkSld = 0x703E5, kV2Swd1, kV2Swd2, kVThrust, kBodyFallM = 0x6002C, kMonsExplode = 0x6000A };

enum Act { kChase, kAttack, kAttackShield, kGuard, kDamage, kChange, kChaseL, kAttackL, kDamageL, kWw, kDying };

// a piece flying off (TP's mStates 2-5: thrown up and out, spinning, a bounce, then gone in a puff)
struct Fly { float pos[3], v[3], ang[3], spin[3], rot[9]; int bounces = 0, rest = 0; bool on = false, thrown = false; };

struct Dn {
    uint32_t actor = 0, id = 0;
    bool stalker = false, alive = true;
    tpm::Monster m;
    int phase = 1, act = kChase, mode = 0, timer3 = 30, next_break = 0, health = 0, dead = 0;
    uint8_t armour = 7;
    bool atk_b = false, at_was[2] = {};
    Fly fly[kParts];
    uint32_t boko = 0, mant = 0;  // its sword and cape actors (resolved each step, for the draw hooks)
};
std::vector<std::unique_ptr<Dn>> g_dns;
std::vector<uint32_t> g_seen;     // WW Darknuts drawn since the last step (the draw hook)
std::vector<uint32_t> g_hidden;   // the swords and capes not drawn
uint32_t g_stalker = 0, g_link = 0;
bool g_all = false;

Dn* find(uint32_t actor) {
    for (auto& d : g_dns)
        if (d->actor == actor && d->alive) return d.get();
    return nullptr;
}

float ssin(int16_t a) { return std::sin(a * (float)M_PI / 32768); }
int16_t atan2s(float y, float x) { return (int16_t)(int)(std::atan2(y, x) * 32768 / (float)M_PI); }
float rnd(float max) { return (float)rand() / RAND_MAX * max; }
float add_calc2(float v, float target, float max) { return v + std::clamp(target - v, -max, max); }
int16_t add_angle(int16_t a, int16_t target, int scale, int max) {
    return (int16_t)(a + std::clamp((int16_t)(target - a) / scale, -max, max));
}
void sound(Dn& d, uint32_t id, float seconds = 0) { tpm::sound(id, (float)d.m.x, (float)d.m.y + 150, (float)d.m.z, seconds); }
void play(Dn& d, const char* anim, float morf, bool once = true) { d.m.anim.play(anim, 1, once, morf); }
bool done(Dn& d) { return d.m.anim.finished(); }
int frame(Dn& d) { return (int)d.m.anim.cur.frame; }

// a piece's world matrix as it is drawn now (on its joint, or in the hand)
void piece_world(Dn& d, int i, float* out) {
    const auto* a = d.m.anim.a;
    const Piece& p = d.m.pieces[i];
    float jw[12];
    if (p.mode == Piece::kHeld) {
        float j[12];
        d.m.joint(p.held, j);
        mul34(j, p.offset, out);
    } else d.m.joint(a->parts[i].joint, jw), memcpy(out, jw, 48);
}

// off it flies (setBreakPart / calcSwordMoveA): up and out, spinning; `at` aims it (the thrown sword: at Link, fast)
void loosen(Dn& d, int i, const float* at = nullptr) {
    Piece& p = d.m.pieces[i];
    if (p.mode == Piece::kHidden || p.mode == Piece::kLoose) return;
    float w[12];
    piece_world(d, i, w);
    Fly& f = d.fly[i];
    f = Fly{};
    f.on = true, f.thrown = at != nullptr;
    for (int k = 0; k < 3; k++) f.pos[k] = w[k * 4 + 3], f.rot[k * 3] = w[k * 4], f.rot[k * 3 + 1] = w[k * 4 + 1], f.rot[k * 3 + 2] = w[k * 4 + 2];
    if (at) {  // at Link's middle, 80 a step
        const float dx = at[0] - f.pos[0], dy = at[1] + 200 - f.pos[1], dz = at[2] - f.pos[2], l = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-3f;
        f.v[0] = dx / l * 80, f.v[1] = dy / l * 80, f.v[2] = dz / l * 80, f.spin[1] = -0x1800 * (float)M_PI / 32768;
    } else {
        const int16_t dir = (int16_t)(int)rnd(65536);
        const float out = rnd(2) + 5;
        f.v[0] = out * ssin(dir), f.v[1] = rnd(5) + 18, f.v[2] = out * ssin((int16_t)(dir + 0x4000));
        for (float& s : f.spin) s = (rnd(2) - 1) * 0.3f;
    }
    p.mode = Piece::kLoose;
    memcpy(p.m, w, 48), memcpy(p.pm, w, 48);
}

// setBreakPart: armour off until `count` pieces are (the head's two together)
void break_to(Dn& d, int count) {
    count = std::min(count, 13);
    int off = 0;
    for (int i : kBreakOrder) off += d.m.pieces[i].mode == Piece::kLoose || d.m.pieces[i].mode == Piece::kHidden;
    for (int i : kBreakOrder) {
        if (off >= count) break;
        Piece& p = d.m.pieces[i];
        if (p.mode == Piece::kLoose || p.mode == Piece::kHidden) continue;
        float w[12];
        piece_world(d, i, w);
        const float at[3] = {w[3], w[7], w[11]};
        loosen(d, i, nullptr), off++;
        tpfx::spawn(0x87DE, at), tpfx::spawn(0x87DF, at), tpfx::spawn(0x87E0, at);
        sound(d, kBrkArmor);
        if (i == kHeadA && off < count) {  // (the zako's helmet comes off in one: head_b with head_a)
            loosen(d, kHeadB, nullptr), off++;
        }
    }
}

// the AT spheres: its sword's (four along the blade from the hand in TP, cSph x 4; two here, WW's weapon spheres) or
// its shield's (the shield bash, on the forearm)
void at_sphere(Cpu* c, Dn& d, int which, const float* pos, float r) {
    static const uint32_t buf = mem::host_alloc(16);
    const uint32_t sph = d.actor + (which ? kWepon2Sph : kWeponSph);
    for (int i = 0; i < 3; i++) st32(buf + 4 * i, u32_bits(pos[i]));
    guest_call(c, GC(d.at_was[which] ? 0x025167E4 : 0x025167C0), {sph, buf});  // dCcD_Sph::MoveCAt / StartCAt
    c->f[1].ps0 = r;
    guest_call(c, GC(0x02018C8C), {sph});                                      // SetR
    guest_call(c, GC(0x0200E240), {guest_call(c, GC(0x025200D4)) + 0x26A4, sph});  // dComIfG_Ccsp()->Set
    d.at_was[which] = true;
}
void sword_at(Cpu* c, Dn& d, float r) {
    float h[12];
    d.m.joint(kHandJoint, h);
    for (int k : {2, 4}) {  // the blade: 12, -6, 36 a step from the hand (tn's mSwordSphs)
        const float l[3] = {12.f * k, -6.f * k, 36.f * k};
        const float p[3] = {h[0] * l[0] + h[1] * l[1] + h[2] * l[2] + h[3], h[4] * l[0] + h[5] * l[1] + h[6] * l[2] + h[7],
                            h[8] * l[0] + h[9] * l[1] + h[10] * l[2] + h[11]};
        at_sphere(c, d, k == 4, p, r);
    }
}

// ---- its mind: daB_TN's (d_a_b_tn.cpp) for the zako, armoured (executeChaseH, executeAttackH, executeAttackShieldH,
// executeGuard, executeDamageH, checkStartAttackH, executeZakoChangeDemo) and not (executeChaseL, executeAttackL,
// executeDamageL), on the WW Darknut's body
void think(Cpu* c, Dn& d) {
    const uint32_t b = d.actor, pl = g_link;
    if (!pl) return;
    const float dx = (float)(ldf32(pl + kPos) - ldf32(b + kPos)), dz = (float)(ldf32(pl + kPos + 8) - ldf32(b + kPos + 8));
    const float dist = std::sqrt(dx * dx + dz * dz);
    const int16_t to_link = atan2s(dx, dz);
    int16_t angle = (int16_t)ld16(b + kAngleY), move = angle;
    float speed = (float)ldf32(b + kSpeedF);
    const int health = (int8_t)ld8(b + kHealth), ww = (int16_t)ld16(b + kAction);
    const uint8_t armour = ld8(b + kArmour);
    if (d.timer3 > 0) d.timer3--;
    bool sword = false, shield = false;
    float sword_r = 60;
    // what WW's damage check saw: a hit on the shield or armour (guarded), a hit that hurt, the armour coming off
    if (ww == kWwDefence && d.act != kGuard && d.act != kChange) d.act = kGuard, d.mode = 0;
    if (health < d.health && health > 0 && d.act != kChange)
        d.act = d.phase == 1 ? kDamage : kDamageL, d.mode = 0;
    if ((d.armour & 2) && !(armour & 2) && d.phase == 1) {  // the helmet off: TP's head pieces go
        for (int i : {kHeadA, kHeadB}) loosen(d, i);
        sound(d, kBrkArmor);
    }
    if ((d.armour & 4) && !(armour & 4)) loosen(d, kShield), sound(d, kBrkArmor);
    if ((d.armour & 1) && !(armour & 1) && d.phase == 1) d.act = kChange, d.mode = 0;
    d.armour = armour, d.health = health;
    const int f = frame(d);
    switch (d.act) {
    case kChase: {  // executeChaseH: walk at Link from afar (4.3), close in watching him (2.3), stand within 300
        if (dist >= 700) play(d, "tna_walk", 10, false), speed = 4.3f;
        else if (dist > 300) play(d, "tna_wait_b_1", 10, false), speed = 2.3f;
        else play(d, "tna_wait", 10, false), speed = 0;
        angle = add_angle(angle, to_link, 4, 0x400);
        // checkStartAttackH
        if (dist < 400 && std::abs((int16_t)(to_link - angle)) < 0x3000) {
            if (d.next_break >= 11) {
                if (d.timer3 > 40) d.timer3 = (int)rnd(20) + 15;
                if (d.timer3 == 0) d.timer3 = 150, d.act = kAttack, d.mode = 0;
            } else if (dist < 200) d.act = kAttackShield, d.mode = 0;
            else {
                if (dist < 250 && d.timer3 > 60) d.timer3 = (int)rnd(20) + 40;
                if (d.timer3 == 0) d.timer3 = 150, d.act = kAttack, d.mode = 0;
            }
        }
        break;
    }
    case kAttack:  // executeAttackH: a cut close in (atack_a), the overhead slam further out (atack_b)
        speed = 0;
        if (d.mode == 0) {
            d.atk_b = dist < 250 ? false : dist < 350 ? true : rand() % 2;
            play(d, d.atk_b ? "tna_atack_b" : "tna_atack_a", 3), d.mode = 1;
        }
        if (f < (d.atk_b ? 22 : 18)) angle = add_angle(angle, to_link, 8, 0x400);
        sword = d.atk_b ? f >= 22 && f <= 30 : f >= 18 && f <= 29, sword_r = d.atk_b ? 60 : 70;
        if (d.atk_b && f == 26) {  // the blade hits the ground: sparks
            float h[12];
            d.m.joint(kHandJoint, h);
            const float at[3] = {h[3], h[7], h[11]};
            for (uint32_t id = 0x87DA; id <= 0x87DD; id++) tpfx::spawn(id, at);
        }
        if (done(d)) d.act = kChase, d.mode = 0;
        break;
    case kAttackShield:  // executeAttackShieldH: a bash with the shield, then a cut
        speed = 0;
        if (d.mode == 0) play(d, "tna_atack_shield", 3), d.mode = 1;
        shield = f >= 5 && f <= 9;
        if (done(d)) d.act = kAttack, d.mode = 0;
        break;
    case kGuard:  // executeGuard: the hit on its shield
        speed = 0;
        if (d.mode == 0) play(d, "tna_guard_damage", 1), sound(d, kAtkNoDmg), d.mode = 1;
        if (done(d)) d.act = d.phase == 1 ? kChase : kChaseL, d.mode = 0;
        break;
    case kDamage:  // executeDamageH: reels, and another piece of armour comes off
        speed = 0;
        if (d.mode == 0) {
            play(d, rand() % 2 ? "tna_damage_l" : "tna_damage_r", 1), sound(d, kVDmg), d.mode = 1;
            break_to(d, ++d.next_break);
        }
        if (done(d) || f >= 35) d.act = kChase, d.mode = 0;
        break;
    case kChange:  // executeZakoChangeDemo: the rest of its armour off, a leap back, the sword thrown at Link, the
                   // second sword drawn
        switch (d.mode) {
        case 0:
            speed = 0, play(d, "tna_damage_last", 3), sound(d, kVDmg2), break_to(d, 13), d.mode = 1;
            break;
        case 1:
            angle = add_angle(angle, to_link, 8, 0x100);
            if (done(d)) play(d, "tnb_jump_b_1", 3), d.mode = 2;
            break;
        case 2:
            move = (int16_t)(angle + 0x8000);
            if (f == 4) speed = 45, st32(b + kSpeed + 4, u32_bits(5.f));
            if (done(d)) play(d, "tnb_jump_b_2", 5), d.mode = 3;
            break;
        case 3:
            move = (int16_t)(angle + 0x8000);
            angle = add_angle(angle, to_link, 8, 0x800);
            speed = std::max(0.f, speed - 9);
            if (done(d)) speed = 0, play(d, "tnb_throw", 3), d.mode = 4;
            break;
        case 4:
            if (f == 12) {
                const float at[3] = {(float)ldf32(pl + kPos), (float)ldf32(pl + kPos + 4), (float)ldf32(pl + kPos + 8)};
                loosen(d, kSwordA, at), sound(d, kWeaponFly, 1);
            }
            if (done(d)) play(d, "tnb_pull", 3), d.mode = 5;
            break;
        case 5:
            if (f == 20) {  // in its hand, as mtx_set's state 1 for the second sword
                Piece& p = d.m.pieces[kSwordB];
                p.mode = Piece::kHeld, p.held = kHandJoint;
                const float c = (float)M_PI / 180, ax = -168 * c, ay = -42.715f * c, az = 77.897f * c;
                const float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay), cz = std::cos(az), sz = std::sin(az);
                // trans(12.65, 13.47, 20.07) x ZXYrot: Rz * Rx * Ry
                const float rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1}, rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx}, ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
                float t[9], r[9];
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++) t[i * 3 + k] = rz[i * 3] * rx[k] + rz[i * 3 + 1] * rx[3 + k] + rz[i * 3 + 2] * rx[6 + k];
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++) r[i * 3 + k] = t[i * 3] * ry[k] + t[i * 3 + 1] * ry[3 + k] + t[i * 3 + 2] * ry[6 + k];
                const float tr[3] = {12.65f, 13.47f, 20.07f};
                for (int i = 0; i < 3; i++) p.offset[i * 4] = r[i * 3], p.offset[i * 4 + 1] = r[i * 3 + 1], p.offset[i * 4 + 2] = r[i * 3 + 2], p.offset[i * 4 + 3] = tr[i];
            }
            if (done(d)) d.phase = 2, d.act = kChaseL, d.mode = 0, d.timer3 = 30;
            break;
        }
        break;
    case kChaseL:  // executeChaseL: walk at Link (4.7), stand and face him close in
        if (dist > 350) play(d, "tnb_walk", 10, false), speed = 4.7f;
        else play(d, "tnb_wait", 10, false), speed = 0;
        angle = add_angle(angle, to_link, 4, 0x800);
        if (dist < 350 && d.timer3 == 0) d.act = kAttackL, d.mode = 0;
        break;
    case kAttackL:  // executeAttackL: the thrust (atack_a) close in, else the dashing cut (atack_b)
        if (d.mode == 0) {
            d.timer3 = 210, d.atk_b = dist >= 200;
            play(d, d.atk_b ? "tnb_atack_b" : "tnb_atack_a", 3), d.mode = 1;
        }
        speed = d.atk_b && f >= 10 && f < 20 ? 10.f : 0.f;
        if (f < (d.atk_b ? 13 : 7)) angle = add_angle(angle, to_link, 8, 0x800);
        sword = d.atk_b ? f >= 13 && f <= 38 : f >= 7 && f <= 14, sword_r = 45;
        if (done(d)) d.act = kChaseL, d.mode = 0;
        break;
    case kDamageL:  // executeDamageL
        speed = 0;
        if (d.mode == 0) play(d, rand() % 2 ? "tnb_damage_l" : "tnb_damage_r", 3), sound(d, kVDmg), d.mode = 1;
        if (done(d) || f >= 35) d.act = kChaseL, d.mode = 0;
        break;
    default: break;
    }
    // moved as daB_TN moves itself (speedF along its heading; a leap back away from it)
    st16(b + kAngleY, (uint16_t)angle);
    st16(b + kAction, (uint16_t)kWwFight);  // to WW's other code, a Darknut in a fight
    st32(b + kSpeedF, u32_bits(speed));
    st32(b + kSpeed, u32_bits(ssin(move) * speed)), st32(b + kSpeed + 8, u32_bits(ssin((int16_t)(move + 0x4000)) * speed));
    if (sword) sword_at(c, d, sword_r);
    else d.at_was[0] = d.at_was[1] = false;
    if (shield) {
        float s[12];
        d.m.joint(kShieldJoint, s);
        const float p[3] = {s[3], s[7], s[11]};
        at_sphere(c, d, 1, p, 70);
    }
}

// what it plays while WW's own action runs (stunned, down, weapon lost, its entrance, dying)
void ww_visual(Dn& d, int ww) {
    switch (ww) {
    case kWwFail: case kWwYoganFail:
        if (d.act != kDying) d.act = kDying, play(d, "tn2b_die", 3), sound(d, kVDead);
        break;
    case kWwSDemo: play(d, "tna_wait_op", 10, false); break;
    case kWwDDozou: d.m.anim.play("tna_wait_op", 0, false, 1); break;
    case kWwDSit: case kWwDMahi: case kWwHukki:
        if (d.act != kWw) play(d, d.phase == 1 ? "tna_damage_last" : "tnb_down", 3);
        break;
    default: play(d, d.phase == 1 ? "tna_wait" : "tnb_wait", 10, false); break;
    }
    if (d.act != kDying) d.act = kWw;
}

// sounds on its animations' frames (daB_TN's checkFrame calls)
void cues(Dn& d, int anim_was, float from, float to) {
    struct Cue { const char* anim; float frame; uint32_t id; };
    static const Cue kCues[] = {
        {"tna_walk", 23, kFootM1}, {"tna_walk", 0, kFootM2},
        {"tna_atack_a", 18, kSwdL}, {"tna_atack_a", 18, kVKiai}, {"tna_atack_a", 23, kFootL}, {"tna_atack_a", 72, kFootM1},
        {"tna_atack_b", 11, kFootM1}, {"tna_atack_b", 19, kVKiaiAge}, {"tna_atack_b", 22, kSwdL}, {"tna_atack_b", 23, kFootL},
        {"tna_atack_b", 26, kSwdImpct}, {"tna_atack_b", 66, kFootM2},
        {"tna_atack_shield", 4, kVAtkSld}, {"tna_atack_shield", 8, kFootL}, {"tna_atack_shield", 21, kFootM1},
        {"tna_damage_last", 3, kFootM1}, {"tna_damage_last", 15, kFootM2}, {"tna_damage_last", 26, kFootM1}, {"tna_damage_last", 37, kFootM2},
        {"tnb_jump_b_1", 6, kVJump}, {"tnb_jump_b_2", 0, kFootM1}, {"tnb_jump_b_2", 4, kFootM2},
        {"tnb_throw", 8, kVThrow}, {"tnb_pull", 10, kVPullOut}, {"tnb_pull", 20, kPullOut},
        {"tnb_walk", 16, kFootM1}, {"tnb_walk", 33, kFootM2},
        {"tnb_atack_a", 7, kSwdS}, {"tnb_atack_a", 8, kVThrust}, {"tnb_atack_b", 10, kV2Swd1}, {"tnb_atack_b", 13, kSwdL},
        {"tn2b_die", 33, kHiza}, {"tn2b_die", 68, kBodyFallM},
    };
    if (d.m.anim.cur.anim != anim_was) from = -1;
    for (const Cue& c : kCues)
        if (d.m.anim.is(c.anim) && (to >= from ? c.frame > from && c.frame <= to : c.frame > from || c.frame <= to))
            sound(d, c.id, c.id == kFootM1 || c.id == kFootM2 || c.id == kFootL ? 0.6f : 0);
}

// the pieces off it: flying, a bounce (a puff of dust), then gone in a little burst (createDisappear 3, as the zako's)
void fly(Dn& d, Cpu* c) {
    for (int i = 0; i < kParts; i++) {
        Fly& f = d.fly[i];
        Piece& p = d.m.pieces[i];
        if (!f.on || p.mode != Piece::kLoose) continue;
        f.v[1] = f.thrown ? std::max(-70.f, f.v[1] - 0.3f * 10) : std::max(-70.f, f.v[1] - 5);
        for (int k = 0; k < 3; k++) f.pos[k] += f.v[k], f.ang[k] += f.spin[k];
        // the ground under it (dGrass's checkGroundY 02549F4C, as the spawns use)
        static const uint32_t probe = mem::host_alloc(16);
        st32(probe, u32_bits(f.pos[0])), st32(probe + 4, u32_bits(f.pos[1] + 100)), st32(probe + 8, u32_bits(f.pos[2]));
        guest_call(c, GC(0x02549F4C), {probe});
        float ground = (float)c->f[1].ps0;
        if (ground == f.pos[1] + 100) ground = (float)d.m.y;
        if (f.pos[1] <= ground && f.v[1] < 0) {
            f.pos[1] = ground;
            const float at[3] = {f.pos[0], f.pos[1], f.pos[2]};
            if (f.thrown) {
                if (!f.bounces) tpm::sound(kWeaponStop, at[0], at[1], at[2]), f.bounces = 1;
                f.v[0] = f.v[1] = f.v[2] = 0, f.spin[0] = f.spin[1] = f.spin[2] = 0;
            } else if (f.bounces == 0) {  // one bounce
                const int16_t dir = (int16_t)(int)rnd(65536);
                f.v[0] = (rnd(2) + 5) * ssin(dir), f.v[1] = rnd(5) + 18, f.v[2] = (rnd(2) + 5) * ssin((int16_t)(dir + 0x4000));
                tpm::sound(i == kShield ? kShieldBnd : kArmorBnd, at[0], at[1], at[2]);
                tpfx::spawn(0x880B, at), f.bounces = 1;
            } else {
                f.v[0] = f.v[1] = f.v[2] = 0;
            }
        }
        if (f.thrown && !f.bounces && d.alive) at_sphere(c, d, 1, f.pos, 45);  // the thrown sword hurts (its AT, flying)
        if (f.bounces && ++f.rest > (f.thrown ? 90 : 20)) {  // gone in a little burst
            const float at[3] = {f.pos[0], f.pos[1] + 20, f.pos[2]};
            for (uint32_t id : {0x108u, 0x109u, 0x10Au, 0x10Bu, 0x10Cu}) tpfx::spawn(id, at, 0.3f);
            p.mode = Piece::kHidden, f.on = false;
            continue;
        }
        // its matrix: the spin about its own axes, at its place
        const float cx = std::cos(f.ang[0]), sx = std::sin(f.ang[0]), cy = std::cos(f.ang[1]), sy = std::sin(f.ang[1]),
                    cz = std::cos(f.ang[2]), sz = std::sin(f.ang[2]);
        const float r[9] = {cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz, cy * sz, sx * sy * sz + cx * cz,
                            cx * sy * sz - sx * cz, -sy, sx * cy, cx * cy};
        for (int row = 0; row < 3; row++) {
            for (int k = 0; k < 3; k++)
                p.m[row * 4 + k] = r[row * 3] * f.rot[k] + r[row * 3 + 1] * f.rot[3 + k] + r[row * 3 + 2] * f.rot[6 + k];
            p.m[row * 4 + 3] = f.pos[row];
        }
    }
}

// a WW Darknut, now TP's
Dn* adopt(uint32_t actor, bool stalker) {
    if (!tpm::asset(tpm::kDarknut)) return nullptr;
    auto d = std::make_unique<Dn>();
    d->actor = actor, d->id = ld32(actor + 4), d->stalker = stalker;
    tpm::Monster& m = d->m;
    m.begin(tpm::kDarknut);
    m.x = m.px = ldf32(actor + kPos), m.y = m.py = ldf32(actor + kPos + 4), m.z = m.pz = ldf32(actor + kPos + 8);
    m.yaw = m.pyaw = (int16_t)ld16(actor + kAngleY) * (float)M_PI / 32768;
    m.variant = ld8(actor + kColour) % 5;
    tpm::read_light(actor, m.light);
    m.pieces[kMace].mode = Piece::kHidden;
    m.pieces[kSwordA].mode = Piece::kHeld, m.pieces[kSwordA].held = kHandJoint;
    m.anim.play("tna_wait", 1, false, 1);
    d->health = (int8_t)ld8(actor + kHealth), d->armour = ld8(actor + kArmour);
    if (!(d->armour & 1)) {  // it has lost its armour already: TP's second phase
        d->phase = 2, d->act = kChaseL;
        for (int i = 0; i < 13; i++) m.pieces[kBreakOrder[i]].mode = Piece::kHidden;
        m.pieces[kSwordA].mode = Piece::kHidden;
    }
    tpm::add(&d->m);
    trace("darknut: %s, the Darknut at %08X", stalker ? "the stalker" : "one of the game's", actor);
    g_dns.push_back(std::move(d));
    return g_dns.back().get();
}

}  // namespace

bool available() { return tpm::asset(tpm::kDarknut) != nullptr; }
void set_stalker(uint32_t body) {
    if (body && body != g_stalker && !find(body)) adopt(body, true);
    g_stalker = body;
}
void replace_all(bool on) { g_all = on; }

bool hides_weapon(uint32_t actor) { return std::find(g_hidden.begin(), g_hidden.end(), actor) != g_hidden.end(); }

void step(Cpu* c, uint32_t link) {
    g_link = link;
    if (g_all)
        for (uint32_t a : g_seen)
            if (!find(a)) adopt(a, false);
    g_seen.clear();
    g_hidden.clear();
    for (auto& dp : g_dns) {
        Dn& d = *dp;
        tpm::Monster& m = d.m;
        if (!d.stalker && !g_all && d.alive) {  // the mod switched off: WW's own Darknut again
            d.alive = false, m.out = false, tpm::remove(&m);
            continue;
        }
        m.step_start();
        if (d.alive && guest_call(c, GC(0x025DE50C), {d.id}) != d.actor) {  // its body gone (fopAcM_SearchByID)
            d.alive = false;
            if (d.act == kDying) {  // its death: it bursts (createDisappear 10, type 0: the monsters' burst, its sound)
                float mid[12];
                m.joint(kMidJoint, mid);
                const float at[3] = {mid[3], mid[7], mid[11]};
                for (uint32_t id : {0x108u, 0x109u, 0x10Au, 0x10Bu, 0x10Cu}) tpfx::spawn(id, at, 1.f);
                tpm::sound(kMonsExplode, at[0], at[1], at[2]);
                m.fade = m.pfade = m.fade_to = 0;
            } else m.fade_to = 0;
        }
        if (d.alive) {
            const uint32_t a = d.actor;
            m.x = ldf32(a + kPos), m.y = ldf32(a + kPos + 4), m.z = ldf32(a + kPos + 8);
            float yaw = (int16_t)ld16(a + kAngleY) * (float)M_PI / 32768;
            while (yaw - m.pyaw > M_PI) yaw -= 2 * M_PI;
            while (yaw - m.pyaw < -M_PI) yaw += 2 * M_PI;
            m.yaw = yaw;
            tpm::read_light(a, m.light);
            const int ww = (int16_t)ld16(a + kAction);
            if (ww_keeps(ww) || (int8_t)ld8(a + kHealth) <= 0) ww_visual(d, (int8_t)ld8(a + kHealth) <= 0 ? kWwFail : ww);
            else if (d.act == kWw) d.act = d.phase == 1 ? kChase : kChaseL, d.mode = 0;
            for (uint32_t id : {ld32(a + kBokoId), ld32(a + kMantId)})
                if (id != 0xFFFFFFFF && id)
                    if (const uint32_t w = guest_call(c, GC(0x025DE50C), {id})) g_hidden.push_back(w);
        }
        const int anim_was = m.anim.cur.anim;
        const float frame_was = m.anim.cur.frame;
        m.anim.advance();
        cues(d, anim_was, frame_was, m.anim.cur.frame);
        fly(d, c);
        m.fade = m.fade_to > m.fade ? std::min(m.fade_to, m.fade + 1 / 30.f) : std::max(m.fade_to, m.fade - 1 / 30.f);
        bool flying = false;
        for (const Fly& f : d.fly) flying |= f.on;
        if (!d.alive && m.fade <= 0 && !flying) m.out = false, tpm::remove(&m);
    }
    std::erase_if(g_dns, [](const std::unique_ptr<Dn>& d) { return !d->m.out; });
}

}  // namespace mods::darknut

// daTn_Draw (024BA238): a Darknut drawn as TP's is not drawn as WW's (its body, shield and armour; its sword and cape
// are daBoko's and daMant's, hidden by their own draw hooks)
extern "C" void f_024BA238_orig(Cpu* c);
extern "C" void hook_024BA238(Cpu* c) {
    using namespace mods::darknut;
    const uint32_t a = c->r[3];
    if (find(a) || (g_all && available())) {
        if (std::find(g_seen.begin(), g_seen.end(), a) == g_seen.end()) g_seen.push_back(a);
        c->r[3] = 1;
        return;
    }
    f_024BA238_orig(c);
}
// Tn_move (024C3E64): WW's mind for a Darknut, replaced by daB_TN's for one drawn as TP's, but for what WW keeps
extern "C" void f_024C3E64_orig(Cpu* c);
extern "C" void hook_024C3E64(Cpu* c) {
    using namespace mods::darknut;
    Dn* d = find(c->r[3]);
    if (d && (int8_t)ld8(c->r[3] + kHealth) > 0 && !ww_keeps((int16_t)ld16(c->r[3] + kAction)) && d->act != kDying) {
        if (d->act == kWw) d->act = d->phase == 1 ? kChase : kChaseL, d->mode = 0;
        think(c, *d);
        return;
    }
    f_024C3E64_orig(c);
}
// daMant draw (021BC2C8): a Darknut's cape, not drawn (TP's has none)
extern "C" void f_021BC2C8_orig(Cpu* c);
extern "C" void hook_021BC2C8(Cpu* c) {
    if (mods::darknut::hides_weapon(c->r[3])) { c->r[3] = 1; return; }
    f_021BC2C8_orig(c);
}

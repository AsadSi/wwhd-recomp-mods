// More built-in mods (Gameplay menu, all off by default), several ported from the Twilight Princess HD
// recomp: damage and rupee multipliers, time of day, a turbo key, and two that spawn
// the game's own monsters around Link: Night of the dead (ReDeads or Stalfos on land at night) and
// Haunted sea (Kargarocs around Link on the open sea).
//
// They run once per full logic step, after the actors' execute (interp.cpp hook_025DE788), and act on what
// changed in that step, so the game's own rules (maxima, wallet, death) still apply. Offsets (WWHD):
// - save data dSv_player_c (cheats.cpp): max life +0, life +2 (quarter hearts), rupees +4, wallet +0x12
//   (200, 1000, 5000); status B's time of day, a float in degrees (15 an hour from midnight) at +0x24.
// - fopAc_ac_c: current.pos +0x314, room +0x326, shape_angle.y +0x32A.
// Not ported (tried, no effect in WWHD): TP's moon jump (pos.y/speed.y) and Link's size (J3DModel base scale +0x14).
// - fopAcM_create (025D5834): procName, parameter, pos, room, angle, scale, subtype, createFunc -> process id;
//   fpcEx_SearchByID (025DE50C): id -> the process while it exists. Process names and subtypes are the
//   game's own object name table (101EBFDC: name[8], s16 proc, s8 subtype, u8 gba).
#include "extras.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "guest_addr.h"
#include "mods.h"
#include "../gx2/gx2.h"
#include "../input.h"
#include "../platform/keycodes.h"
#include "runtime.h"

namespace mods::extras {
namespace {
enum Id { kDamage, kRupees, kTime, kTurbo, kNight, kSea, kCount };
std::atomic<bool> g_on[kCount];
std::atomic<int> g_choice[kCount];

const uint32_t kPlay = GD(0x1046F0B0);
const uint32_t kLinkPtr = kPlay + 0x5B34;  // daPy_lk_c
const uint32_t kStage = kPlay + 0x5134;    // the current stage's name
constexpr uint32_t kPos = 0x314, kRoom = 0x326, kAngleY = 0x32A;

uint32_t save() { return ld32(GD(0x101F84DC)) + 0x20; }
bool in_mem2(uint32_t a) { return a >= 0x10000000 && a < 0x50000000; }
uint32_t u32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// a monster from the object name table, and the parameter it is created with (0xFFFFFFFF: the defaults)
struct Monster { int16_t proc; int8_t subtype; uint32_t param; };
constexpr Monster kRedead{0xE0, 0, 0xFFFFFFFF}, kStalfos{0xBE, -1, 0xFFFFFFFF}, kKargaroc{0xB5, -1, 0};

// the monsters a spawner has out, by process id; forgotten when they die or the stage changes
struct Spawner {
    std::vector<uint32_t> ids;
    double next = 0;  // game time of the next spawn
    void prune(Cpu* c) {
        std::erase_if(ids, [c](uint32_t id) { return !guest_call(c, GC(0x025DE50C), {id}); });
    }
};
Spawner g_night, g_sea;

// spawns `m` at `dist` from Link, `behind` him (out of the camera's view) or anywhere around him
void spawn(Cpu* c, Spawner& s, Monster m, uint32_t link, float dist, bool behind, float dy) {
    static const uint32_t buf = mem::host_alloc(0x20);  // pos (cXyz) at +0, angle (csXyz) at +0x10
    const double yaw = (int16_t)ld16(link + kAngleY) * M_PI / 32768;
    const double a = behind ? yaw + M_PI + (rand() / (double)RAND_MAX - 0.5) * 1.6 : rand() / (double)RAND_MAX * 2 * M_PI;
    const float x = (float)ldf32(link + kPos) + (float)(sin(a) * dist), z = (float)ldf32(link + kPos + 8) + (float)(cos(a) * dist);
    st32(buf, u32_bits(x));
    st32(buf + 4, u32_bits((float)ldf32(link + kPos + 4) + dy));
    st32(buf + 8, u32_bits(z));
    st16(buf + 0x10, 0);
    st16(buf + 0x12, (uint16_t)(int16_t)(atan2(-sin(a), -cos(a)) * 32768 / M_PI));  // facing Link
    st16(buf + 0x14, 0);
    const uint32_t id = guest_call(c, GC(0x025D5834), {(uint32_t)m.proc, m.param, buf, (uint32_t)(int8_t)ld8(link + kRoom), buf + 0x10, 0,
                                                       (uint32_t)(int32_t)m.subtype, 0});
    trace("extras: spawned proc %#x id %#x at %.0f %.0f %.0f", m.proc, id, x, ldf32(buf + 4), z);
    if (id != 0xFFFFFFFF) s.ids.push_back(id);
}
}  // namespace

// ---- switches ----
template <int I> bool on() { return g_on[I].load(std::memory_order_relaxed); }
template <int I> void set(bool v) { g_on[I] = v; }
bool (*const kOn[kCount])() = {on<0>, on<1>, on<2>, on<3>, on<4>, on<5>};
void (*const kSet[kCount])(bool) = {set<0>, set<1>, set<2>, set<3>, set<4>, set<5>};

const std::vector<Mod>& catalogue() {
    static const std::vector<Mod> mods = {
        {"damage", "Damage multiplier", "Gameplay", "Link takes more or less damage than the game deals.",
         {"x2", "x4", "One hit kills", "x0.5", "None"}},
        {"rupees", "Rupee multiplier", "Gameplay", "Rupees Link picks up or is given count more (up to what the wallet holds).",
         {"x2", "x3", "x5", "x10"}},
        {"time", "Time of day", "Gameplay", "Stops the clock, or keeps it at an hour of your choice.",
         {"Stopped", "Morning (7:00)", "Noon", "Evening (18:30)", "Night (23:00)"}},
        {"turbo", "Turbo key", "Gameplay", "Hold T on the keyboard to run the game as fast as this Mac can (sailing, text, cutscenes).", {}},
        {"night-dead", "Night of the dead (experimental)", "Spooky",
         "After dark the dead rise on land: ReDeads or Stalfos close in from behind Link, a few at a time. "
         "Pairs well with Time of day: Night. Experimental: the picture can stop after a spawn (the game keeps running).",
         {"Sometimes", "Often", "Relentless"}},
        {"haunted-sea", "Haunted sea (experimental)", "Spooky",
         "Out on the open sea (on the boat or swimming) Kargarocs come for Link, day and night (more at night). "
         "Experimental: the picture can stop after a spawn (the game keeps running).",
         {"Sometimes", "Often", "Relentless"}},
    };
    return mods;
}
const Mod* find(const std::string& id) {
    for (const auto& m : catalogue()) if (m.id == id) return &m;
    return nullptr;
}
static int index(const std::string& id) {
    const auto& c = catalogue();
    for (size_t i = 0; i < c.size(); i++) if (c[i].id == id) return (int)i;
    return -1;
}
bool (*enabled_fn(const std::string& id))() { int i = index(id); return i < 0 ? nullptr : kOn[i]; }
void (*apply_fn(const std::string& id))(bool) { int i = index(id); return i < 0 ? nullptr : kSet[i]; }
int choice(const std::string& id) { int i = index(id); return i < 0 ? 0 : g_choice[i].load(std::memory_order_relaxed); }
void set_choice(const std::string& id, int c) {
    const int i = index(id);
    if (i >= 0 && c >= 0 && c < (int)std::max<size_t>(1, catalogue()[i].choices.size())) g_choice[i] = c;
}

// ---- the game thread, once per full logic step ----
void step(Cpu* c) {
    bool keys[256];
    input::held_keys(keys);
    static bool turbo = false;
    if (const bool t = on<kTurbo>() && keys[kVK_ANSI_T]; t != turbo) gx2::set_uncapped(turbo = t);

    static uint32_t last_link = 0;
    static uint16_t life = 0, rupees = 0;
    const uint32_t link = ld32(kLinkPtr);
    const bool fresh = link != last_link;  // a file loaded, a stage entered: start over
    last_link = link;
    std::string stage((const char*)mem::ptr(kStage), strnlen((const char*)mem::ptr(kStage), 8));
    if (!in_mem2(link) || stage.empty() || stage == "sea_T" || stage == "Name" || stage == "ENDumi") return;  // title, file select
    const uint32_t sv = save();
    if (fresh) g_night = Spawner{}, g_sea = Spawner{};

    const uint16_t max_life = ld16(sv), cur = ld16(sv + 2);
    if (on<kDamage>() && !fresh && cur < life) {  // also a hit that killed: None and x0.5 can save Link
        static const double kRate[] = {2, 4, 1e9, 0.5, 0};
        const double rate = kRate[g_choice[kDamage].load()];
        const long v = std::lround(life - (life - cur) * rate);
        st16(sv + 2, (uint16_t)std::clamp<long>(rate > 0 && rate < 1 && v == life ? life - 1 : v, 0, max_life));
    }
    life = ld16(sv + 2);

    const uint16_t money = ld16(sv + 4);
    if (on<kRupees>() && !fresh && money > rupees) {
        static const int kRate[] = {2, 3, 5, 10}, kWallet[] = {200, 1000, 5000};
        const int wallet = kWallet[std::min<int>(ld8(sv + 0x12), 2)];
        st16(sv + 4, (uint16_t)std::min<int>(rupees + (money - rupees) * kRate[g_choice[kRupees].load()], wallet));
    }
    rupees = ld16(sv + 4);

    // time of day: held where it stood, or kept at the chosen hour
    static float held = -1;
    if (!on<kTime>() || fresh) held = -1;
    if (const int t = g_choice[kTime].load(); on<kTime>() && t > 0) {
        static const float kHour[] = {0, 7, 12, 18.5f, 23};
        st32(sv + 0x24, u32_bits(kHour[t] * 15));
    } else if (on<kTime>()) {
        if (held < 0) held = (float)ldf32(sv + 0x24);
        st32(sv + 0x24, u32_bits(held));
    }

    const double now = game_time(), hour = ldf32(sv + 0x24) / 15;
    // on the water: riding the boat (daPy_lk_c::mCurProc +0x65F0, SHIP_READY..SHIP_RESTART but not SHIP_GET_OFF) or
    // swimming (mModeFlg +0x6A70, ModeFlg_SWIM); the sea stage also holds the islands
    const int proc = (int)ld32(link + 0x65F0);
    const bool water = (proc >= 0x86 && proc <= 0x91 && proc != 0x90) || (ld32(link + 0x6A70) & 0x40000);
    const bool night = hour >= 19 || hour < 5, sea = stage == "sea" && water;
    static const double kEvery[] = {25, 12, 5};  // seconds between spawns: Sometimes, Often, Relentless
    static const size_t kMax[] = {2, 4, 7};      // monsters out at once
    // Night of the dead: on land at night (the sea stage's islands count)
    if (on<kNight>() && night && !water && !fresh) {
        const int level = g_choice[kNight].load();
        g_night.prune(c);
        // one kind a visit: ReDeads mixed with Stalfos overflowed the game's 4 MB per-frame command list (gx2 "display
        // list overflow", begun at 027C0768; the picture stops). Open: it also happened once with ReDeads alone (1 run
        // in 3), probably a spawn in the water. A Poe spawned this way halts the game (sead assert).
        static Monster kind = kRedead;
        if (g_night.ids.empty() && g_night.next == 0) kind = rand() % 3 ? kRedead : kStalfos;
        if (now >= g_night.next && g_night.ids.size() < kMax[level]) {
            spawn(c, g_night, kind, link, 450 + rand() % 350, true, 30);
            g_night.next = now + kEvery[level] * (0.6 + rand() / (double)RAND_MAX * 0.8);
        }
    }
    // Haunted sea: on the open sea, more often at night
    if (on<kSea>() && sea && !fresh) {
        const int level = g_choice[kSea].load();
        g_sea.prune(c);
        if (now >= g_sea.next && g_sea.ids.size() < kMax[level] + (night ? 2 : 0)) {
            // Kargarocs only for now: a Gyorg or Seahat spawned this way (parameter 0 or 0xFFFFFFFF) makes a frame overflow
            // the game's 4 MB command list (gx2 "display list overflow", begun at 027C0768) and the picture stops
            spawn(c, g_sea, kKargaroc, link, 900 + rand() % 600, false, 300);
            g_sea.next = now + kEvery[level] * (night ? 0.6 : 1) * (0.6 + rand() / (double)RAND_MAX * 0.8);
        }
    }
}
}  // namespace mods::extras

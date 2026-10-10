// More built-in mods (Gameplay menu, all off by default), several ported from the Twilight Princess HD
// recomp: damage and rupee multipliers, time of day, a turbo key; stormy weather and thick fog; and two that spawn
// the game's own monsters around Link: Night of the dead (ReDeads, Stalfos and Poes on land at night) and
// Haunted sea (Gyorgs, Seahats, giant Seahats, Kargarocs and, at night, Poes around Link on the open sea).
// The spooky ones are events now and then, by their How often setting (the director, "spooky events" below).
//
// They run once per full logic step, after the actors' execute (interp.cpp hook_025DE788), and act on what
// changed in that step, so the game's own rules (maxima, wallet, death) still apply. Offsets (WWHD):
// - save data dSv_player_c (cheats.cpp): max life +0, life +2 (quarter hearts), rupees +4, wallet +0x12
//   (200, 1000, 5000); status B's time of day, a float in degrees (15 an hour from midnight) at +0x24.
// - fopAc_ac_c: current.pos +0x314, room +0x326, shape_angle.y +0x32A.
// Not ported (tried, no effect in WWHD): TP's moon jump (current.pos.y +0x318, even +300 at once, and speed.y +0x340
// are both put back the next step while Link stands. Lifting every copy of his position (+0x2EC home, +0x300 old,
// +0x314, +0x84C, +0x68B0, +0x7350) by 300 and switching him to falling (procFall_init 023F6564) or to the game's own
// auto jump (procAutoJump_init 023F7958) with speed.y held at 20: his own execute still puts him on the ground below
// the next step (proc 0x25, landing). The landing check inside his execute would have to be hooked.)
// and Link's size (mpCLModel +0x448: +0x14 and its 1,1,1 at +0xBC).
// - fopAcM_create (025D5834): procName, parameter, pos, room, angle, scale, subtype, createFunc -> process id;
//   fpcEx_SearchByID (025DE50C): id -> the process while it exists. Process names and subtypes are the
//   game's own object name table (101EBFDC: name[8], s16 proc, s8 subtype, u8 gba).
#include "extras.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "guest_addr.h"
#include "mods.h"
#include "../gx2/gx2.h"
#include "../input.h"
#include "../platform/keycodes.h"
#include "runtime.h"

namespace mods::extras {
namespace {
enum Id { kDamage, kRupees, kTime, kTurbo, kNight, kSea, kStorm, kFog, kSwim, kGiants, kPhantom, kBigShip, kAudio, kDeep, kLeviathan, kMolgera, kStalker, kCount };  // the catalogue's order
std::atomic<bool> g_on[kCount];
std::atomic<int> g_choice[kCount];
std::atomic<int> g_often[kCount] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // How often: Now and then
static_assert(kCount == 17, "one How often per mod above");

const uint32_t kPlay = GD(0x1046F0B0);
const uint32_t kLinkPtr = kPlay + 0x5B34;  // daPy_lk_c
const uint32_t kStage = kPlay + 0x5134;    // the current stage's name
constexpr uint32_t kPos = 0x314, kRoom = 0x326, kAngleY = 0x32A;

uint32_t save() { return ld32(GD(0x101F84DC)) + 0x20; }
bool in_mem2(uint32_t a) { return a >= 0x10000000 && a < 0x50000000; }
uint32_t u32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// a monster from the object name table, and the parameter it is created with (0xFFFFFFFF: the defaults)
struct Monster { int16_t proc; int8_t subtype; uint32_t param; int16_t angle_x = 0; };  // some read a second parameter from angle.x
constexpr Monster kRedead{0xE0, 0, 0xFFFFFFFF}, kStalfos{0xBE, -1, 0xFFFFFFFF}, kPoe{0xD4, -1, 0xFFFFFFFF},
                  kGyorg{0xE4, -1, 0xFFFFFFFF}, kSeahat{0xD1, -1, 0xFFFFFF01}, kKargaroc{0xB5, -1, 0},
                  // a cannon boat (daOship): sub mode 0 (no path), no Triforce / salvage / switch conditions, switch B none
                  kOship{0xB3, -1, 0x00FFFF00, 0x00FF},
                  // the Ghost Ship (daGhostship): no path; its moon phase (low byte) is tonight's when spawned (see below)
                  kAyush{0xB2, -1, 0x00FF0000},
                  // Tetra's pirate ship (daObjPirateship): type 3 (no pirates, no scene), no path; a moving BG with a walkable deck
                  kPirates{0x39, -1, 0x03FFFF00},
                  // Phantom Ganon (daFganon): type 3 (the Ganon's Tower fight) with no switch; he waits 10000 above, unseen,
                  // until Link comes within byte 1 x 10 of where he was made (2550), then descends and fights (flies, fires,
                  // spin attacks, and makes his own short-lived copies). HD fganon_class: mAction +0x5BA, mMode +0x5BC,
                  // materialized +0x8C1 (GameCube + 0x234 from there; the first fields are + 0x11C)
                  kFganon{0xF1, -1, 0x00FFFF03};

// the monsters a spawner has out, by process id; forgotten when they die or the stage changes
struct Spawner {
    std::vector<uint32_t> ids;
    double next = 0;  // game time of the next spawn
    void prune(Cpu* c) {
        std::erase_if(ids, [c](uint32_t id) { return !guest_call(c, GC(0x025DE50C), {id}); });
    }
};
Spawner g_night, g_sea;
constexpr Monster kGyCtrl{0xE5, -1, 0x7F00FF01};
// one actor kept out at a time; a spawn takes a few steps to come to life, so it is not retried before `next`
struct Single {
    uint32_t id = 0xFFFFFFFF;
    double next = 0;
    uint32_t actor(Cpu* c) { return id == 0xFFFFFFFF ? 0 : guest_call(c, GC(0x025DE50C), {id}); }
    bool wanted(Cpu* c, double now) { return !actor(c) && now >= next; }
};
Single g_ghost, g_bigship, g_phantom, g_leviathan, g_molgera;
uint32_t g_leviathan_actor = 0, g_molgera_actor = 0;  // for the hooks below, which must not call into the game
int g_molgera_dives = 0;  // times Molgera has dived and come up ahead of the boat again this event
// ph_class is the Peahat and the Seahat: the parameter's low byte picks it (0 or 0xFF a Peahat, else a Seahat, which
// loads its own archive). Giant Seahats: the class ignores the actor's scale; its two J3DModels (body +0x12C4,
// propeller +0x12CC) are drawn at their base scale (J3DModel +0xBC in HD), which nothing else sets, so it is set there
std::vector<std::pair<uint32_t, float>> g_giant_ids;  // and the scale
constexpr float kGiant = 1.5f, kColossus = 3.0f;
// bigger hits: the collider sizes Gyorgs (spheres) and Seahats (cylinders) set each step are doubled while the
// collider lies inside one of them (sph_set_r / cyl_setR / cyl_setH hooks below)
struct BigHit { uint32_t lo, hi; float scale; };  // [actor, actor + its size)
std::vector<BigHit> g_big_hits;
constexpr float kHitScale = 2.0f;

// spawns `m` at `dist` from Link, `dy` above him; a
// monster `on_ground` only where the game's ground check (dGrass's checkGroundY, 02549F4C) finds land within 250 of
// Link's height and above the sea (y 0), never over water or off a cliff (6 tries, the last 3 all around him, else
// not this time)
// `side`: 0 anywhere around him, 1 behind (out of the camera's view), -1 ahead
uint32_t spawn(Cpu* c, Spawner& s, Monster m, uint32_t link, float dist, int side, float dy, bool on_ground, uint32_t parent = 0) {
    static const bool seeded = (srand((unsigned)time(nullptr)), true);
    (void)seeded;
    static const uint32_t buf = mem::host_alloc(0x20);  // pos (cXyz) at +0, angle (csXyz) at +0x10
    const float ly = (float)ldf32(link + kPos + 4);
    for (int attempt = 0; attempt < 6; attempt++) {
        const double yaw = (int16_t)ld16(link + kAngleY) * M_PI / 32768;
        const double a = side && attempt < 3 ? yaw + (side > 0 ? M_PI : 0) + (rand() / (double)RAND_MAX - 0.5) * 1.6 : rand() / (double)RAND_MAX * 2 * M_PI;
        const float x = (float)ldf32(link + kPos) + (float)(sin(a) * dist), z = (float)ldf32(link + kPos + 8) + (float)(cos(a) * dist);
        st32(buf, u32_bits(x));
        st32(buf + 4, u32_bits(ly + dy));
        st32(buf + 8, u32_bits(z));
        if (on_ground) {
            st32(buf + 4, u32_bits(ly + 250));  // look down from above Link's height
            guest_call(c, GC(0x02549F4C), {buf});
            const float ground = (float)c->f[1].ps0;
            if (ground == ly + 250 || ground < 5 || std::fabs(ground - ly) > 250) continue;  // nothing, water or a drop
            st32(buf + 4, u32_bits(ground + 5));
        }
        st16(buf + 0x10, (uint16_t)m.angle_x);
        st16(buf + 0x12, (uint16_t)(int16_t)(atan2(-sin(a), -cos(a)) * 32768 / M_PI));  // facing Link
        st16(buf + 0x14, 0);
        // created in Link's process layer (base_process_class::mLyTg.mpLayer, +0x2C), as an actor's own spawns are: here,
        // after the executor, the current layer is the root one, and an actor made there was drawn outside the play
        // scene's draw buffers (a J3D draw list looped through the grass packet the first time it came into view)
        const uint32_t layer = guest_call(c, GC(0x025DED64));  // fpcLy_CurrentLayer
        guest_call(c, GC(0x025DEAB4), {ld32(link + 0x2C)});    // fpcLy_SetCurrentLayer
        const uint32_t room = (uint32_t)(int8_t)ld8(link + kRoom), subtype = (uint32_t)(int32_t)m.subtype;
        // fopAcM_createChild's 9th argument, createFunc, is on the stack (caller's sp + 8; guest_call's frame is 0x40)
        if (parent) st32(c->r[1] - 0x40 + 8, 0);
        const uint32_t id = parent ? guest_call(c, GC(0x025D5A20), {(uint32_t)m.proc, parent, m.param, buf, room, buf + 0x10, 0, subtype})
                                   : guest_call(c, GC(0x025D5834), {(uint32_t)m.proc, m.param, buf, room, buf + 0x10, 0, subtype, 0});
        guest_call(c, GC(0x025DEAB4), {layer});
        trace("extras: spawned proc %#x id %#x at %.0f %.0f %.0f (%zu out)", m.proc, id, x, ldf32(buf + 4), z, s.ids.size());
        if (id != 0xFFFFFFFF) s.ids.push_back(id);
        return id;
    }
    trace("extras: no ground for proc %#x near Link", m.proc);
    return 0xFFFFFFFF;
}

// Gyorgs: the game makes them only once Link has the Boomerang (daGy_c::_create and daGy_Ctrl_c::_create check
// dComIfGs_checkGetItem(dItemNo_BOOMERANG_e) and fail otherwise), and a Gyorg only as the child of a GyCtrl (proc 0xE5,
// it counts its Gyorgs at +0x438). Haunted sea makes one quiet GyCtrl near Link (type 1, no Gyorgs of its own, waiting
// for switch 0x7F, which nothing on the sea sets: with 0xFF it would go on to its own modes and, having no Gyorgs of
// its own, delete itself; switch numbers from 0x80 halt the game, d_save.cpp "i_no < SWITCH_MAX") and its Gyorgs as that controller's children; the check passes for those processes only
// (site hooks below, by process id, base_process_class +4).
std::vector<uint32_t> g_gyorg_ids;  // the controller and Gyorgs made here
uint32_t g_gyctrl = 0xFFFFFFFF;

// Boss effects at sea. Phantom Ganon's (energy and dark balls, dark shots, smoke, light-arrow hits) live in Ganon's
// Tower's scene particle bank, Pscene080 (all 22 he uses); Molgera's (sand bursts) in the Wind Temple boss room's,
// Pscene071. The sea's rooms load their own scene bank into JPAEmitterManager slot 1 (dPa_control_c::createRoomScene
// 025A7C30), which lacks them, so they go into slots 2 and 3 (the manager has 8, the game uses 0 and 1) the way
// createRoomScene makes a bank: a solid heap from the game heap, a JPAResourceManager on it (02827000) over the bank,
// an embedded file of Particle.bfres (getArchiveFile 026124B0; its embedded-file dictionary at +0x4C, looked up by
// 027DFA24). Scene-bank effects slot 1 lacks are made from slot 2 or 3 (createSimpleEmitterID hook, true60_link.cpp);
// dPa_control_c::removeScene (hook below) frees them with the stage.
uint32_t g_bank_heap[4];  // by slot, 2 and 3
uint32_t emitter_manager() { return ld32(GD(0x1047B2D4)); }  // its resource banks at +0x30
void load_scene_bank(Cpu* c, const char* name, int slot) {
    const uint32_t mgr = emitter_manager(), control = ld32(kPlay + 0x5AB0);  // dPa_control_c
    if (g_bank_heap[slot] || !in_mem2(mgr) || !in_mem2(control) || ld32(mgr + 0x30 + 4 * slot)) return;
    // the archive's and the file's names as the game passes them (const string objects: the text, then the class's
    // vtable, 100534B4), and the bank's name at +0x20
    static const uint32_t buf = [] {
        const uint32_t b = mem::host_alloc(0x40);
        memcpy(mem::ptr(b), "Particle\0\0\0\0\0\0\0\0Particle.bfres", 0x1F);
        st32(b + 0x30, b), st32(b + 0x34, GD(0x100534B4));
        st32(b + 0x38, b + 0x10), st32(b + 0x3C, GD(0x100534B4));
        return b;
    }();
    snprintf((char*)mem::ptr(buf + 0x20), 0x10, "%s", name);
    const uint32_t bfres = guest_call(c, GC(0x026124B0), {ld32(GD(0x101F4F7C)), buf + 0x30, buf + 0x38, 0});
    if (!bfres || !ld32(bfres + 0x4C)) return;
    const uint32_t file = guest_call(c, GC(0x027DFA24), {bfres + 0x4C + ld32(bfres + 0x4C), buf + 0x20});
    if (!file || !ld32(file)) return;
    const uint32_t heap = guest_call(c, GC(0x025E3528), {0, 0});  // mDoExt_createSolidHeapFromGame: all that is free
    if (!heap) return;
    const uint32_t res = guest_call(c, GC(0x0273B050), {0xC, heap, 4});  // operator new on that heap
    if (res) guest_call(c, GC(0x02827000), {res, file + ld32(file), heap, ld32(control + 0x415C)});
    guest_call(c, GC(0x025E3678), {heap});  // mDoExt_adjustSolidHeap: back down to what the bank took
    if (!res) { guest_call(c, GC(0x025E3868), {heap}); return; }
    st32(mgr + 0x30 + 4 * slot, res);
    g_bank_heap[slot] = heap;
    trace("extras: %s loaded into particle bank %d", name, slot);
}
}  // namespace

// ---- switches ----
template <int I> bool on() { return g_on[I].load(std::memory_order_relaxed); }
template <int I> void set(bool v) { g_on[I] = v; }
template <size_t... I> constexpr auto switches(std::index_sequence<I...>) {
    return std::pair{std::array<bool (*)(), kCount>{on<I>...}, std::array<void (*)(bool), kCount>{set<I>...}};
}
constexpr auto kSwitches = switches(std::make_index_sequence<kCount>());
constexpr auto& kOn = kSwitches.first;
constexpr auto& kSet = kSwitches.second;

const std::vector<Mod>& catalogue() {
    static const std::vector<Mod> mods = {
        {"damage", "Damage multiplier", "Gameplay", "Link takes more or less damage than the game deals.",
         {"x2", "x4", "One hit kills", "x0.5", "None"}},
        {"rupees", "Rupee multiplier", "Gameplay", "Rupees Link picks up or is given count more (up to what the wallet holds).",
         {"x2", "x3", "x5", "x10"}},
        {"time", "Time of day", "Gameplay", "Stops the clock, or keeps it at an hour of your choice.",
         {"Stopped", "Morning (7:00)", "Noon", "Evening (18:30)", "Night (23:00)"}},
        {"turbo", "Turbo key", "Gameplay", "Hold T on the keyboard to run the game as fast as this Mac can (sailing, text, cutscenes).", {}},
        {"night-dead", "Night of the dead", "Spooky",
         "On some nights the dead rise on land: ReDeads, Stalfos and Poes close in from behind Link, a few at a time, "
         "until dawn. Pairs well with Stormy weather.",
         {"Sometimes", "Often", "Relentless"}, true},
        {"haunted-sea", "Haunted sea", "Spooky",
         "Now and then, far from land, the wind dies, fog rolls in and the sea turns on Link: Gyorgs, Seahats (some of "
         "them giants), Kargarocs and cannon boats come for him for a few minutes; at night more of them, Poes over the "
         "water and the Ghost Ship. Reaching an island ends it. Gyorgs come even before Link has the Boomerang.",
         {"Sometimes", "Often", "Relentless"}, true},
        {"storm", "Stormy weather", "Spooky", "Spells of heavy rain (the game's own), at night or at any hour.", {"At night", "Always"}, true},
        {"fog", "Thick fog", "Spooky", "Spells of fog in which the world fades a short way from Link, at night or at any hour.", {"At night", "Always"}, true},
        {"swim", "Endless swimming", "Gameplay", "Link's swim meter never runs out, so he can swim as far as he likes.", {}},
        {"giant-seahats", "Giant Seahats", "Spooky",
         "With Haunted sea on, some of its spells are giant ones: every Seahat a giant, one in four a colossus, and most of "
         "what comes for Link are Seahats (Gyorgs less often).", {}, true},
        {"phantom-ganon", "Phantom Ganon", "Spooky",
         "Rarely, far out at sea and mostly at night, the wind dies, fog rolls in and Phantom Ganon (his own size or "
         "titanic) comes out of it "
         "to fight around the boat as he does in Ganon's Tower, until he is beaten, Link reaches land or he fades back "
         "into the fog.",
         {"Normal size", "Titanic (12x)"}, true},
        {"big-ship", "Big ship (look-test)", "Experimental",
         "A pirate ship hull follows the King of Red Lions wherever he sails. A first step towards a walkable ship.", {}},
        {"horror-audio", "Horror audio", "Spooky",
         "While a spooky event's monsters are about, the music sinks low, and a heartbeat sounds that quickens as "
         "one of them comes close.", {}},
        {"deep", "Fog and the Deep", "Spooky",
         "Spells when, far from land, the sea and sky darken and a haze closes in the further Link sails from the "
         "nearest island, so the islands stand out like lighthouses.", {}, true},
        {"leviathan", "Leviathan", "Spooky",
         "Rarely, far out at sea, the wind dies, fog rolls in and a Gyorg eight times the size of any other surfaces "
         "behind the boat and circles it like a shark, keeping up however fast Link sails. Nothing kills it: make for land, "
         "or hold out until it loses interest.", {}, true},
        {"molgera", "Molgera of the deep", "Spooky",
         "Very rarely, far out at sea, the wind dies, fog rolls in and Molgera, the Wind Temple's sand worm and bigger "
         "than in its lair, bursts out of the water ahead of the boat and stays in its way, biting at Link.", {}, true},
        {"phantom-stalker", "Phantom stalker", "Spooky",
         "Rarely, on an island at night, fog rolls in and Phantom Ganon comes down behind Link and hunts him across the "
         "island, until Link takes to the sea, beats him, or he fades back into the fog.", {}, true},
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
const std::vector<std::string> kOften = {"Rare", "Now and then", "Often", "Always"};
int often(const std::string& id) { int i = index(id); return i < 0 ? 1 : g_often[i].load(std::memory_order_relaxed); }
void set_often(const std::string& id, int o) {
    const int i = index(id);
    if (i >= 0 && o >= 0 && o < (int)kOften.size()) g_often[i] = o;
}

// ---- spooky events ----
// Below Always, a spooky mod is an event the director starts now and then at a natural moment, as the TP HD recomp's
// horror mod does. Nothing starts before Link has the sail and the Wind Waker. On the open sea a roll comes once a
// minute: one sea event (Haunted sea, Phantom Ganon) at a time, a cooldown after each, none within reach of an island
// (the sea is a 7x7 grid of 100000-unit squares, each island near its square's centre), and chances that grow the
// longer Link stays out (x1 + minutes/10, up to x3) and reset when he lands. A sea event comes after 8 s of calm
// (dKyw_custom_windpower 0257E594: above 0 it overrides the wind, 0 gives it back) while fog rolls in, and ends on
// land, near an island or after its time, the fog rolling out. Night of the dead is a roll at nightfall for that night;
// Stormy weather and Thick fog are spells of a few minutes. Rain and fog fade in and out: the rain count
// (dKyw_rain_set) is ramped, and our fog is blended with the game's by dKy_fog_startendz_set's ratio.
// Test aid: WWHD_SPOOKY=<sea|giants|phantom|dead|storm|fog|deep|leviathan|molgera|stalker>@<game seconds>[;...] starts that event at that time.
namespace {
constexpr int kAlways = 3;
constexpr double kScale[] = {0.4, 1, 3};       // the chances: Rare, Now and then, Often
double g_until[kCount], g_cool;                // each event's end (game time); the sea events' cooldown
int g_warn = -1;                               // the sea event the calm is before
double g_warn_end, g_out_since;                // when the calm ends; when Link last stood on land
bool g_giant_spell;                            // this Haunted sea spell's Seahats are giants
bool chance(double p) { return rand() / (double)RAND_MAX < p; }
bool always(int k) { return g_often[k].load() >= kAlways; }
bool live(int k, double now) { return g_on[k].load() && (always(k) || now < g_until[k]); }
bool rolls(int k, double p) {
    const int o = g_often[k].load();
    return g_on[k].load() && o < kAlways && chance(p * kScale[o]);
}
constexpr double kGiantShare[] = {1 / 6.0, 1 / 3.0, 2 / 3.0};  // Giant Seahats: its share of Haunted sea's spells
bool giant_spell() {
    const int o = g_often[kGiants].load();
    return g_on[kGiants].load() && (o >= kAlways || chance(kGiantShare[o]));
}
const char* kEventName[kCount] = {"", "", "", "", "Night of the dead", "Haunted sea", "storm", "fog", "", "giants", "Phantom Ganon", "", "", "the deep", "Leviathan", "Molgera", "Phantom stalker"};
// Molgera (bwd_class, proc 0xD9; HD fields = decomp + 0x284 up to +0x1B5C): its l_HIO (10462A68) holds the head's
// scale (+0x1C, 1.3, also its 9 spheres' radii), the push on Link while it waits (+0x0C, 6.0) and its bite's reach
// (+0x14, 700), set while it is out
// and put back after. It leaves behind its lock-on helper (ATT, its process id at +0x1E38) and, if it made one, its
// sand floor (Bwdg, proc 0xDB, a BG mesh that reads Molgera every frame): both go with it
constexpr uint32_t kMolgeraScale = 0x10462A84, kMolgeraPush = 0x10462A74, kMolgeraBite = 0x10462A7C;
void remove_molgera(Cpu* c) {
    if (const uint32_t a = g_molgera.actor(c)) {
        guest_call(c, GC(0x025D57E4), {ld32(a + 0x1E38)});
        static const uint32_t found = mem::host_alloc(4);
        if (st32(found, 0); guest_call(c, GC(0x025D5578), {0xDB, found}) && ld32(found)) guest_call(c, GC(0x025D57E0), {ld32(found)});
        guest_call(c, GC(0x025D57E4), {g_molgera.id});
    }
    g_molgera = Single{}, g_molgera_actor = 0, g_molgera_dives = 0;
    st32(GD(kMolgeraScale), u32_bits(1.3f)), st32(GD(kMolgeraPush), u32_bits(6.f)), st32(GD(kMolgeraBite), u32_bits(700.f));
}
void wind(Cpu* c, float power) { c->f[1].ps0 = power; guest_call(c, GC(0x0257E594)); }
void begin(int k, double now) {
    g_until[k] = now + (k == kPhantom || k == kLeviathan ? 180 : k == kMolgera ? 150 : k == kStalker ? 120 : k == kSea ? 120 + rand() % 120 : 180 + rand() % 300);
    if (k == kPhantom || k == kStalker) g_phantom = Single{};  // made afresh by their blocks below
    if (k == kLeviathan) g_leviathan = Single{};
    if (k == kMolgera) g_molgera = Single{};
    if (k == kSea) g_giant_spell = giant_spell();
    trace("extras: event %s begins (%.0f s)%s", kEventName[k], g_until[k] - now, k == kSea && g_giant_spell ? ", giants" : "");
}
void finish(Cpu* c, int k, double now, const char* why) {
    trace("extras: event %s ends (%s)", kEventName[k], why);
    g_until[k] = 0;
    if (k != kNight && k != kStorm && k != kFog && k != kDeep) g_cool = now + (k == kSea ? 300 : 600);
    if ((k == kPhantom || k == kStalker) && g_phantom.actor(c)) guest_call(c, GC(0x025D57E4), {g_phantom.id});  // fopAcM_delete_id
    if (k == kLeviathan && g_leviathan.actor(c)) guest_call(c, GC(0x025D57E4), {g_leviathan.id});
    if (k == kMolgera) remove_molgera(c);
    if (k == kSea) {  // the spell's monsters break off: those out in the fog (cannon boats, the Ghost Ship) go
        const uint32_t link = ld32(kLinkPtr);
        for (uint32_t id : g_sea.ids)
            if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id});
                a && std::hypot(ldf32(a + kPos) - ldf32(link + kPos), ldf32(a + kPos + 8) - ldf32(link + kPos + 8)) > 2000)
                guest_call(c, GC(0x025D57E4), {id});
        if (g_ghost.actor(c)) guest_call(c, GC(0x025D57E4), {g_ghost.id});
    }
}
struct Forced { int k; double at; };
std::vector<Forced> forced_events() {
    std::vector<Forced> v;
    const char* e = getenv("WWHD_SPOOKY");
    static const std::pair<const char*, int> kNames[] = {{"sea", kSea}, {"giants", kGiants}, {"phantom", kPhantom},
                                                         {"dead", kNight}, {"storm", kStorm}, {"fog", kFog}, {"deep", kDeep}, {"leviathan", kLeviathan}, {"molgera", kMolgera}, {"stalker", kStalker}};
    for (std::string rest = e ? e : ""; !rest.empty();) {
        const std::string item = rest.substr(0, rest.find(';'));
        rest = item.size() < rest.size() ? rest.substr(item.size() + 1) : "";
        const size_t at = item.find('@');
        for (const auto& [name, k] : kNames)
            if (at != std::string::npos && item.compare(0, at, name) == 0) v.push_back({k, atof(item.c_str() + at + 1)});
    }
    return v;
}

// once a step, before the mods below read live()
void direct(Cpu* c, uint32_t sv, uint32_t link, double now, bool night, bool water, bool boat, bool settled, const std::string& stage) {
    auto has = [sv](uint8_t item) {
        for (int i = 0; i < 21; i++) if (ld8(sv + 0x3C + i) == item) return true;
        return false;
    };
    const bool sailor = has(0x78) && has(0x22);  // the sail, the Wind Waker
    const double x = ldf32(link + kPos), z = ldf32(link + kPos + 8);
    const double isle = std::hypot(x - std::round(x / 1e5) * 1e5, z - std::round(z / 1e5) * 1e5);
    static double wet = 0;  // the last time Link was on the boat or swimming: knocked off the boat he is neither for a moment
    if (water) wet = now;
    const bool ashore = stage != "sea" || now - wet > 3, cutscene = ld8(kPlay + 0x5292) != 0;
    if (ashore) g_out_since = now;

    static auto forced = forced_events();
    for (auto& f : forced)
        if (f.at >= 0 && now >= f.at) {
            f.at = -1;
            if (f.k == kGiants) g_until[kSea] = 0, begin(kSea, now), g_giant_spell = true;
            else begin(f.k, now);
        }
    // the sea events end on land, near an island, after their time, or (Phantom Ganon) once he is gone
    for (int k : {kSea, kPhantom, kLeviathan, kMolgera, kStalker})  // set to Always or switched off mid-event
        if (g_until[k] > 0 && (always(k) || !g_on[k].load())) finish(c, k, now, "its setting changed");
    for (int k : {kSea, kPhantom, kLeviathan, kMolgera})
        if (g_until[k] > 0) {
            if (ashore || isle < 3500) finish(c, k, now, ashore ? "Link is off the water" : "near an island");
            else if (now >= g_until[k]) finish(c, k, now, "time");
            else if (k == kPhantom && g_phantom.id != 0xFFFFFFFF && now >= g_phantom.next && !g_phantom.actor(c)) finish(c, k, now, "he is gone");
            else if (const uint32_t m = k == kMolgera && g_molgera_dives >= 3 ? g_molgera.actor(c) : 0; m && ld16(m + 0x1B32) && std::hypot(ldf32(m + kPos) - x, ldf32(m + kPos + 8) - z) > 4000)
                finish(c, k, now, "outrun");
        }
    // the Phantom stalker ends as Link boards the boat or leaves the sea stage, after its time, or once he is gone
    if (g_until[kStalker] > 0) {
        if (boat || stage != "sea") finish(c, kStalker, now, "Link took to the sea");
        else if (now >= g_until[kStalker]) finish(c, kStalker, now, "time");
        else if (g_phantom.id != 0xFFFFFFFF && now >= g_phantom.next && !g_phantom.actor(c)) finish(c, kStalker, now, "he is gone");
    }
    if (g_warn >= 0 && (now >= g_warn_end || ashore)) {
        wind(c, 0);
        if (!ashore && g_on[g_warn].load() && !always(g_warn)) begin(g_warn, now);
        g_warn = -1;
    }
    // with Haunted sea on Always there are no spells: Giant Seahats has its own, a roll every 3 minutes
    static double giants_roll = 0;
    if (always(kSea) && now >= giants_roll) g_giant_spell = giant_spell(), giants_roll = now + 180;
    // Night of the dead: a roll as night falls (or on arriving at night), for that night
    static bool was_night = false;
    if (night && !was_night && sailor && rolls(kNight, 1 / 3.0)) begin(kNight, now), g_until[kNight] = 1e18;
    if (!night && g_until[kNight] > 0) finish(c, kNight, now, "dawn");
    was_night = night;

    static double last_roll = 0;
    if (now - last_roll < 60 || !sailor || !settled || cutscene) return;
    last_roll = now;
    const double hour = ldf32(sv + 0x24) / 15;
    if (now >= g_until[kStorm] && (night || g_choice[kStorm].load() == 1) && rolls(kStorm, 0.05 * (night ? 1.5 : 0.7))) begin(kStorm, now);
    if (now >= g_until[kFog] && (night || g_choice[kFog].load() == 1) &&
        rolls(kFog, 0.06 * (night ? 1.5 : hour >= 4 && hour < 7 ? 2 : 0.5)))
        begin(kFog, now);
    if (stage == "sea" && ashore && night && now >= g_cool && g_until[kStalker] == 0 && rolls(kStalker, 0.03))
        begin(kStalker, now);
    if (now >= g_until[kDeep] && stage == "sea" && !ashore && rolls(kDeep, 0.07 * (night ? 1.5 : 0.8))) begin(kDeep, now);
    if (stage != "sea" || ashore || isle < 6000 || now < g_cool || g_warn >= 0 || g_until[kSea] > 0 || g_until[kPhantom] > 0 || g_until[kLeviathan] > 0 || g_until[kMolgera] > 0) return;
    const double more = std::min(3.0, 1 + (now - g_out_since) / 600);
    if (boat && rolls(kPhantom, 0.017 * (night ? 2 : 0.4) * more)) g_warn = kPhantom;
    else if (rolls(kLeviathan, 0.012 * (night ? 1.5 : 0.7) * more)) g_warn = kLeviathan;
    else if (rolls(kMolgera, 0.008 * (night ? 1.5 : 0.7) * more)) g_warn = kMolgera;
    else if (rolls(kSea, 0.08 * (night ? 1.5 : 0.7) * more)) g_warn = kSea;
    if (g_warn >= 0) {
        g_warn_end = now + 8;
        wind(c, 0.05f);
        trace("extras: the calm before %s (%.0f m from the island, x%.1f)", kEventName[g_warn], isle, more);
    }
}

// Horror audio: while a monster event runs or one of these mods' monsters is near, the music sinks to a fifth and a
// heartbeat sounds, quicker the closer one is. JAIZelBasic (ld32(GD(0x101FFC78))): its main BGM (+0x78) plays at the
// product of the factors at +0x90..+0xAC and +0xBC, as bgmAllMute (0202844C) computes it before JAISound::setVolume
// (0280B68C: the sound, the volume in f1, r4 frames to get there, r5 0); +0x90 is the factor bgmAllMute zeroes. It is
// held low here and set again when the game puts it back to 1 (it does as a BGM starts); not during a cutscene, which
// may mute it itself. The beat: mDoAud_seStart (025E1988, the sound id alone).
constexpr uint32_t kHeartbeat = 0x509B;  // JA_SE_CM_KK_HEARTBEAT
void horror_audio(Cpu* c, uint32_t link, double now, bool event, bool enabled) {
    const uint32_t zel = ld32(GD(0x101FFC78));
    static bool ducked = false;
    if (!in_mem2(zel) || (enabled && ld8(kPlay + 0x5292)) || (!enabled && !ducked)) return;
    double nearest = 1e9;
    auto near = [&](uint32_t id) {
        if (const uint32_t a = id == 0xFFFFFFFF ? 0 : guest_call(c, GC(0x025DE50C), {id}))
            nearest = std::min(nearest, std::hypot(ldf32(a + kPos) - ldf32(link + kPos), ldf32(a + kPos + 8) - ldf32(link + kPos + 8)));
    };
    for (uint32_t id : g_sea.ids) near(id);
    for (uint32_t id : g_night.ids) near(id);
    near(g_phantom.id);
    near(g_leviathan.id);
    near(g_molgera.id);
    const bool duck = enabled && (event || nearest < 2000);
    if (const float level = duck ? 0.2f : 1.f; (duck || ducked) && std::fabs(ldf32(zel + 0x90) - level) > 0.01f) {
        st32(zel + 0x90, u32_bits(level));
        if (const uint32_t bgm = ld32(zel + 0x78)) {
            double vol = ldf32(zel + 0xBC);
            for (uint32_t o = 0x90; o <= 0xAC; o += 4) vol *= ldf32(zel + o);
            c->f[1].ps0 = vol;
            guest_call(c, GC(0x0280B68C), {bgm, duck ? 90u : 60u, 0});
        }
    }
    ducked = duck;
    static double next_beat = 0;
    if (enabled && nearest < 900 && now >= next_beat) {
        guest_call(c, GC(0x025E1988), {kHeartbeat});
        next_beat = now + (nearest < 400 ? 0.45 : 0.8);
    }
}
}  // namespace

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
    static double settle = 0;  // no spawning while a stage is still being entered (the spawns would land in the old one)
    if (fresh) {
        g_night = Spawner{}, g_sea = Spawner{}, g_gyctrl = 0xFFFFFFFF, g_ghost = g_bigship = g_phantom = g_leviathan = g_molgera = Single{};
        g_gyorg_ids.clear(), g_giant_ids.clear(), g_big_hits.clear();
        settle = game_time() + 2;
        if (g_warn >= 0) wind(c, 0), g_warn = -1;  // a sea event does not outlive its stage
        g_until[kSea] = g_until[kPhantom] = g_until[kLeviathan] = g_until[kMolgera] = g_until[kStalker] = 0;
        remove_molgera(c);
    }
    const bool settled = !fresh && game_time() >= settle;
    g_big_hits.clear();  // refilled below by the mods whose monsters hit bigger

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
    const bool night = hour >= 19 || hour < 5, sea = stage == "sea" && water, boat = proc >= 0x86 && proc <= 0x91 && proc != 0x90;
    static const double kEvery[] = {25, 12, 5};  // seconds between spawns: Sometimes, Often, Relentless
    static const size_t kMax[] = {2, 4, 7};      // monsters out at once
    direct(c, sv, link, now, night, water, boat, settled, stage);
    // stormy weather: the rain count (dKyw_rain_set, 0257E7C0; 250 is a downpour) set every step while it rains, eased
    // up and down over 30 s, and 0 once at the end
    static float rain = 0;
    if (const float to = live(kStorm, now) && (night || g_choice[kStorm].load() == 1) ? 250.f : 0.f; rain > 0 || to > 0) {
        rain = to > rain ? std::min(to, rain + 250 / 900.f) : std::max(to, rain - 250 / 900.f);
        guest_call(c, GC(0x0257E7C0), {(uint32_t)rain});
    }
    // thick fog, also before and during a sea event: dKy_fog_startendz_set (0255671C; start, end, ratio in f1..f3),
    // which actors call for their own fog, every step: the world fades out from 100 to 2500 units around the camera,
    // blended with the game's own fog by the ratio, which eases up and down over 20 s
    // Fog and the Deep: further than 15000 from the island in Link's sea square the sea darkens, up to 40000; the
    // whole picture (dKy_set_allcol_ratio 02560444) and the sky (dKy_set_vrboxcol_ratio 0255A51C) are dimmed, factors
    // the environment gathers from its callers each frame, and a haze from 300 to 6000 blends in; eased over 20 s
    static float deep = 0;
    {
        const double x = ldf32(link + kPos), z = ldf32(link + kPos + 8);
        const double isle = std::hypot(x - std::round(x / 1e5) * 1e5, z - std::round(z / 1e5) * 1e5);
        const float to = live(kDeep, now) && stage == "sea" ? (float)std::clamp((isle - 15000) / 25000, 0.0, 1.0) : 0.f;
        deep = to > deep ? std::min(to, deep + 1 / 600.f) : std::max(to, deep - 1 / 600.f);
        if (deep > 0) {
            c->f[1].ps0 = 1 - 0.45 * deep;
            guest_call(c, GC(0x02560444));
            c->f[1].ps0 = 1 - 0.55 * deep;
            guest_call(c, GC(0x0255A51C));
        }
    }
    static float fog = 0;
    const bool sea_event = g_warn >= 0 || (g_until[kSea] > now && !always(kSea)) || (g_until[kPhantom] > now && !always(kPhantom)) ||
                           (g_until[kLeviathan] > now && !always(kLeviathan)) || (g_until[kMolgera] > now && !always(kMolgera)) ||
                           (g_until[kStalker] > now && !always(kStalker));
    static bool fogged = false;
    const float to = (live(kFog, now) && (night || g_choice[kFog].load() == 1)) || sea_event ? 1.f : 0.f;
    fog = to > fog ? std::min(to, fog + 1 / 600.f) : std::max(to, fog - 1 / 600.f);
    if (fog > 0 || deep > 0 || fogged) {  // the thicker of the two, and once more at 0 to hand the game its fog back
        const bool thick = fog >= deep * 0.9f;
        c->f[1].ps0 = thick ? 100 : 300, c->f[2].ps0 = thick ? 2500 : 6000, c->f[3].ps0 = thick ? fog : deep * 0.9f;
        guest_call(c, GC(0x0255671C));
        fogged = fog > 0 || deep > 0;
    }
    // endless swimming: the air meter (dComIfG_play_c::mAirMeter, 900 = full, +0x5B4C) kept full while Link swims
    if (on<kSwim>() && (ld32(link + 0x6A70) & 0x40000)) st32(kPlay + 0x5B4C, 900);
    // Night of the dead: on land at night (the sea stage's islands count)
    if (live(kNight, now) && night && !water && settled) {
        const int level = g_choice[kNight].load();
        g_night.prune(c);
        static const Monster kDead[] = {kRedead, kRedead, kStalfos, kPoe};
        if (now >= g_night.next && g_night.ids.size() < kMax[level]) {
            spawn(c, g_night, kDead[rand() % 4], link, 450 + rand() % 350, true, 30, true);
            g_night.next = now + kEvery[level] * (0.6 + rand() / (double)RAND_MAX * 0.8);
        }
    }
    // Haunted sea: on the open sea, more often at night
    if (live(kSea, now) && sea && settled) {
        const bool giants = on<kGiants>() && (always(kGiants) || g_giant_spell);
        const int level = g_choice[kSea].load();
        g_sea.prune(c);
        if (now >= g_sea.next && g_sea.ids.size() < kMax[level] + (night ? 2 : 0)) {
            // the Gyorgs' controller first, then by chance a cannon boat (a sailing Link only), Gyorgs, or Seahats,
            // Kargarocs and at night Poes; the Ghost Ship is made below, Phantom Ganon by his own mod
            const bool ctrl = g_gyctrl != 0xFFFFFFFF && guest_call(c, GC(0x025DE50C), {g_gyctrl});
            if (!ctrl) {  // its controller first; Gyorgs once it exists
                Spawner none;
                g_gyctrl = spawn(c, none, kGyCtrl, link, 0, false, 0, false);
                g_gyorg_ids.push_back(g_gyctrl);
            } else if (boat && rand() % 4 == 0) {  // a cannon boat ahead of a sailing Link, bombarding from its range (2500-6000)
                spawn(c, g_sea, kOship, link, 3000 + rand() % 2500, false, 0, false);
            } else if (rand() % (giants ? 4 : 2) == 0) {
                const uint32_t id = spawn(c, g_sea, kGyorg, link, 900 + rand() % 600, false, -40, false, g_gyctrl);
                if (id != 0xFFFFFFFF) g_gyorg_ids.push_back(id);
            } else {  // Seahats (one in three a giant), Kargarocs; at night also Poes drifting over the water
                static const Monster kDay[] = {kSeahat, kSeahat, kKargaroc}, kNightSea[] = {kSeahat, kKargaroc, kPoe, kPoe};
                const Monster m = giants ? kSeahat : night ? kNightSea[rand() % 4] : kDay[rand() % 3];
                const bool poe = m.proc == kPoe.proc;
                const uint32_t id = spawn(c, g_sea, m, link, poe ? 500 + rand() % 400 : 900 + rand() % 600, false, poe ? 120 : 300, false);
                if (m.proc == kSeahat.proc && (giants || rand() % 3 == 0) && id != 0xFFFFFFFF)
                    g_giant_ids.push_back({id, giants && rand() % 4 == 0 ? kColossus : kGiant});
            }
            g_sea.next = now + kEvery[level] * (night ? 0.6 : 1) * (0.6 + rand() / (double)RAND_MAX * 0.8);
        }
        // the Ghost Ship: one at a time, at night (the game hides it by day and when its moon phase is not tonight's,
        // dKy_moon_type_chk 02560828), far off, singing; it fades as Link comes close (unless he has the Ghost Ship Chart)
        if (night && boat && g_ghost.wanted(c, now)) {
            Monster ghost = kAyush;
            ghost.param |= guest_call(c, GC(0x02560828)) & 0xFF;
            Spawner none;
            g_ghost.id = spawn(c, none, ghost, link, 4500 + rand() % 2000, -1, 0, false);
            g_ghost.next = now + 5;
        }
        for (uint32_t id : g_sea.ids)
            if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}); a && (ld16(a + 8) == kGyorg.proc || ld16(a + 8) == kSeahat.proc)) {
                float scale = kHitScale;
                for (const auto& [gid, gs] : g_giant_ids) if (gid == id && gs > kGiant) scale = gs;
                g_big_hits.push_back({a, a + (ld16(a + 8) == kGyorg.proc ? 0x1000u : 0x1600u), scale});
            }
        for (const auto& [id, scale] : g_giant_ids)
            if (const uint32_t ph = guest_call(c, GC(0x025DE50C), {id}))
                for (uint32_t off : {0x12C4u, 0x12CCu})
                    if (const uint32_t model = ld32(ph + off); in_mem2(model))
                        for (int i = 0; i < 3; i++) st32(model + 0xBC + 4 * i, u32_bits(scale));
        // the Gyorgs hunt Link: without their controller's path and battle state they would only dive and circle
        // (daGy_c mode at +0x3CC: 0 dive, 1 circle, 6 circle with others), so those are turned into an attack on the
        // player (modeAttackPlayerInit, 02169B48, mode 3); attacks, damage and deletion run as the game decides
        for (uint32_t id : g_gyorg_ids) {
            const uint32_t gy = id == g_gyctrl || id == g_leviathan.id ? 0 : guest_call(c, GC(0x025DE50C), {id});
            if (const uint32_t mode = gy ? ld32(gy + 0x3CC) : 9; mode == 0 || mode == 1 || mode == 6) guest_call(c, GC(0x02169B48), {gy});
        }
    }
    // Phantom Ganon: one near a sailing Link, at the chosen size, or (the Phantom stalker) his own size behind Link on an
    // island at night: his own execute eases the actor's scale (+0x330) to 1, so it is
    // set after it every step; his colliders' centres follow the model and their sizes are scaled by the hooks below
    const bool stalked = live(kStalker, now) && stage == "sea" && !boat && (night || !always(kStalker));
    if (((live(kPhantom, now) && sea && boat) || stalked) && settled) {
        load_scene_bank(c, "Pscene080.jpc", 2);
        if (g_phantom.wanted(c, now)) {
            Spawner none;
            g_phantom.id = spawn(c, none, kFganon, link, 1500 + rand() % 500, 1, 0, false);
            g_phantom.next = now + 20;
        }
        if (const uint32_t pg = g_phantom.actor(c)) {
            static const float kSize[] = {1.f, 12.f};
            const float size = stalked ? 1.f : kSize[g_choice[kPhantom].load()];
            for (int i = 0; i < 3; i++) st32(pg + 0x330 + 4 * i, u32_bits(size));
            g_big_hits.push_back({pg, pg + 0x1000, size});  // his sword and body colliders as big as he is
            // his energy sphere (the model at +0x8A8 that tama_draw shows) and its emitters (dark ball and throw effects,
            // +0x600..+0x60C; JPABaseEmitter scale +0x220 and particle scale +0x238, xyz each) as big as he is
            if (size > 1) {
                for (uint32_t o : {0x600u, 0x604u, 0x608u, 0x60Cu})
                    if (const uint32_t em = ld32(pg + o); in_mem2(em))
                        for (int i = 0; i < 3; i++) st32(em + 0x220 + 4 * i, u32_bits(size)), st32(em + 0x238 + 4 * i, u32_bits(size));
                if (const uint32_t model = ld32(pg + 0x8A8); in_mem2(model))
                    for (int i = 0; i < 3; i++) st32(model + 0xBC + 4 * i, u32_bits(size));
            }
            // after a volley he waits (action +0x5BA 6 "shot", mode +0x5BC 3) for his energy ball to die, which at sea,
            // with nothing for a missed ball to hit, can take a long while: after 8 s it is ended (ball state +0x63C
            // 35, as when it flies too far), and he flies on to his next attack
            static double waiting = 0;
            if (ld16(pg + 0x5BA) != 6 || ld16(pg + 0x5BC) != 3) waiting = now;
            else if (now - waiting > 8) st8(pg + 0x63C, 35), waiting = now;
            // he flies and fights around his home point (fopAc_ac_c::home.pos, +0x2EC): Link, wherever he goes
            st32(pg + 0x2EC, ld32(link + kPos));
            st32(pg + 0x2F4, ld32(link + kPos + 8));
        }
    }
    // Leviathan: one Gyorg (made as the others are, a child of the Haunted sea's GyCtrl, which is made here too if
    // needed) at 8x: setMtx (0216731C) copies the actor's scale (+0x330) into its model each execute, its spheres are
    // sized through the SetR hooks (g_big_hits) but its body collider's radius (+0x9B4) is a plain store each execute,
    // multiplied here after it; it circles and rams the boat (below), and site_02168488 keeps it alive
    g_leviathan_actor = g_leviathan.actor(c);  // site_02168488 keeps it alive wherever it is
    if (live(kLeviathan, now) && sea && settled) {
        if (!(g_gyctrl != 0xFFFFFFFF && guest_call(c, GC(0x025DE50C), {g_gyctrl}))) {
            if (g_leviathan.wanted(c, now)) {
                Spawner none;
                g_gyctrl = spawn(c, none, kGyCtrl, link, 0, false, 0, false);
                g_gyorg_ids.push_back(g_gyctrl);
                g_leviathan.next = now + 2;
            }
        } else if (g_leviathan.wanted(c, now)) {
            Spawner none;
            g_leviathan.id = spawn(c, none, kGyorg, link, 2500, 1, -40, false, g_gyctrl);
            if (g_leviathan.id != 0xFFFFFFFF) g_gyorg_ids.push_back(g_leviathan.id);
            g_leviathan.next = now + 10;
        }
        g_leviathan_actor = g_leviathan.actor(c);
        if (const uint32_t gy = g_leviathan_actor) {
            constexpr float kLeviathanSize = 8;
            for (int i = 0; i < 3; i++) st32(gy + 0x330 + 4 * i, u32_bits(kLeviathanSize));
            st32(gy + 0x9B4, u32_bits((float)ldf32(gy + 0x9B4) * kLeviathanSize));
            g_big_hits.push_back({gy, gy + 0x1000, kLeviathanSize});
            // it circles the boat (modeCircle 02169C00: around the ship, play +0x5B3C) while its GyCtrl (+0x3D4) has its
            // child slot's clear-line flag set (byte +0x42E + the child index +0x3C8), which our parked controller never
            // sets (it dives instead); every 10 s it is sent to breach and ram the ship (modeAttackInit 02169B68, mode 2):
            // its own circle only attacks when play +0x5CD8 lacks 0x01000000, set while sailing
            static double rammed = 0;
            if (const uint32_t ctrl = ld32(gy + 0x3D4); in_mem2(ctrl) && ld32(gy + 0x3C8) < 8) st8(ctrl + 0x42E + ld32(gy + 0x3C8), 1);
            if (ld32(gy + 0x3CC) == 1 && now >= rammed + 10) guest_call(c, GC(0x02169B68), {gy}), rammed = now;
            // a sailing boat would leave it behind: it is kept within 2500 of Link
            const double far = std::hypot(ldf32(gy + kPos) - ldf32(link + kPos), ldf32(gy + kPos + 8) - ldf32(link + kPos + 8));
            if (far > 2500)
                for (uint32_t o : {kPos, kPos + 8})
                    st32(gy + o, u32_bits((float)(ldf32(link + o) + (ldf32(gy + o) - ldf32(link + o)) * 2500 / far)));
        }
    }
    // Molgera: made far ahead (it starts out of sight, state 0 at y -5000) and sent from its intro straight into waiting
    // (state +0x1B32 1, substate +0x1B34 0), from which it rises and bites when Link is within reach (state 5); kept
    // 1100 ahead of him, its bite reaching 1300 (it is 2.5x); no sand floor (+0x1B5C set before it makes one), no forced boss camera
    // (+0x3EC8 0, which would make its Execute force "WindBoss"); hook_020FF80C keeps it from swallowing Link
    // it never stays without the event (switched off, or Always and Link has landed), and hook_020FF80C guards it
    // wherever it is
    static double wet = 0;
    if (water) wet = now;
    if (g_molgera.id != 0xFFFFFFFF && (!live(kMolgera, now) || now - wet > 3)) remove_molgera(c);
    g_molgera_actor = g_molgera.actor(c);
    if (live(kMolgera, now) && sea && settled) {
        load_scene_bank(c, "Pscene071.jpc", 3);
        if (g_molgera.wanted(c, now)) {
            Spawner none;
            g_molgera.id = spawn(c, none, Monster{0xD9, -1, 0}, link, 2000, -1, 0, false);
            g_molgera.next = now + 10;
        }
        if (const uint32_t a = g_molgera_actor = g_molgera.actor(c)) {
            st8(a + 0x1B5C, 1), st32(a + 0x3EC8, 0);
            if (ld16(a + 0x1B32) == 0) st16(a + 0x1B32, 1), st16(a + 0x1B34, 0), st32(a + kPos + 4, u32_bits(-2500.f));
            st32(GD(kMolgeraScale), u32_bits(1.3f * 2.5f)), st32(GD(kMolgeraPush), 0), st32(GD(kMolgeraBite), u32_bits(1300.f));
            // it makes itself at the world's origin (its boss room's centre): it is put straight ahead of Link, then
            // chases at up to 40 a step, about a boat under full sail; left 3000 behind it dives and bursts out ahead
            // again, three times, and then lets him go (outrun by 4000, the event ends)
            const double yaw = (int16_t)ld16(link + kAngleY) * M_PI / 32768;
            const float tx = (float)(ldf32(link + kPos) + sin(yaw) * 1100), tz = (float)(ldf32(link + kPos + 8) + cos(yaw) * 1100);
            static uint32_t placed = 0;
            if (placed == g_molgera.id && g_molgera_dives < 3 && ld16(a + 0x1B32) &&
                std::hypot(ldf32(a + kPos) - ldf32(link + kPos), ldf32(a + kPos + 8) - ldf32(link + kPos + 8)) > 3000) {
                placed = 0, g_molgera_dives++;
                st16(a + 0x1B32, 1), st16(a + 0x1B34, 0), st32(a + kPos + 4, u32_bits(-2500.f));
                trace("extras: Molgera dives and comes up ahead again (%d)", g_molgera_dives);
            }
            const bool snap = placed != g_molgera.id;
            placed = g_molgera.id;
            for (auto [o, to] : {std::pair{kPos, tx}, std::pair{kPos + 8, tz}}) {
                const float at = (float)ldf32(a + o);
                st32(a + o, u32_bits(snap ? to : at + std::clamp(to - at, -40.f, 40.f)));
            }
            // at its size its head's spheres reach the boat every few seconds for 4 hearts: a hit costs at most one
            // heart, and none in the 4 s after one (unless the Damage multiplier says otherwise)
            static uint16_t life = 0;
            static double hit = 0, seen = 0;
            if (now - seen > 1) life = ld16(sv + 2);  // a new event (or after a pause): start from his hearts now
            seen = now;
            if (const uint16_t was = life, cur = ld16(sv + 2); cur < was && !on<kDamage>()) {
                st16(sv + 2, now < hit + 4 ? was : std::max<int>(cur, was - 4));
                if (now >= hit + 4) hit = now;
            }
            life = ld16(sv + 2);
        }
    }
    horror_audio(c, link, now, sea_event || (g_until[kNight] > now && !always(kNight) && night), on<kAudio>());
    // Big ship: a pirate ship hull made once the King of Red Lions (dComIfGp_getShipActor, play +0x5B3C) is in the sea
    // stage, then put at his position and heading every step (current.pos, current.angle.y +0x322, shape_angle.y)
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); on<kBigShip>() && stage == "sea" && in_mem2(korl) && settled) {
        const uint32_t hull = g_bigship.actor(c);
        if (!hull) {
            if (g_bigship.wanted(c, now)) {
                Spawner none;
                g_bigship.id = spawn(c, none, kPirates, korl, 0, false, 0, false);
                g_bigship.next = now + 5;
            }
        } else {
            // the hull's centre 1300 ahead of him, so he rides at its stern
            const double yaw = (int16_t)ld16(korl + kAngleY) * M_PI / 32768;
            st32(hull + 0x314, u32_bits((float)(ldf32(korl + 0x314) + sin(yaw) * 1300)));
            st32(hull + 0x318, ld32(korl + 0x318));
            st32(hull + 0x31C, u32_bits((float)(ldf32(korl + 0x31C) + cos(yaw) * 1300)));
            st16(hull + 0x322, ld16(korl + kAngleY));
            st16(hull + kAngleY, ld16(korl + kAngleY));
        }
    }
    // test aid: WWHD_EXTRAS_SPAWN=proc:param:dist:dy[:anglex] (hex) spawns that once, 3 s after Link appears
    static const char* test_spawn = getenv("WWHD_EXTRAS_SPAWN");
    static double test_at = -1;
    if (test_spawn) {
        if (fresh) test_at = now + 4;
        if (test_at > 0 && now >= test_at) {
            test_at = -1;
            unsigned proc = 0, param = 0, dist = 0, dy = 0, ax = 0;
            if (sscanf(test_spawn, "%x:%x:%x:%x:%x", &proc, &param, &dist, &dy, &ax) >= 4) {
                Spawner none;
                spawn(c, none, Monster{(int16_t)proc, -1, param, (int16_t)ax}, link, (float)dist, false, (float)dy, false);
            }
        }
    }
}
}  // namespace mods::extras

// the Boomerang check of daGy_c::_create (02167A64, r30 the Gyorg) and daGy_Ctrl_c::_create (0216BB70, r31 the
// controller), just after dComIfGs_checkGetItem returned in r3: passed for the processes Haunted sea made
static bool ours(uint32_t actor) {
    return std::find(mods::extras::g_gyorg_ids.begin(), mods::extras::g_gyorg_ids.end(), ld32(actor + 4)) != mods::extras::g_gyorg_ids.end();
}
extern "C" void site_02167A64(Cpu* c) { if (!c->r[3] && ours(c->r[30])) c->r[3] = 1; }
extern "C" void site_0216BB70(Cpu* c) {
    if (!c->r[3] && ours(c->r[31])) c->r[3] = 1;
}

// the size setters of the collision shapes (cM3dGSph::SetR, cM3dGCyl::SetR / SetH; the size in f1): bigger for
// Haunted sea's Gyorgs and Seahats
static void big_hit(Cpu* c) {
    for (const auto& [lo, hi, scale] : mods::extras::g_big_hits)
        if (c->r[3] >= lo && c->r[3] < hi) { c->f[1].ps0 *= scale; return; }
}
extern "C" void f_02018C8C_orig(Cpu* c);
extern "C" void hook_02018C8C(Cpu* c) { big_hit(c); f_02018C8C_orig(c); }
extern "C" void f_020184DC_orig(Cpu* c);
extern "C" void hook_020184DC(Cpu* c) { big_hit(c); f_020184DC_orig(c); }
extern "C" void f_02018428_orig(Cpu* c);
extern "C" void hook_02018428(Cpu* c) { big_hit(c); f_02018428_orig(c); }

// JPABaseEmitter::setGlobalRTMatrix (028249B0; r4, r5 inside the emitter): Phantom Ganon's energy ball calls it on the
// emitters of its hit effect without checking them (daFgmahou_Execute 0213E098). With Pscene080 in bank 2 they are
// made; this guard is for when they are not (the bank failed to load or went with the stage): nothing to set then
extern "C" void f_028249B0_orig(Cpu* c);
extern "C" void hook_028249B0(Cpu* c) {
    if (c->r[4] < 0x1000) return;
    f_028249B0_orig(c);
}

// dPa_control_c::removeScene (025A809C), as the play scene ends: the boss banks go with the stage's own
// (JPAEmitterManager::clearResourceManager 028221EC deletes their emitters and empties the slot)
extern "C" void f_025A809C_orig(Cpu* c);
extern "C" void hook_025A809C(Cpu* c) {
    using namespace mods::extras;
    const uint32_t control = c->r[3];
    for (int slot : {2, 3})
        if (g_bank_heap[slot]) {
            guest_call(c, GC(0x028221EC), {emitter_manager(), (uint32_t)slot});
            guest_call(c, GC(0x025E3868), {g_bank_heap[slot]});
            g_bank_heap[slot] = 0;
            mods::trace("extras: particle bank %d freed with the stage", slot);
        }
    c->r[3] = control;
    f_025A809C_orig(c);
}

// daGy_c::checkTgHit's death test (02168488, lbz r11, 0x3A1(r30); r30 the Gyorg): the Leviathan's health is put back
// first, so nothing (a bomb sets it to 0 outright) kills it
extern "C" void site_02168488(Cpu* c) {
    using namespace mods::extras;
    if (g_leviathan_actor && c->r[30] == g_leviathan_actor) st8(c->r[30] + 0x3A1, 100);
}

// Molgera's bwd_move (020FF80C): when its bite catches Link (eat_attack counts m3C1E, s16 +0x3E96, up to 1) the
// swallow would order an event, lock the camera and fling him off the boat: here it is a missed bite instead, back to
// waiting (state 1, substate 0, timers +0x1B50 0x7A and +0x1B54 100), before bwd_demo_camera sees it
extern "C" void f_020FF80C_orig(Cpu* c);
extern "C" void hook_020FF80C(Cpu* c) {
    using namespace mods::extras;
    const uint32_t a = c->r[3];
    f_020FF80C_orig(c);
    if (a == g_molgera_actor && g_molgera_actor && ld16(a + 0x3E96) == 1)
        st16(a + 0x3E96, 0), st16(a + 0x1B32, 1), st16(a + 0x1B34, 0), st16(a + 0x1B50, 0x7A), st16(a + 0x1B54, 100);
}

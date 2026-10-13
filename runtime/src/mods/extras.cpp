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
#include <unordered_map>
#include <utility>
#include <vector>

#include "guest_addr.h"
#include "mods.h"
#include "coop_pose.h"
#include "coop_sync.h"
#include "darknut.h"
#include "shadow_beast.h"
#include "../gx2/gx2.h"
#include "../input.h"
#include "../input_map.h"
#include "../true60.h"
#include "../overlay/overlay.h"
#include "../savestate.h"
#include "../screenshot.h"
#include "../platform/keycodes.h"
#include "imgui.h"
#include "runtime.h"

namespace mods::extras {
namespace {
enum Id { kDamage, kRupees, kTime, kTurbo, kNight, kSea, kStorm, kFog, kSwim, kGiants, kPhantom, kBigShip, kAudio, kDeep, kLeviathan, kMolgera, kStalker, kBeedle, kCalm, kPhoto, kGunner, kBlood, kCyclone, kMaelstrom, kArmada, kOcto, kWrongLion, kDrowned, kBoarding, kRaft, kFairWind, kPacing, kUpgrades, kWarp, kHelmaroc, kCoop, kTpDarknuts, kCount };  // the catalogue's order
std::atomic<bool> g_on[kCount];
std::atomic<int> g_choice[kCount];
std::atomic<int> g_often[kCount];
[[maybe_unused]] const bool g_often_set = [] { for (auto& o : g_often) o = 1; return true; }();  // How often: Now and then

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
                  kOship{0xB3, -1, 0x00FFFF00, (int16_t)0xFFFF},  // (angle.x 0xFFFF the plain kind; 0x00FF the gold ship)
                  // the Ghost Ship (daGhostship): no path; its moon phase (low byte) is tonight's when spawned (see below)
                  kAyush{0xB2, -1, 0x00FF0000},
                  // Tetra's pirate ship (daObjPirateship): type 5 (no pirates, no scene, but its wheel Kaji and catapult
                  // Touseki, which type 3 leaves out though their collision is in the hull), no path; a moving BG with a
                  // walkable deck. Its sail (Psail), flag, wheel and catapult follow its model by themselves
                  kPirates{0x39, -1, 0x05FFFF00},
                  // Phantom Ganon (daFganon): type 3 (the Ganon's Tower fight) with no switch; he waits 10000 above, unseen,
                  // until Link comes within byte 1 x 10 of where he was made (2550), then descends and fights (flies, fires,
                  // spin attacks, and makes his own short-lived copies). HD fganon_class: mAction +0x5BA, mMode +0x5BC,
                  // materialized +0x8C1 (GameCube + 0x234 from there; the first fields are + 0x11C)
                  kFganon{0xF1, -1, 0x00FFFF03},
                  // a Bokoblin (daBk), the Shadow stalker's body (mods/shadow_beast.cpp): type 0, no lantern, the default
                  // weapon, sight 0xFA x 10, no path, no switch
                  kBokoblin{0xBD, -1, 0xFFFFFA00},
                  // a Darknut (daTn), the Darknut stalker's body (mods/darknut.cpp): roams (behaviour 0), colour 0, its
                  // default range, no path, no switch
                  kDarknutTn{0xBF, -1, 0xFFFFFF00},
                  // Beedle's Shop Ship (daObj_Ikada type 1, IkadaH vtsp): no path (angle.x bits 8..15 0xFF); a moving BG
                  kBeedleShip{0x44, -1, 0x00000001, (int16_t)0xFF00},
                  // the flagged raft (daObj_Ikada type 0, IkadaH vikae): the same actor, flat, with a mast and a flag
                  kRaftShip{0x44, -1, 0x00000000, (int16_t)0xFF00};

// the monsters a spawner has out, by process id; forgotten when they die or the stage changes
struct Spawner {
    std::vector<uint32_t> ids;
    double next = 0;  // game time of the next spawn
    void prune(Cpu* c) {
        std::erase_if(ids, [c](uint32_t id) { return !guest_call(c, GC(0x025DE50C), {id}); });
    }
};
Spawner g_night, g_sea, g_armada, g_boarders;
constexpr Monster kGyCtrl{0xE5, -1, 0x7F00FF01};
// one actor kept out at a time; a spawn takes a few steps to come to life, so it is not retried before `next`
struct Single {
    uint32_t id = 0xFFFFFFFF;
    double next = 0;
    uint32_t actor(Cpu* c) { return id == 0xFFFFFFFF ? 0 : guest_call(c, GC(0x025DE50C), {id}); }
    bool wanted(Cpu* c, double now) { return !actor(c) && now >= next; }
};
Single g_ghost, g_bigship, g_phantom, g_leviathan, g_molgera, g_beedle, g_tow_rope, g_cyclone, g_whirl, g_helmaroc;
uint32_t g_cyclone_actor = 0;  // for its hooks
// the event the armada's boats were made for (its end time; -1 none yet), and the boats made
double g_armada_for = -1;
size_t g_armada_made = 0;
bool g_whirl_fading = false;  // Maelstrom's whirlpool told to fade: deleted once it has
int g_octo_gyorgs = 0;       // Gyorgs owed with Big Octo, made once their controller exists
double g_octo_for = -1;      // the event Big Octo was made for
Single g_octo;               // Big Octo, and (for its death hook) the actor and whether it has died
uint32_t g_octo_actor = 0;
bool g_octo_dead = false;
uint32_t g_leviathan_actor = 0, g_molgera_actor = 0;  // for the hooks below, which must not call into the game
uint32_t g_phantom_actor = 0;                          // Phantom Ganon, and his size, for the same
// Beedle's ship in tow behind the King of Red Lions (below), for its hooks: the ship, the boat, the ship's BG matrix
uint32_t g_beedle_actor = 0, g_beedle_lion = 0, g_beedle_mtx = 0;
std::atomic<int> g_cam_yaw{0};  // the way the camera looks (s16): spawns out of its view, and a test driver
bool g_beedle_aboard = false;  // Link on its deck: its hut's door (an exit only where Beedle's own ship lies) does nothing
float g_phantom_size = 1, g_fx_size = 1;               // g_fx_size: his size while his execute runs (particle hook)
// the Leviathan and Molgera can be killed, by any weapon (a cannonball or bomb 4, an arrow 2, the sword its power):
// what is left of their health, and the Leviathan's death as its hit test saw it (loot and the event's end follow)
constexpr int kLeviathanHealth = 48, kMolgeraHealth = 80, kHelmarocHealth = 60;
// Ship upgrades, bought from Zunari on Windfall (shop below): each 0 to 3, kept in the Quest Log's own save data, two
// bits each in one byte (upgrade_byte): sailing (the boat carried on by a share of its speed), the guns' reload (a
// manned gun's; the crew test aid's), and the shots (their burst, their size, and their harm to the sea's bosses)
enum Upgrade { kUpSail, kUpReload, kUpShot };
constexpr double kSailBonus[] = {0, 0.15, 0.30, 0.50}, kReload[] = {1, 0.75, 0.5, 0.3}, kCrewReload[] = {4, 3, 2, 1};
constexpr float kShot[] = {1, 1.5f, 2, 3};
constexpr int kUpPrice[] = {500, 900, 1500};
// the byte: the last of the save data's reserve block (dSv_save_c::mReserve, sv +0x724, 0x50 bytes), which is written
// to the card and read back with the Quest Log (dSv_info_c::memory_to_card 025BA9FC, card_to_memory 025BA7B0),
// zeroed for a new file, and read by nothing in the game
uint32_t save();
// a price paid as the game's shops pay it: into the rupee meter's count to come (play +0x5B48, s32, as Bms1's buy at
// 0220B2C8), which the meter then counts off the wallet; the wallet itself (sv +4) written straight was put back by it
void pay(int rupees) { st32(kPlay + 0x5B48, ld32(kPlay + 0x5B48) - rupees); }
int wallet() { return ld16(save() + 4) + (int32_t)ld32(kPlay + 0x5B48); }  // what Link has once the meter is done
uint32_t upgrade_byte() { return save() + 0x773; }
int level(int k) { return (ld8(upgrade_byte()) >> (2 * k)) & 3; }  // as bought (0..3), the toggle on or off
int upgrade(int k) { return g_on[kUpgrades].load() ? level(k) : 0; }  // as it works
// Tetra's ship, bought at the Bomb Shop on Windfall (bit 0 of the reserve block's byte before, kept the same way)
constexpr int kShipPrice = 2500;
bool owns_ship() { return ld8(save() + 0x772) & 1; }
// The shop: our panel over the game (overlay::show_panel), drawn on the render thread from what the game step last
// told it (levels, rupees, a line of news); a buy it asks for (g_buy) is made by the game step
const char* const kUpName[] = {"Faster sailing", "Faster guns", "Bigger shots"};
std::atomic<int> g_shop_level[3], g_shop_rupees{0}, g_shop_sel{0}, g_buy{-1};
std::atomic<const char*> g_shop_news{""};
bool shop_draw(const float* pad) {
    // the controller as the game sees it (the player's controls: on a Nintendo layout A is the right-hand button), its
    // stick as up and down; and the keyboard's own keys
    const input::PadState st = input_map::controller_state(input_map::current(), pad);
    const uint32_t now = st.buttons | input::live_buttons() | (st.ly > 0.5f ? input::kUp : 0) | (st.ly < -0.5f ? input::kDown : 0);
    static uint32_t prev = 0;
    static bool keys_prev[256], fresh = true;
    bool keys[256];
    input::held_keys(keys);
    if (fresh) prev = now, std::copy(keys, keys + 256, keys_prev), fresh = false;  // (the A that opened it)
    auto hit = [&](uint32_t b, std::initializer_list<int> ks) {
        bool k = false;
        for (int c : ks) k |= keys[c] && !keys_prev[c];
        return (now & b && !(prev & b)) || k;
    };
    int sel = g_shop_sel;
    if (hit(input::kDown, {kVK_ANSI_S, kVK_DownArrow})) sel = (sel + 1) % 3;
    if (hit(input::kUp, {kVK_ANSI_W, kVK_UpArrow})) sel = (sel + 2) % 3;
    g_shop_sel = sel;
    if (hit(input::kA, {kVK_ANSI_K, kVK_Space, kVK_Return})) g_buy = sel;
    const bool close = hit(input::kB, {kVK_ANSI_J, kVK_Escape});
    prev = now, std::copy(keys, keys + 256, keys_prev);
    if (close) fresh = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##ship_shop", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        ImGui::TextUnformatted("Zunari's ship fittings");
        ImGui::Separator();
        for (int i = 0; i < 3; i++) {
            const int lv = g_shop_level[i];
            char line[96];
            if (lv >= 3) snprintf(line, sizeof line, "%-15s  level 3 (best)", kUpName[i]);
            else snprintf(line, sizeof line, "%-15s  level %d -> %d   %d rupees", kUpName[i], lv, lv + 1, kUpPrice[lv]);
            ImGui::Selectable(line, i == sel);
        }
        ImGui::Separator();
        ImGui::Text("You have %d rupees", g_shop_rupees.load());
        if (const char* news = g_shop_news; *news) ImGui::TextUnformatted(news);
        ImGui::TextDisabled("A buy    B leave");
    }
    ImGui::End();
    return close;
}
// Tetra's ship for sale: the Bomb Shop's panel, as the shop's (above)
std::atomic<bool> g_ship_buy{false}, g_ship_sold{false};  // asked for; hers already (told by the game step)
bool ship_sale_draw(const float* pad) {
    const input::PadState st = input_map::controller_state(input_map::current(), pad);
    const uint32_t now = st.buttons | input::live_buttons();
    static uint32_t prev = 0;
    static bool keys_prev[256], fresh = true;
    bool keys[256];
    input::held_keys(keys);
    if (fresh) prev = now, std::copy(keys, keys + 256, keys_prev), fresh = false;
    auto hit = [&](uint32_t b, std::initializer_list<int> ks) {
        bool k = false;
        for (int c : ks) k |= keys[c] && !keys_prev[c];
        return (now & b && !(prev & b)) || k;
    };
    if (hit(input::kA, {kVK_ANSI_K, kVK_Space, kVK_Return})) g_ship_buy = true;
    const bool close = hit(input::kB, {kVK_ANSI_J, kVK_Escape});
    prev = now, std::copy(keys, keys + 256, keys_prev);
    if (close) fresh = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##ship_sale", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        ImGui::TextUnformatted("For sale, no questions asked");
        ImGui::Separator();
        ImGui::Selectable(g_ship_sold ? "Tetra's ship   SOLD" : "Tetra's ship   2500 rupees", true);
        ImGui::Separator();
        ImGui::Text("You have %d rupees", g_shop_rupees.load());
        if (const char* news = g_shop_news; *news) ImGui::TextUnformatted(news);
        ImGui::TextDisabled("A buy    B leave");
    }
    ImGui::End();
    return close;
}
// The cabin below Tetra's deck (the stage her door leads to): our panel there, drawn as the shop's: sleep the clock
// round to morning or nightfall, the bottles filled with Elixir Soup (once a visit), the ship's bomb store
const char* const kCabinLine[] = {"Sleep until morning", "Sleep until nightfall", "Fill empty bottles with soup", "Store your bombs",
                                  "Take bombs"};
std::atomic<int> g_cabin_sel{0}, g_cabin_do{-1}, g_cabin_bombs{0};
bool cabin_draw(const float* pad) {
    const input::PadState st = input_map::controller_state(input_map::current(), pad);
    const uint32_t now = st.buttons | input::live_buttons() | (st.ly > 0.5f ? input::kUp : 0) | (st.ly < -0.5f ? input::kDown : 0);
    static uint32_t prev = 0;
    static bool keys_prev[256], fresh = true;
    bool keys[256];
    input::held_keys(keys);
    if (fresh) prev = now, std::copy(keys, keys + 256, keys_prev), fresh = false;
    auto hit = [&](uint32_t b, std::initializer_list<int> ks) {
        bool k = false;
        for (int c : ks) k |= keys[c] && !keys_prev[c];
        return (now & b && !(prev & b)) || k;
    };
    constexpr int n = (int)std::size(kCabinLine);
    int sel = g_cabin_sel;
    if (hit(input::kDown, {kVK_ANSI_S, kVK_DownArrow})) sel = (sel + 1) % n;
    if (hit(input::kUp, {kVK_ANSI_W, kVK_UpArrow})) sel = (sel + n - 1) % n;
    g_cabin_sel = sel;
    if (hit(input::kA, {kVK_ANSI_K, kVK_Space, kVK_Return})) g_cabin_do = sel;
    const bool close = hit(input::kB, {kVK_ANSI_J, kVK_Escape});
    prev = now, std::copy(keys, keys + 256, keys_prev);
    if (close) fresh = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##cabin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        ImGui::TextUnformatted("Tetra's cabin");
        ImGui::Separator();
        for (int i = 0; i < n; i++) ImGui::Selectable(kCabinLine[i], i == sel);
        ImGui::Separator();
        ImGui::Text("Bombs in the ship's store: %d", g_cabin_bombs.load());
        if (const char* news = g_shop_news; *news) ImGui::TextUnformatted(news);
        ImGui::TextDisabled("A choose    B leave (Save from the pause menu, as anywhere)");
    }
    ImGui::End();
    return close;
}
// Warp: places on the sea stage, by the sea's 7x7 grid of 100000 squares (room = row * 7 + column + 1, the centre
// square room 25 at the origin): Link arrives there on his boat; with `near` (an actor's process name, e.g. Zunari's
// daNpc_Rsh1_c 0x160) he then goes on, on foot, to beside it once its room has made it (a second stage change)
struct Place { const char* name; float x, z; int16_t near; };
constexpr Place kPlaces[] = {
    {"Windfall: Zunari's stall", 0, -191000, 0x160}, {"Windfall, by boat", 0, -191000, 0}, {"Outset, by boat", -200000, 291000, 0},
    {"Open sea, south-west", -160000, 240000, 0},    {"Open sea, north-east", 150000, -250000, 0},
};
int sea_room(float x, float z) { return (int)std::lround(z / 1e5 + 3) * 7 + (int)std::lround(x / 1e5 + 3) + 1; }
std::atomic<int> g_warp_sel{0}, g_warp_go{-1};
bool warp_draw(const float* pad) {
    const input::PadState st = input_map::controller_state(input_map::current(), pad);
    const uint32_t now = st.buttons | input::live_buttons() | (st.ly > 0.5f ? input::kUp : 0) | (st.ly < -0.5f ? input::kDown : 0);
    static uint32_t prev = 0;
    static bool keys_prev[256], fresh = true;
    bool keys[256];
    input::held_keys(keys);
    if (fresh) prev = now, std::copy(keys, keys + 256, keys_prev), fresh = false;
    auto hit = [&](uint32_t b, std::initializer_list<int> ks) {
        bool k = false;
        for (int c : ks) k |= keys[c] && !keys_prev[c];
        return (now & b && !(prev & b)) || k;
    };
    constexpr int n = (int)std::size(kPlaces);
    int sel = g_warp_sel;
    if (hit(input::kDown, {kVK_ANSI_S, kVK_DownArrow})) sel = (sel + 1) % n;
    if (hit(input::kUp, {kVK_ANSI_W, kVK_UpArrow})) sel = (sel + n - 1) % n;
    g_warp_sel = sel;
    const bool go = hit(input::kA, {kVK_ANSI_K, kVK_Space, kVK_Return}), close = go || hit(input::kB, {kVK_ANSI_J, kVK_Escape, kVK_F9});
    if (go) g_warp_go = sel;
    prev = now, std::copy(keys, keys + 256, keys_prev);
    if (close) fresh = true;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##warp", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        ImGui::TextUnformatted("Warp to");
        ImGui::Separator();
        for (int i = 0; i < n; i++) ImGui::Selectable(kPlaces[i].name, i == sel);
        ImGui::Separator();
        ImGui::TextDisabled("A go    B back");
    }
    ImGui::End();
    return close;
}
int g_leviathan_pool = kLeviathanHealth, g_molgera_pool = kMolgeraHealth;
bool g_leviathan_slain = false;
double g_step_time = 0;  // game time at the last step, for the hooks
bool g_molgera_dying = false;     // slain, diving under (hook_020FF80C lets it dive at its own speed)
int32_t g_molgera_pitch = -1;     // its pitch before a return to its flight's start, which points it straight up: put back
bool g_molgera_gave_up = false;  // it has stopped hunting him (the event ends once he is 5000 away)
double g_molgera_pace = 1;        // its circling flight's speed, x its own 40 a step (hook_020FF80C)
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
// `side`: 0 anywhere around him, 1 behind him as the camera looks (out of its view), -1 ahead; `sea`: `dy` from the sea (y 0), not
// from Link, who may be on the Big ship's deck or up its mast; `deck`: on the deck he stands on, within 60 of his
// height (a low deck, the raft's or the giant Lion's back, is less than 250 above the sea, which would pass as ground)
// `m` made at a place, facing `yaw`, in Link's room: created in Link's process layer (base_process_class::mLyTg.mpLayer,
// +0x2C), as an actor's own spawns are: here, after the executor, the current layer is the root one, and an actor made
// there was drawn outside the play scene's draw buffers (a J3D draw list looped through the grass packet the first time
// it came into view); `parent`: a child of that process (fopAcM_createChild)
uint32_t create_at(Cpu* c, Monster m, uint32_t link, float x, float y, float z, int16_t yaw, uint32_t parent) {
    static const uint32_t buf = mem::host_alloc(0x20);  // pos (cXyz) at +0, angle (csXyz) at +0x10
    st32(buf, u32_bits(x)), st32(buf + 4, u32_bits(y)), st32(buf + 8, u32_bits(z));
    st16(buf + 0x10, (uint16_t)m.angle_x), st16(buf + 0x12, (uint16_t)yaw), st16(buf + 0x14, 0);
    const uint32_t layer = guest_call(c, GC(0x025DED64));  // fpcLy_CurrentLayer
    guest_call(c, GC(0x025DEAB4), {ld32(link + 0x2C)});    // fpcLy_SetCurrentLayer
    const uint32_t room = (uint32_t)(int8_t)ld8(link + kRoom), subtype = (uint32_t)(int32_t)m.subtype;
    // fopAcM_createChild's 9th argument, createFunc, is on the stack (caller's sp + 8; guest_call's frame is 0x40)
    if (parent) st32(c->r[1] - 0x40 + 8, 0);
    const uint32_t id = parent ? guest_call(c, GC(0x025D5A20), {(uint32_t)m.proc, parent, m.param, buf, room, buf + 0x10, 0, subtype})
                               : guest_call(c, GC(0x025D5834), {(uint32_t)m.proc, m.param, buf, room, buf + 0x10, 0, subtype, 0});
    guest_call(c, GC(0x025DEAB4), {layer});
    return id;
}
// online co-op (below): the host's monsters made or taken for a guest's
bool coop_shared(const Monster& m);
uint32_t coop_spawned(Cpu* c, Monster m, uint32_t id, uint32_t parent, float x, float y, float z, int16_t yaw);
uint32_t coop_adopt(Monster m, Spawner& s);

uint32_t spawn(Cpu* c, Spawner& s, Monster m, uint32_t link, float dist, int side, float dy, bool on_ground, uint32_t parent = 0, bool sea = false,
               bool deck = false) {
    static const bool seeded = (srand((unsigned)time(nullptr)), true);
    (void)seeded;
    if (mods::coop::sync::guest() && coop_shared(m)) return coop_adopt(m, s);  // a guest's monsters are the host's
    static const uint32_t buf = mem::host_alloc(0x10);  // the ground check's place
    const float ly = sea ? 0.f : (float)ldf32(link + kPos + 4);
    for (int attempt = 0; attempt < 6; attempt++) {
        const double yaw = (side ? (int16_t)g_cam_yaw.load() : (int16_t)ld16(link + kAngleY)) * M_PI / 32768;  // (the camera's way)
        const double a = side && attempt < 3 ? yaw + (side > 0 ? M_PI : 0) + (rand() / (double)RAND_MAX - 0.5) * 1.6 : rand() / (double)RAND_MAX * 2 * M_PI;
        const float x = (float)ldf32(link + kPos) + (float)(sin(a) * dist), z = (float)ldf32(link + kPos + 8) + (float)(cos(a) * dist);
        st32(buf, u32_bits(x));
        st32(buf + 4, u32_bits(ly + dy));
        st32(buf + 8, u32_bits(z));
        if (on_ground) {
            st32(buf + 4, u32_bits(ly + 250));  // look down from above Link's height
            guest_call(c, GC(0x02549F4C), {buf});
            const float ground = (float)c->f[1].ps0;
            if (ground == ly + 250 || ground < 5 || std::fabs(ground - ly) > (deck ? 60 : 250)) continue;  // nothing, water or a drop
            st32(buf + 4, u32_bits(ground + 5));
        }
        const float y = (float)ldf32(buf + 4);
        const int16_t face = (int16_t)(atan2(-sin(a), -cos(a)) * 32768 / M_PI);  // facing Link
        const uint32_t id = create_at(c, m, link, x, y, z, face, parent);
        trace("extras: spawned proc %#x id %#x at %.0f %.0f %.0f (%zu out)", m.proc, id, x, y, z, s.ids.size());
        if (id != 0xFFFFFFFF) s.ids.push_back(id);
        return coop_spawned(c, m, id, parent, x, y, z, face);
    }
    trace("extras: no ground for proc %#x near Link", m.proc);
    return 0xFFFFFFFF;
}

// Big ship: Tetra's ship (daObjPirateship type 3: its deck a moving BG, Kaizokusen room1b.dzb, re-made from current.pos
// and shape_angle.y each execute, which carries Link standing on it) sails beside the King of Red Lions. Its frame
// (cMtx_YrotS): local +x is (cos a, -sin a) in the world, +z (forward) (sin a, cos a); the main deck at local y 400
// (x +-350, z -700..650), the gunwale top 126 above it (x 350..500), the hull's side out to x 700 at the waterline
struct V3 { double x, y, z; };
V3 to_local(uint32_t hull, V3 p) {
    const double t = (int16_t)ld16(hull + kAngleY) * M_PI / 32768;
    const double dx = p.x - ldf32(hull + kPos), dz = p.z - ldf32(hull + kPos + 8);
    return {dx * cos(t) - dz * sin(t), p.y - ldf32(hull + kPos + 4), dx * sin(t) + dz * cos(t)};
}
V3 to_local(uint32_t hull, uint32_t a) { return to_local(hull, V3{ldf32(a + kPos), ldf32(a + kPos + 4), ldf32(a + kPos + 8)}); }
V3 to_world(V3 o, int16_t yaw, V3 l) {
    const double t = yaw * M_PI / 32768;
    return {o.x + l.x * cos(t) + l.z * sin(t), o.y + l.y, o.z - l.x * sin(t) + l.z * cos(t)};
}
V3 pos_of(uint32_t a) { return {ldf32(a + kPos), ldf32(a + kPos + 4), ldf32(a + kPos + 8)}; }
// online co-op (below): a shot fired here, an event's sign, start or end on the host, the host's ship on a guest
void coop_fired(Cpu* c, V3 from, int16_t yaw, int16_t up);
void coop_event(int op, int k, double now, int to = -1);
bool coop_host_ship(V3& pos, int16_t& yaw, bool& sail);
// back up from Tetra's cabin: where she lay, for the Big ship to be made there again
struct CabinReturn { V3 ship; int16_t yaw; bool pending; } g_cabin_return{};
// Link put somewhere by a mod: every copy of his position (current +0x314, old +0x300, his collision's +0x84C, +0x68B0,
// +0x7350) and the one d_a_player keeps (1046CD48), which his next execute reads back (without it he is back where he
// was; without his collision's he falls through the floor he is put above)
void put_link(uint32_t link, V3 w) {
    for (uint32_t a : {link + kPos, link + 0x300, link + 0x84C, link + 0x68B0, link + 0x7350, GD(0x1046CD48)})
        st32(a, u32_bits((float)w.x)), st32(a + 4, u32_bits((float)w.y)), st32(a + 8, u32_bits((float)w.z));
}
// a leap of Link's onto a ship: carried on an arc (every copy of his position, each step after his own execute) from
// where he jumps to a spot in the ship's frame (followed as it moves), 250 over the higher end, in 36 steps
struct Leap {
    int step = -1;  // steps into it; -1 none
    V3 from, onto;  // onto: in the ship's frame
    uint32_t ship = 0;
    void start(uint32_t link, uint32_t s, V3 local) { step = 0, from = pos_of(link), ship = s, onto = local; }
    // a step of it; true on the step it lands
    bool carry(uint32_t link) {
        if (step < 0) return false;
        constexpr int kSteps = 36;
        const V3 to = to_world(pos_of(ship), (int16_t)ld16(ship + kAngleY), onto);
        const double k = ++step / (double)kSteps, top = std::max(from.y, to.y) + 250;
        const double y = from.y + (to.y - from.y) * k + 4 * (top - (from.y + to.y) / 2) * k * (1 - k);
        put_link(link, {from.x + (to.x - from.x) * k, y, from.z + (to.z - from.z) * k});
        st32(link + 0x340, 0), st32(link + 0x6A14, 0);
        if (step < kSteps) return false;
        step = -1;
        return true;
    }
};
constexpr double kDeckY = 400;
bool on_deck(uint32_t hull, uint32_t link) {  // anywhere on board: the main deck, the castles, the crow's nest
    const V3 l = to_local(hull, link);
    return std::fabs(l.x) < 720 && l.z > -1800 && l.z < 2250 && l.y > kDeckY - 30 && l.y < 2500;
}
// A gun: where it stands in the ship's frame, which way it rests (0 over the bow, 0x8000 the stern), and, for one up on
// a roof, the deck spot it is manned from (`lift`)
struct Gun { V3 at; int16_t face; bool lift = false; V3 man{}; };
// The craft in tow (both daObj_Ikada, an IkadaH model): its scale, how far it sits lowered, its deck's height and half
// sizes in its frame, its centre's distance behind the King of Red Lions' centre, its bow (the rope's end), where a
// leap lands on it and the box a climb out of the water lands in (x within +-climb_x, z climb_z0..climb_z1), its guns.
// Beedle's ship (vtsp), 1.5x and lowered 60: its deck (found with the ground check) a strip x +-300 from z 0 to 900
// (y 50..130, the bow rising to 170), Beedle's hut over z -300..300 (its floor 520). The raft (vikae), at 1x: flat at
// 60 (x +-425, z +-350; its rim at 40 out to x +-510, z +-450), its mast in the middle. In tow: its centre kept `tow` behind
// his, on the line from it to him (a rigid tow-shaft from his stern to its bow), so it swings in behind him as he
// turns, as a trailer does
struct Craft {
    Monster m;
    float scale;
    double dy, top, half_x, half_z, tow, bow, front, onto_z, climb_x, climb_z0, climb_z1;  // front: A beyond it leaps to the boat
    Gun guns[2];
};
constexpr Craft kBeedleCraft{kBeedleShip, 1.5f, -60, 130 * 1.5 - 60, 575 * 1.5, 635 * 1.5, 1500, 900, 760, 700, 200, 500, 1000,
                             {{{-120, 0, 880}, 0}, {{0, 0, -60}, (int16_t)0x8000, true, {0, 0, 560}}}};
// the raft 2x (a deck about 1700 x 1400, 2000 x 1800 with its rim), lowered 40
constexpr Craft kRaftCraft{kRaftShip, 2.f, -40, 60 * 2 - 40, 1020, 900, 1450, 880, 650, 300, 600, 300, 650,
                           {{{760, 0, 550}, (int16_t)0x4000}, {{-760, 0, -550}, (int16_t)0xC000}}};  // right and left, at its edges, off the mast's line
const Craft* g_craft = &kBeedleCraft;  // the one in tow (or last in tow)
bool on_beedle(uint32_t ship, uint32_t link) {  // anywhere on board, the hut's roof too
    const V3 l = to_local(ship, link);
    return std::fabs(l.x) < g_craft->half_x && std::fabs(l.z) < g_craft->half_z && l.y > -40 && l.y < 1200;
}
// the GamePad as the game reads it (filter_pad, below): what was held at the last read and everything pressed since
// the last step; while a deck cannon is manned its stick, A, B and ZL are the cannon's, not the game's
std::atomic<uint32_t> g_pad_hold{0}, g_pad_seen{0};
std::atomic<float> g_pad_lx{0}, g_pad_ly{0};
std::atomic<bool> g_take_pad{false};
std::atomic<bool> g_at_helm{false};
// the free camera (F7): flying, and the pad it flies by (filter_pad keeps it from the game)
std::atomic<bool> g_free_cam{false};
std::atomic<float> g_free_lx{0}, g_free_ly{0}, g_free_rx{0}, g_free_ry{0};
std::atomic<uint32_t> g_free_buttons{0};  // the pad is the helm's: ZL is left to the game (its camera is forced instead)
// a manned deck cannon, for the camera (camera_draw): where it stands, which way it points (s16 yaw, elevation)
std::atomic<bool> g_cannon_cam{false};
std::atomic<float> g_cannon_x{0}, g_cannon_y{0}, g_cannon_z{0};
std::atomic<int> g_cannon_yaw{0}, g_cannon_up{0};
// the button the Bomb Bag is on (play +0x5BBB/C/D: the X, Y and R items; 0x31 the Bomb Bag) while Link stands by a deck
// cannon, which then mans it instead of taking out a bomb, as on the boat
std::atomic<uint32_t> g_cannon_btn{0};
uint32_t bomb_button() {
    const uint32_t play = GD(0x1046F0B0);
    return ld8(play + 0x5BBB) == 0x31 ? input::kX : ld8(play + 0x5BBC) == 0x31 ? input::kY : ld8(play + 0x5BBD) == 0x31 ? input::kR : 0;
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
        {"big-ship", "Big ship", "Experimental",
         "Tetra's ship, a boat of its own, once bought at the Bomb Shop on Windfall (2500 rupees: talk to the shopkeeper). "
         "Out at sea she waits at anchor near the King of Red Lions, in place of the raft or Beedle's ship. Stop beside her and get "
         "out to jump onto its deck; walk it, fight from it, man its cannons, and sail it from its wheel while he comes "
         "along beside it. Her door leads below to the cabin: sleep until morning or nightfall, free soup for your empty "
         "bottles, and a store for bombs. Leave it far behind and it waits near you again the next time you are out at sea.", {}},
        {"horror-audio", "Horror audio", "Spooky",
         "While a spooky event's monsters are about, the music sinks low, and a heartbeat sounds that quickens as "
         "one of them comes close.", {}},
        {"deep", "Fog and the Deep", "Spooky",
         "Spells when, far from land, the sea and sky darken and a haze closes in the further Link sails from the "
         "nearest island, so the islands stand out like lighthouses.", {}, true},
        {"leviathan", "Leviathan", "Spooky",
         "Rarely, far out at sea, the wind dies, fog rolls in and a Gyorg eight times the size of any other surfaces "
         "behind the boat and circles it like a shark, keeping up however fast Link sails. Make for land, hold out until "
         "it loses interest, or kill it (it takes a lot of hitting) for 200 rupees and a blue potion.", {}, true},
        {"molgera", "Molgera of the deep", "Spooky",
         "Very rarely, far out at sea, the wind dies, fog rolls in and Molgera, the Wind Temple's sand worm and bigger "
         "than in its lair, comes for Link over the water from far off, circling him as a Gyorg does and charging in to bite, "
         "then round again; when it dies it bursts apart as it dives under. "
         "It is faster than the boat, however fast she sails, and hunts Link until he makes land: run for an "
         "island, or fight it (it takes a lot of hitting) for 300 rupees and a blue potion.", {}, true},
        {"phantom-stalker", "Phantom stalker", "Spooky",
         "Rarely, on an island at night, fog rolls in and something hunts Link across the island, until he takes to the "
         "sea, beats it, or it fades back into the fog: a Shadow Beast from Twilight Princess, which drops from the sky "
         "and fights as it does there, Phantom Ganon, who comes down behind him, or Twilight Princess's Darknut, who "
         "walks out of the fog after him (the Twilight Princess ones need game/tp_*.bin, made from the Twilight Princess "
         "HD dump by tools/tp_monsters.py; Phantom Ganon comes without them).",
         {"Shadow Beast", "Phantom Ganon", "Darknut"}, true},
        {"beedle-ship", "Beedle's ship", "Experimental",
         "Out at sea the King of Red Lions tows Beedle's shop ship, a size bigger, behind him on a tow-shaft. Stop and get "
         "out to leap back onto its deck, walk it and fight from it with its two cannons (one at the bow, one up on the "
         "hut's roof, manned from before the hut); A at its bow leaps Link forward into the boat. Fall in, and A by its "
         "side climbs back aboard.", {}},
        {"dead-calm", "Dead calm", "Spooky",
         "Now and then, far out at sea, the wind dies away altogether and a still fog settles: the sail hangs slack, the "
         "Wind Waker can turn the wind but not wake it, and the music fades. Drift on, wait it out, or reach an island.",
         {}, true},
        {"photo-mode", "Free camera", "Gameplay",
         "F7 lets the camera go: fly it with the left stick (WASD), look with the right stick (arrow keys), L and R (Q, E) "
         "down and up, ZL (Shift) held for speed. Link stands still and the world goes on; F7 again gives the camera back.", {}},
        {"second-gunner", "Second gunner", "Experimental",
         "With a second controller connected, player 2 is a gunner on Beedle's ship or the Big ship: the gun Link is not "
         "at is theirs. Left stick turns and raises it, ZL locks it on the nearest monster (again: the next nearest, and off after the last), A or ZR "
         "fires, from the same Bomb Bag. Player 1 sails and plays on as ever on the first controller.", {}},
        {"blood-moon", "Blood moon", "Spooky",
         "On some nights, most of all at full moon, the moon rises red: the whole world and the sky turn blood red until "
         "dawn, the music sinks low, and with Night of the dead on the dead rise that night, twice as fast.", {}, true},
        {"cyclones", "Wandering cyclones", "Gameplay",
         "Now and then, out at sea, one of Cyclos's cyclones wanders across Link's course. Steer clear: sail within 3000 "
         "of it and it takes the boat up and throws it to another part of the Great Sea. Works even after Cyclos is beaten.",
         {}, true},
        {"maelstrom", "Maelstrom", "Spooky",
         "Now and then, far out at sea, the water beside the boat begins to turn: a whirlpool opens and drags the boat round "
         "and in. Wrench the stick from side to side to break free; held for 45 seconds, it takes two hearts before it "
         "lets go.", {}, true},
        {"armada", "Pirate armada", "Spooky",
         "Now and then, far out at sea, distant gunfire, then four cannon boats come over the horizon one after another, "
         "each from its own side: they close in, heave to and open fire, following if Link sails off. Sink all four (with "
         "the boat's cannon, bombs or a ship's deck cannons) for 200 rupees and a blue potion.", {}, true},
        {"big-octo", "Big Octo", "Spooky",
         "Now and then, far out at sea at night, the sea goes still and dark, then a whirlpool opens by the boat and Big "
         "Octo rises in it, as in its own fight, and Gyorgs with it, ramming the boat. Shoot out its eyes (the Boomerang, "
         "arrows, bombs) as it sweeps you round, for 300 rupees and a blue potion.", {}, true},
        {"wrong-lion", "Something in the Lion", "Spooky",
         "Very rarely, sailing at night, the King of Red Lions laughs a laugh that is not his, swerves off and races on by "
         "himself for a few seconds, then sails on as if nothing happened.", {}, true},
        {"drowned", "The drowned", "Spooky",
         "Now and then, at night on the deck of Beedle's ship or the Big ship, the drowned climb aboard out of the sea: "
         "ReDeads, one after another, for a couple of minutes.", {}, true},
        {"boarding", "Boarding party", "Spooky",
         "Cannon boats that come within 2500 of the ship Link stands on send Bokoblins leaping across onto its deck, two "
         "from each boat, spoiling for a fight.", {}},
        {"raft", "Raft in tow", "Experimental",
         "Out at sea the King of Red Lions tows a big flagged raft behind him on a rope. Stop and get out to leap onto "
         "it, walk it and fight from its two guns; A at its front leaps Link back to the boat. Fall in, and A by its side "
         "climbs back aboard. With Beedle's ship also on, the raft is the one towed.", {}},
        {"fair-wind", "Fair wind", "Gameplay",
         "Out sailing, the wind always blows the way the boat is heading, so the sail is always full whichever way you go.", {}},
        {"scare-pacing", "Scare pacing", "Spooky",
         "How long the sea stays quiet after a scare before the next can begin. Off, scares are rare: about 12 quiet "
         "minutes after each. None comes in the first 4 minutes after a file loads, and only one at a time.",
         {"Now and then (6 min)", "Often (3 min)", "Very rare (20 min)"}},
        {"ship-upgrades", "Ship upgrades", "Gameplay",
         "Faster sailing, faster guns and bigger shots for your ship, three levels each (500, 900 and 1500 rupees), kept in "
         "your Quest Log when you save. Beedle sells them in his shop ships: the sail fitting where Red Potions stood, the "
         "next cannon fitting where 30 Bombs stand. Zunari on Windfall (who sells the sail) also fits them, after you talk to him.",
         {}},
        {"warp", "Warp", "Gameplay",
         "F9 opens a list of places (Zunari's stall on Windfall, Outset, open sea far from land); pick one and Link is "
         "there, on his boat or, at a stall, on foot beside it. Not during a cutscene or with a menu open.", {}},
        {"helmaroc", "Helmaroc King", "Spooky",
         "Now and then, far out at sea, the wind dies and the Helmaroc King comes down out of the sky to hunt the boat: he "
         "circles high and dives at Link. Bring him down with arrows, bombs, the Boomerang or a ship's guns for 300 rupees "
         "and a blue potion. Hold out too long and he snatches the boat, Link and all, and drops him somewhere on the Great "
         "Sea: in the open water, or on an island's shore.", {}, true},
        {"coop", "Online co-op", "Online",
         "Play with friends: one hosts their game, the others join it over the internet (Tailscale) or the local network "
         "and play in the host's world, drawn as Link: on Tetra's ship together, the same sea events and monsters. A guest's "
         "own Quest Logs are never saved to; when the host leaves, the guest goes back to the title screen. Both need the "
         "same build of the game. Work in progress (docs/multiplayer.md).", {"Host", "Join"}},
        {"tp-darknuts", "Twilight Princess Darknuts", "Gameplay",
         "Every Darknut is Twilight Princess's: its armour and two swords, its moves (the cut, the overhead slam, the "
         "shield bash), a piece of armour flying off each time you hurt it, and once its armour is gone the leap back, "
         "its sword thrown at you and the second one drawn; its sounds and sparks too. Needs game/tp_darknut.bin, made "
         "from the Twilight Princess HD dump by tools/tp_monsters.py.", {}},
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
// Test aid: WWHD_SPOOKY=<sea|giants|phantom|dead|storm|fog|deep|leviathan|molgera|stalker|calm|blood|cyclone|maelstrom|armada|octo|lion|drowned>@<game seconds>[;...] starts that event at that time
// (@+<seconds>: that long after Link is first in play).
namespace {
constexpr int kAlways = 3;
constexpr double kScale[] = {0.4, 1, 3};       // the chances: Rare, Now and then, Often
double g_until[kCount], g_cool;                // each event's end (game time); the sea events' cooldown
int g_warn = -1;                               // the event the warning signs are of
double g_warn_end, g_warn_far, g_out_since;    // when it comes; how far off its place was; when Link last stood on land
double g_warn_x, g_warn_z;                     // its place, ahead of Link: sailing away from it calls it off
double g_file_at = -1;                         // when the file being played was loaded (-1: at the title)
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
const char* event_name(int k) { return catalogue()[k].name.c_str(); }
// Molgera (bwd_class, proc 0xD9; HD fields = decomp + 0x284 up to +0x1B5C): its l_HIO (10462A68) holds the head's
// scale (+0x1C, 1.3, also its 9 spheres' radii), the push on Link while it waits (+0x0C, 6.0) and its bite's reach
// (+0x14, 700), set while it is out
// and put back after. It leaves behind its lock-on helper (ATT, its process id at +0x1E38) and, if it made one, its
// sand floor (Bwdg, proc 0xDB, a BG mesh that reads Molgera every frame): both go with it
constexpr uint32_t kMolgeraScale = 0x10462A84, kMolgeraPush = 0x10462A74, kMolgeraBite = 0x10462A7C;
// a kill's loot: rupees through the game's own item get (execItemGet 0254DA38: silver 0x0F 200, orange 6 100), which
// the meter counts into the wallet up to what it holds (and the Rupee multiplier then sees), with the rupee sound
// (0x836); and a blue potion in the first empty bottle (items 14..17, an empty bottle 0x50; item_func_blue_bottle
// 0254F3BC fills it), with the pickup sound (0x827)
void loot(Cpu* c, int rupees) {
    for (; rupees >= 100; rupees -= rupees >= 200 ? 200 : 100) guest_call(c, GC(0x0254DA38), {rupees >= 200 ? 0x0Fu : 6u});
    guest_call(c, GC(0x025E1988), {0x836});
    const uint32_t sv = save();
    for (int i = 14; i < 18; i++)
        if (ld8(sv + 0x3C + i) == 0x50) {
            guest_call(c, GC(0x0254F3BC));
            guest_call(c, GC(0x025E1988), {0x827});
            break;
        }
}
// an effect (dPa_control_c::set 025A847C on play +0x5AB0: group 0, id, pos, angle, scale, alpha 0xFF, no callback;
// on the stack the room -1 and no colours)
void effect(Cpu* c, uint16_t id, V3 at, float scale) {
    static const uint32_t buf = mem::host_alloc(0x20);  // pos +0, angle +0xC, scale +0x14
    st32(buf, u32_bits((float)at.x)), st32(buf + 4, u32_bits((float)at.y)), st32(buf + 8, u32_bits((float)at.z));
    st16(buf + 0xC, (uint16_t)rand()), st16(buf + 0xE, (uint16_t)rand()), st16(buf + 0x10, 0);
    for (int i = 0; i < 3; i++) st32(buf + 0x14 + 4 * i, u32_bits(scale));
    st32(c->r[1] - 0x40 + 8, 0xFFFFFFFF), st32(c->r[1] - 0x40 + 0xC, 0), st32(c->r[1] - 0x40 + 0x10, 0), st32(c->r[1] - 0x40 + 0x14, 0);
    guest_call(c, GC(0x025A847C), {ld32(kPlay + 0x5AB0), 0, id, buf, buf + 0xC, buf + 0x14, 0xFF, 0});
}
void remove_molgera(Cpu* c) {
    if (const uint32_t a = g_molgera.actor(c)) {
        guest_call(c, GC(0x025D57E4), {ld32(a + 0x1E38)});
        static const uint32_t found = mem::host_alloc(4);
        if (st32(found, 0); guest_call(c, GC(0x025D5578), {0xDB, found}) && ld32(found)) guest_call(c, GC(0x025D57E0), {ld32(found)});
        guest_call(c, GC(0x025D57E4), {g_molgera.id});
    }
    g_molgera = Single{}, g_molgera_actor = 0, g_molgera_gave_up = false, g_molgera_dying = false;
    st32(GD(kMolgeraScale), u32_bits(1.3f)), st32(GD(kMolgeraPush), u32_bits(6.f)), st32(GD(kMolgeraBite), u32_bits(700.f));
}
void wind(Cpu* c, float power) { c->f[1].ps0 = power; guest_call(c, GC(0x0257E594)); }
// a scare: everything but the moods (Night of the dead and Blood moon for a night, rain, fog, the Deep), which may come
// with one; one scare at a time, and after each the sea is quiet for a while (Scare pacing; off, 12 minutes)
bool scare(int k) { return k != kNight && k != kStorm && k != kFog && k != kDeep && k != kBlood; }
double quiet() {
    static constexpr double kQuiet[] = {360, 180, 1200};
    return on<kPacing>() ? kQuiet[g_choice[kPacing].load()] : 720;
}
bool scare_on(double now) {
    if (g_warn >= 0) return true;
    for (int k = 0; k < kCount; k++)
        if (scare(k) && g_until[k] > now && !always(k)) return true;
    return false;
}
void begin(int k, double now) {
    g_until[k] = now + (k == kPhantom || k == kLeviathan ? 180 : k == kMolgera ? 150 : k == kStalker ? 120 : k == kSea ? 120 + rand() % 120 : k == kCalm ? 60 + rand() % 90 : k == kCyclone ? 150 : k == kMaelstrom ? 90 : k == kArmada ? 240 : k == kOcto ? 180 : k == kWrongLion ? 6 + rand() % 4 : k == kDrowned ? 120 : k == kHelmaroc ? 150 : 180 + rand() % 300);
    if (k == kPhantom || k == kStalker) g_phantom = Single{};  // made afresh by their blocks below
    if (k == kLeviathan) g_leviathan = Single{};
    if (k == kMolgera) g_molgera = Single{};
    if (k == kHelmaroc) g_helmaroc = Single{};
    if (k == kSea) g_giant_spell = giant_spell();
    // a boss at sea: the Bomb Bag filled up (its count +0x6A to what it holds +0x70), for the deck cannons
    if (const uint32_t sv = save(); k == kPhantom || k == kLeviathan || k == kMolgera) st8(sv + 0x6A, ld8(sv + 0x70));
    trace("extras: event %s begins (%.0f s)%s", event_name(k), g_until[k] - now, k == kSea && g_giant_spell ? ", giants" : "");
    coop_event(1, k, now);
}
// one of ours deleted; a cannon boat with its flag (daMajuu_Flag, proc 0xAE, which its create makes; the decomp's
// mFlagPcId +0xD50, found here by its id, as HD moved it): the flag takes its matrix from inside the boat every step,
// and left behind it read freed memory and flew off to 1e36, halting the game (seen twice as Haunted sea ended)
void delete_actor(Cpu* c, uint32_t id) {
    if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}); a && ld16(a + 8) == kOship.proc)
        for (uint32_t o = 0xD00; o < 0xF00; o += 4)
            if (const uint32_t f = guest_call(c, GC(0x025DE50C), {ld32(a + o)}); f && f != a && ld16(f + 8) == 0xAE) {
                guest_call(c, GC(0x025D57E4), {ld32(a + o)});
                trace("extras: a cannon boat's flag goes with it (its id at +%#x)", o);
                break;
            }
    guest_call(c, GC(0x025D57E4), {id});
}
void finish(Cpu* c, int k, double now, const char* why) {
    trace("extras: event %s ends (%s)", event_name(k), why);
    coop_event(2, k, now);
    g_until[k] = 0;
    if (scare(k) && !always(k)) g_cool = now + quiet();
    if ((k == kPhantom || k == kStalker) && g_phantom.actor(c)) guest_call(c, GC(0x025D57E4), {g_phantom.id});  // fopAcM_delete_id
    if (k == kLeviathan && g_leviathan.actor(c)) guest_call(c, GC(0x025D57E4), {g_leviathan.id});
    if (k == kHelmaroc && g_helmaroc.actor(c)) guest_call(c, GC(0x025D57E4), {g_helmaroc.id});
    if (k == kMolgera) remove_molgera(c);
    if (k == kCyclone && g_cyclone.actor(c)) guest_call(c, GC(0x025D57E4), {g_cyclone.id});
    // the whirlpool lets go: told to fade (+0x434 0) from nearly nothing (+0x430), so the boat is free at its next step;
    // deleted once it has (its own delete leaves its wave effect, which it removes itself as it reaches 0)
    if (k == kOcto) {  // Big Octo sinks out of sight, and its whirlpool with it
        if (const uint32_t o = g_octo.actor(c)) {
            if (const uint32_t w = ld32(o + 0x2844); guest_call(c, GC(0x025DE50C), {w})) guest_call(c, GC(0x025D57E4), {w});
            guest_call(c, GC(0x025D57E4), {g_octo.id});
        }
        g_octo = Single{}, g_octo_actor = 0, g_octo_dead = false;
    }
    if (k == kArmada) {  // what is left of it sails off into the fog: gone
        for (uint32_t id : g_armada.ids) delete_actor(c, id);
        g_armada = Spawner{};
    }
    if (const uint32_t w = k == kMaelstrom ? g_whirl.actor(c) : 0)
        st8(w + 0x434, 0), st32(w + 0x430, u32_bits(0.01f)), g_whirl.next = now + 2, g_whirl_fading = true;
    if (k == kSea) {  // the spell's monsters break off: those out in the fog (cannon boats, the Ghost Ship) go
        const uint32_t link = ld32(kLinkPtr);
        for (uint32_t id : g_sea.ids)
            if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id});
                a && std::hypot(ldf32(a + kPos) - ldf32(link + kPos), ldf32(a + kPos + 8) - ldf32(link + kPos + 8)) > 2000)
                delete_actor(c, id);
        if (g_ghost.actor(c)) guest_call(c, GC(0x025D57E4), {g_ghost.id});
    }
}
struct Forced { int k; double at; bool after_play; bool end = false; };  // after_play: seconds after Link is first in play (@+s); end: "end-<name>" ends it then
std::vector<Forced> forced_events() {
    std::vector<Forced> v;
    const char* e = getenv("WWHD_SPOOKY");
    static const std::pair<const char*, int> kNames[] = {{"sea", kSea}, {"giants", kGiants}, {"phantom", kPhantom},
                                                         {"dead", kNight}, {"storm", kStorm}, {"fog", kFog}, {"deep", kDeep}, {"leviathan", kLeviathan}, {"molgera", kMolgera}, {"stalker", kStalker}, {"calm", kCalm}, {"blood", kBlood}, {"cyclone", kCyclone}, {"maelstrom", kMaelstrom}, {"armada", kArmada}, {"octo", kOcto}, {"lion", kWrongLion}, {"drowned", kDrowned}, {"helmaroc", kHelmaroc}};
    for (std::string rest = e ? e : ""; !rest.empty();) {
        const std::string item = rest.substr(0, rest.find(';'));
        rest = item.size() < rest.size() ? rest.substr(item.size() + 1) : "";
        const size_t at = item.find('@');
        const bool end = item.rfind("end-", 0) == 0;
        for (const auto& [name, k] : kNames)
            if (at != std::string::npos && item.compare(end ? 4 : 0, at - (end ? 4 : 0), name) == 0)
                v.push_back({k, atof(item.c_str() + at + 1 + (item[at + 1] == '+')), item[at + 1] == '+', end});
    }
    return v;
}

// The warning signs before a scare (kWarning seconds), `t` 0 to 1 as it nears: the wind dies away (before a cyclone it
// rises), and the fog that comes with a sea event rolls in (the fog block in step(), g_warn); before
// notes of its song drift over the water, before the armada there is far-off gunfire (the boat's cannon sound)
constexpr double kWarning = 30;
void omens(Cpu* c, int k, double now, double t) {
    static double next = 0;
    if (now < next) return;
    next = now + 2;
    wind(c, k == kCyclone ? (float)(0.4 + 0.6 * t) : (float)std::max(0.02, 0.3 * (1 - t)));
    if (k == kArmada && rand() % 3 == 0) guest_call(c, GC(0x025E1988), {0x2852});
}
// the events that come with warning signs, and their place: 7000 ahead of Link (the drowned's where he is: they come
// for the ship, which sails on with him, so only making land escapes them)
bool warned(int k) {
    return k == kSea || k == kPhantom || k == kLeviathan || k == kMolgera || k == kCalm || k == kCyclone || k == kMaelstrom ||
           k == kArmada || k == kOcto || k == kDrowned || k == kHelmaroc;
}
void warn(int k, uint32_t link, double now) {
    // (far enough ahead that sailing on reaches it as the signs end: at full sail with the upgrade the boat made 7000 in
    // 4 s; its speedF +0x370 a step, 30 a second, the 30 s of signs less 5)
    const uint32_t korl = ld32(kPlay + 0x5B3C);
    const double speed = in_mem2(korl) ? std::fabs((double)ldf32(korl + 0x370)) * 30 : 0;
    const double yaw = (int16_t)ld16(link + kAngleY) * M_PI / 32768, ahead = k == kDrowned ? 0 : std::max(7000.0, speed * (kWarning - 5));
    g_warn = k, g_warn_end = now + kWarning, g_warn_far = ahead;
    g_warn_x = ldf32(link + kPos) + sin(yaw) * ahead, g_warn_z = ldf32(link + kPos + 8) + cos(yaw) * ahead;
    trace("extras: signs of %s", event_name(k));
    coop_event(0, k, now);
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

    if (mods::coop::sync::guest()) {  // a guest's events are the host's (coop_world): none rolled or ended here
        if (g_warn >= 0) omens(c, g_warn, now, std::clamp((now - (g_warn_end - kWarning)) / kWarning, 0.0, 1.0));
        return;
    }
    static auto forced = forced_events();
    static const double in_play = now;  // the first step Link is in play (for the test aid's times)
    if (g_file_at < 0) g_file_at = now, g_cool = std::max(g_cool, now + 240);  // a file loaded: no scare in its first 4 minutes
    for (auto& f : forced) {
        if (f.after_play) f.at += in_play, f.after_play = false;
        if (f.at >= 0 && now >= f.at) {
            f.at = -1;
            if (f.end) { if (g_until[f.k] > 0) finish(c, f.k, now, "test"); }
            else if (f.k == kGiants) g_until[kSea] = 0, begin(kSea, now), g_giant_spell = true;
            else if (warned(f.k) && g_warn < 0) warn(f.k, link, now);
            else begin(f.k, now);
        }
    }
    // the sea events end on land, near an island, after their time, or (Phantom Ganon) once he is gone
    for (int k : {kSea, kPhantom, kLeviathan, kMolgera, kStalker, kCalm, kCyclone, kMaelstrom, kArmada, kOcto, kWrongLion, kDrowned, kHelmaroc})  // set to Always or switched off mid-event
        if (g_until[k] > 0 && (always(k) || !g_on[k].load())) finish(c, k, now, "its setting changed");
    for (int k : {kWrongLion, kDrowned})  // (so the quiet after a scare follows them too)
        if (g_until[k] > 0 && now >= g_until[k]) finish(c, k, now, "time");
    for (int k : {kSea, kPhantom, kLeviathan, kMolgera, kCalm, kCyclone, kMaelstrom, kArmada, kOcto, kHelmaroc})
        if (g_until[k] > 0) {
            if (ashore || isle < 3500) finish(c, k, now, ashore ? "Link is off the water" : "near an island");
            else if (now >= g_until[k]) finish(c, k, now, "time");
            else if (k == kPhantom && g_phantom.id != 0xFFFFFFFF && now >= g_phantom.next && !g_phantom.actor(c)) finish(c, k, now, "he is gone");
            else if (const uint32_t m = k == kMolgera && g_molgera_gave_up ? g_molgera.actor(c) : 0; m && std::hypot(ldf32(m + kPos) - x, ldf32(m + kPos + 8) - z) > 5000)
                finish(c, k, now, "outrun");
        }
    // the Phantom stalker ends as Link boards the boat or leaves the sea stage, after its time, or once he is gone
    if (g_until[kStalker] > 0) {
        if (boat || stage != "sea") finish(c, kStalker, now, "Link took to the sea");
        else if (now >= g_until[kStalker]) finish(c, kStalker, now, "time");
        else if (g_phantom.id != 0xFFFFFFFF && now >= g_phantom.next && !g_phantom.actor(c)) finish(c, kStalker, now, "he is gone");
    }
    // the warning signs: until they come to a head the event can be escaped, by landing or by sailing away from where
    // they are (2500 further off than when they began); sailing on toward it (or waiting) brings it
    if (g_warn >= 0) {
        const double d = std::hypot(x - g_warn_x, z - g_warn_z);
        const bool fled = g_warn != kDrowned && d > g_warn_far + 2500, landed = ashore || isle < 3500;
        if (fled || landed || !g_on[g_warn].load()) {
            trace("extras: the signs of %s fade (%s)", event_name(g_warn), landed ? "Link made land" : fled ? "Link sailed away" : "switched off");
            if (fled) g_cool = std::max(g_cool, now + quiet() / 3);
            g_warn = -1, wind(c, 0);
        } else if (now >= g_warn_end || (g_warn_far > 0 && d < 2000)) {  // (sailed right up to it)
            wind(c, 0);
            begin(g_warn, now);
            g_warn = -1;
        } else {
            omens(c, g_warn, now, (now - (g_warn_end - kWarning)) / kWarning);
        }
    }
    // with Haunted sea on Always there are no spells: Giant Seahats has its own, a roll every 3 minutes
    static double giants_roll = 0;
    if (always(kSea) && now >= giants_roll) g_giant_spell = giant_spell(), giants_roll = now + 180;
    // Night of the dead: a roll as night falls (or, arriving at night, 2 minutes in), for that night
    static bool was_night = false;
    const bool dusk = night && !was_night && now - g_file_at >= 120;
    if (dusk && sailor && g_until[kNight] <= now && rolls(kNight, 1 / 3.0)) begin(kNight, now), g_until[kNight] = 1e18;
    if (!night && g_until[kNight] > 0) finish(c, kNight, now, "dawn");
    // Blood moon: a roll as night falls too, twice the chance at full moon (dKy_moon_type_chk 02560828: 0 full, 3 new)
    if (dusk && sailor && g_until[kBlood] <= now && rolls(kBlood, guest_call(c, GC(0x02560828)) == 0 ? 0.5 : 0.2)) begin(kBlood, now), g_until[kBlood] = 1e18;
    if (!night && g_until[kBlood] > 0) finish(c, kBlood, now, "dawn");
    if (now - g_file_at >= 120) was_night = night;

    static double last_roll = 0;
    if (now - last_roll < 60 || !sailor || !settled || cutscene) return;
    last_roll = now;
    const double hour = ldf32(sv + 0x24) / 15;
    if (now >= g_until[kStorm] && (night || g_choice[kStorm].load() == 1) && rolls(kStorm, 0.05 * (night ? 1.5 : 0.7))) begin(kStorm, now);
    if (now >= g_until[kFog] && (night || g_choice[kFog].load() == 1) &&
        rolls(kFog, 0.06 * (night ? 1.5 : hour >= 4 && hour < 7 ? 2 : 0.5)))
        begin(kFog, now);
    if (now >= g_until[kDeep] && stage == "sea" && !ashore && rolls(kDeep, 0.07 * (night ? 1.5 : 0.8))) begin(kDeep, now);
    // the scares: one at a time, none in the quiet after the last. The horror ones only at night (Haunted sea, Phantom
    // Ganon and his stalking, the Leviathan, Molgera, Big Octo, the Lion, the drowned); the eerie ones by day too
    // (dead calm, the armada, cyclones, the maelstrom)
    if (now < g_cool || scare_on(now)) return;
    if (stage == "sea" && ashore && night && rolls(kStalker, 0.03)) { begin(kStalker, now); return; }
    if (stage == "sea" && night && boat && g_until[kOcto] == 0 && (always(kWrongLion) ? g_on[kWrongLion].load() : rolls(kWrongLion, 0.02))) {
        begin(kWrongLion, now);  // (Always: every minute of the night)
        return;
    }
    if (night && boat && rolls(kDrowned, 0.1)) { warn(kDrowned, link, now); return; }
    if (stage != "sea" || ashore || isle < 6000) return;
    const double more = std::min(3.0, 1 + (now - g_out_since) / 600);
    int k = -1;
    if (night && boat && rolls(kPhantom, 0.034 * more)) k = kPhantom;
    else if (night && rolls(kLeviathan, 0.018 * more)) k = kLeviathan;
    else if (night && rolls(kMolgera, 0.012 * more)) k = kMolgera;
    else if (night && rolls(kSea, 0.12 * more)) k = kSea;
    else if (night && boat && rolls(kOcto, 0.06 * more)) k = kOcto;
    else if (boat && rolls(kHelmaroc, 0.02 * more)) k = kHelmaroc;
    else if (boat && rolls(kCalm, 0.04 * more)) k = kCalm;
    else if (boat && rolls(kCyclone, 0.04 * more)) k = kCyclone;
    else if (boat && rolls(kMaelstrom, 0.03 * more)) k = kMaelstrom;
    else if (boat && rolls(kArmada, 0.03 * more)) k = kArmada;
    if (k >= 0) warn(k, link, now), trace("extras: (%.0f from the island, chances x%.1f)", isle, more);
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
    for (uint32_t id : g_boarders.ids) near(id);
    near(g_phantom.id);
    near(g_leviathan.id);
    near(g_molgera.id);
    near(g_helmaroc.id);
    const bool duck = enabled && (event || nearest < 2000);
    if (const float level = duck ? 0.2f : 1.f; (duck || ducked) && std::fabs(ldf32(zel + 0x90) - level) > 0.01f) {
        st32(zel + 0x90, u32_bits(level));
        if (const uint32_t bgm = ld32(zel + 0x78)) {
            double vol = ldf32(zel + 0xBC);
            for (uint32_t o = 0x90; o <= 0xAC; o += 4) vol *= ldf32(zel + o);
            c->f[1].ps0 = vol;
            guest_call(c, GC(0x0280B68C), {bgm, duck ? 150u : 120u, 0});  // down over 5 s, up over 4
        }
    }
    ducked = duck;
    static double next_beat = 0;
    if (enabled && nearest < 900 && now >= next_beat) {
        guest_call(c, GC(0x025E1988), {kHeartbeat});
        next_beat = now + (nearest < 400 ? 0.45 : 0.8);
    }
}
// Deck cannons, two on a ship: the Big ship's at its bow (on the forecastle, beside the catapult) and stern (on the
// poop deck behind the wheel), Beedle's ship's at its bow and on its hut's roof (manned from the deck before the hut,
// which lifts Link up behind it, and down again as he leaves it):
// the cannon game's cannon (daCanon_c, proc 0x3D, its own Bomber archive; it only waits for its minigame's event, and
// draws from current.pos and shape_angle each execute: y the way it points, x its barrel from straight up, 0x4000
// level), its speak prompt (attention flags +0x39C) cleared, put on the deck where the ground check finds it each step.
// The boat's controls: standing within 220 of one (its cylinder keeps him 110 off), the Bomb Bag's button (or A) mans
// it; the stick then turns and raises it (Link faces where it points, the camera behind it along its aim), ZL locks it on the nearest
// of the monsters out (again: the next by distance, off after the last), the Bomb Bag's button (or A) fires and B leaves; the game gets none of these
// (filter_pad). A shot is the boat's own cannonball (daShip_c::procCannon): a Bomb (proc 0x126) made in state 4
// (0x80020004) by fopAcM_fastCreate (025D5928), no gravity for its first 8 steps (020CB89C), then speedF +0x370
// v cos e, speed.y +0x340 v sin e (v kShotSpeed), gravity +0x374 -2.5; it bursts on what it hits. One bomb from the bag each
// (dSv_player_c +0x6A), none when it is empty (the boat's dud sound 0x0883); the firing sound 0x2852.
constexpr Gun kBigShipGuns[2] = {{{-130, 0, 950}, 0}, {{0, 0, -1320}, (int16_t)0x8000}};
// the shot's speed: the boat's is 110 (about 4800 at most); a ship's gun carries twice as far (about 10000), so it
// reaches a titanic Phantom Ganon
constexpr double kShotSpeed = 160;
// it points from 40 degrees down (from the high deck, at a monster close by in the water) to 60 up
constexpr int kDown = 0x1C71;
Single g_cannon[2];
int g_manned = -1;  // the cannon Link stands at
int g_p2_gun = -1;  // and the second gunner's, and its aim
int16_t g_p2_yaw = 0, g_p2_up = 0;
int16_t g_crew_yaw[2], g_crew_up[2];  // the ship's crew's aim at each gun, while it has one (crew(), below)
bool g_crew_aims[2];
uint32_t g_deck_shots = 0;  // deck cannon shots so far: cannon boats answer them as the boat's own (hook_023C68B8)
std::vector<uint32_t> monsters_out() {
    std::vector<uint32_t> v = g_sea.ids;
    v.insert(v.end(), g_armada.ids.begin(), g_armada.ids.end());
    for (uint32_t id : {g_leviathan.id, g_molgera.id, g_phantom.id}) if (id != 0xFFFFFFFF) v.push_back(id);
    if (const uint32_t o = g_octo_actor)  // Big Octo's eyes (their ids at +0x25C0, up to 12): what the guns lock on to
        for (uint32_t i = 0; i < 12; i++)
            if (const uint32_t id = ld32(o + 0x25C0 + 4 * i); id && id != 0xFFFFFFFF) v.push_back(id);
    return v;
}
// Phantom Ganon takes no harm from bombs in the game; 18 deck-cannon shots that burst on him (within 300 x his size,
// at most 2500, of his eyes when the ball goes) drive him off, and his event ends
constexpr int kPhantomShots = 18;
std::vector<std::pair<uint32_t, V3>> g_balls;  // the shots in flight, by process id, and where each was last
void cannon_hits(Cpu* c, double now) {
    static uint32_t counted = 0xFFFFFFFF;
    static int hits = 0;
    if (counted != g_phantom.id) counted = g_phantom.id, hits = 0;
    const uint32_t pg = g_phantom.actor(c), lev = g_leviathan.actor(c), mol = g_molgera.actor(c);
    // Bigger shots: a ball's colliders (its burst's too) bigger through the SetR hooks, as the Gyorgs' are; a burst by
    // Phantom Ganon counts as that many shots, and by the Leviathan or Molgera does its share more harm than a bomb's 4
    const float shot = kShot[upgrade(kUpShot)];
    std::erase_if(g_balls, [&](auto& b) {
        if (const uint32_t a = guest_call(c, GC(0x025DE50C), {b.first})) {
            if (shot > 1) g_big_hits.push_back({a, a + 0x1000, shot});
            return b.second = pos_of(a), false;
        }
        // (on the nearer one only, and only a burst on its body: within its reach, 500 or Molgera's head 350, times
        // the shot's size, of where it is, its height too)
        uint32_t on = 0;
        double best = 1e9;
        for (const uint32_t m : {lev, mol})
            if (const double d = m ? std::hypot(b.second.x - ldf32(m + kPos), b.second.y - ldf32(m + kPos + 4), b.second.z - ldf32(m + kPos + 8)) : 1e9;
                d < (m == lev ? 500 : 350) * shot && d < best)
                best = d, on = m;
        if (int& pool = on == lev ? g_leviathan_pool : g_molgera_pool; on && shot > 1 && pool > 0) {
            pool -= (int)std::lround(4 * (shot - 1));
            if (on == lev && pool <= 0) g_leviathan_slain = true;
            trace("extras: a bigger shot bursts on %s (%d left)", on == lev ? "the Leviathan" : "Molgera", pool);
        }
        if (pg && std::hypot(b.second.x - ldf32(pg + 0x37C), b.second.y - ldf32(pg + 0x37C + 4), b.second.z - ldf32(pg + 0x37C + 8)) <
                      std::min(2500.f, 300 * g_phantom_size * shot)) {
            hits += (int)std::lround(shot);
            trace("extras: a deck cannon shot bursts on Phantom Ganon (%d of %d)", hits, kPhantomShots);
            if (hits >= kPhantomShots && (g_until[kPhantom] > 0 || g_until[kStalker] > 0))
                finish(c, g_until[kPhantom] > 0 ? kPhantom : kStalker, now, "driven off by the deck cannons");
        }
        return true;
    });
}
// the monster out within 9000 of `from` that comes after `after` by distance (the nearest when `after` is none or gone;
// none after the last): ZL steps the lock through them all, so one nearer under the water does not hold it
uint32_t next_monster(Cpu* c, V3 from, uint32_t after) {
    std::vector<std::pair<double, uint32_t>> by;
    for (uint32_t id : monsters_out())
        if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}))
            if (const double d = std::hypot(ldf32(a + kPos) - from.x, ldf32(a + kPos + 8) - from.z); d < 9000) by.push_back({d, id});
    std::sort(by.begin(), by.end());
    for (size_t i = 0; i < by.size(); i++)
        if (by[i].second == after) return i + 1 < by.size() ? by[i + 1].second : 0xFFFFFFFF;
    return by.empty() ? 0xFFFFFFFF : by[0].second;
}
// the nearest monster out within 9000 of `from`, by process id (0xFFFFFFFF none)
uint32_t nearest_monster(Cpu* c, V3 from) {
    uint32_t best_id = 0xFFFFFFFF;
    double best = 9000;
    for (uint32_t id : monsters_out())
        if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}))
            if (const double d = std::hypot(ldf32(a + kPos) - from.x, ldf32(a + kPos + 8) - from.z); d < best) best = d, best_id = id;
    return best_id;
}
// a gun at `from` laid on monster `a`: at its eyes (eyePos +0x37C) where they will be when the shot gets there (led by
// how fast they moved since the last step, `m`), on the shot's low arc (speed v kShotSpeed, gravity g 2.5): tan e =
// (v^2 - sqrt(v^4 - g (g x^2 + 2 y v^2))) / (g x), x the distance and y the height to it; out of reach, 45 degrees
struct Lay { V3 last{}; uint32_t id = 0xFFFFFFFF; };
void lay_on(uint32_t a, uint32_t id, V3 from, Lay& m, int16_t& yaw, int16_t& up) {
    V3 eye{ldf32(a + 0x37C), ldf32(a + 0x37C + 4), ldf32(a + 0x37C + 8)};
    if (const V3 p = pos_of(a); std::hypot(eye.x - p.x, eye.z - p.z) > 3000) eye = p;  // Molgera never sets its eyes
    const V3 vel = m.id == id ? V3{eye.x - m.last.x, eye.y - m.last.y, eye.z - m.last.z} : V3{};
    m.last = eye, m.id = id;
    constexpr double v = kShotSpeed, v2 = v * v, g = 2.5;
    double e = M_PI / 8, dx = 0, dz = 0;
    for (int pass = 0; pass < 3; pass++) {  // the flight time depends on the aim, the aim on where it will be
        const double t = std::hypot(eye.x - from.x, eye.z - from.z) / (v * cos(e));
        dx = eye.x + vel.x * t - from.x, dz = eye.z + vel.z * t - from.z;
        const double x = std::max(1.0, std::hypot(dx, dz)), y = eye.y + vel.y * t - from.y;
        const double root = v2 * v2 - g * (g * x * x + 2 * y * v2);
        e = root < 0 ? M_PI / 4 : atan((v2 - std::sqrt(root)) / (g * x));
    }
    yaw = (int16_t)(atan2(dx, dz) * 32768 / M_PI);
    up = (int16_t)std::clamp(e * 32768 / M_PI, (double)-kDown, (double)0x2AAA);
}
// a shot from a gun whose pivot is `from`, from over its barrel, clear of the gunner and of the cannon's own cylinder (a
// shot bursts on anything it touches); `locked` only for the trace; `sv` 0: the crew's own powder, no bomb taken
void fire(Cpu* c, uint32_t sv, uint32_t link, V3 from, int16_t yaw, int16_t up, uint32_t locked) {
    if (sv && !ld8(sv + 0x6A)) { guest_call(c, GC(0x025E1988), {0x0883}); return; }
    if (sv) st8(sv + 0x6A, ld8(sv + 0x6A) - 1);
    const double e = up * M_PI / 32768, t = yaw * M_PI / 32768;
    static const uint32_t buf = mem::host_alloc(0x20);  // pos at +0, angle at +0x10
    st32(buf, u32_bits((float)(from.x + sin(t) * cos(e) * 230)));
    st32(buf + 4, u32_bits((float)(from.y + sin(e) * 230)));
    st32(buf + 8, u32_bits((float)(from.z + cos(t) * cos(e) * 230)));
    st16(buf + 0x10, (uint16_t)(int16_t)-up), st16(buf + 0x12, (uint16_t)yaw), st16(buf + 0x14, 0);
    const uint32_t layer = guest_call(c, GC(0x025DED64));
    guest_call(c, GC(0x025DEAB4), {ld32(link + 0x2C)});
    st32(c->r[1] - 0x40 + 8, 0);  // the 9th argument, on the stack
    const uint32_t ball = guest_call(c, GC(0x025D5928), {0x126, 0x80020004, buf, (uint32_t)(int8_t)ld8(link + kRoom), buf + 0x10, 0, 0xFFFFFFFF, 0});
    guest_call(c, GC(0x025DEAB4), {layer});
    if (!in_mem2(ball)) return;
    g_balls.push_back({ld32(ball + 4), pos_of(ball)});
    // (Bigger shots: bigger balls, at most 1.5x to look at: its burst's effects take the ball's size, and at 3x they
    // filled the screen; the harm is the full size's, cannon_hits)
    for (int i = 0; i < 3; i++) st32(ball + 0x330 + 4 * i, u32_bits(std::min(1.5f, kShot[upgrade(kUpShot)])));
    guest_call(c, GC(0x020CB89C), {ball, 8});
    st32(ball + 0x370, u32_bits((float)(kShotSpeed * cos(e)))), st32(ball + 0x340, u32_bits((float)(kShotSpeed * sin(e)))), st32(ball + 0x374, u32_bits(-2.5f));
    guest_call(c, GC(0x025E1988), {0x2852});
    g_deck_shots++;
    coop_fired(c, from, yaw, up);
    trace("extras: deck cannon fires (yaw %d, up %d, %u bombs left)%s", yaw, up, sv ? ld8(sv + 0x6A) : 0, locked == 0xFFFFFFFF ? ", aimed by hand" : ", locked on");
}
// the gun Link stands by on `ship` (by the deck spot it is manned from, for one up on a roof), or -1
int gun_by(uint32_t ship, const Gun* guns, uint32_t link) {
    const V3 me = to_local(ship, link);
    for (int i = 0; i < 2; i++)
        if (const V3 p = guns[i].lift ? guns[i].man : guns[i].at; std::hypot(me.x - p.x, me.z - p.z) < 220) return i;
    return -1;
}
// `deck_y`: the deck's height in the ship's frame, until the ground check has found the floor under each gun
void deck_cannons(Cpu* c, uint32_t sv, uint32_t link, uint32_t hull, const Gun* guns, double deck_y, bool deck, int proc, uint32_t pressed, double now) {
    static int16_t yaw = 0, up = 0x0C00;
    static uint32_t locked = 0xFFFFFFFF;
    static double fired = 0;
    cannon_hits(c, now);
    static V3 stand;  // where he stood when he manned it, in the hull's frame: a hit's knockback does not carry him off
    if (g_manned >= 0 && (!hull || !deck || !g_take_pad || ld8(kPlay + 0x5292) || (pressed & input::kB))) {
        // down off the roof, onto the deck before the hut
        if (hull && deck && guns[g_manned].lift) put_link(link, to_world(pos_of(hull), (int16_t)ld16(hull + kAngleY), {guns[g_manned].man.x, deck_y + 30, guns[g_manned].man.z}));
        g_manned = -1, g_take_pad = false, g_cannon_btn = 0, g_cannon_cam = false;
        trace("extras: Link leaves the deck cannon");
        return;
    }
    if (!hull) {  // no ship: its guns go too
        for (Single& s : g_cannon)
            if (s.actor(c)) guest_call(c, GC(0x025D57E4), {s.id}), s = Single{};
        g_cannon_btn = 0;
        return;
    }
    const int16_t hull_yaw = (int16_t)ld16(hull + kAngleY);
    static float floor_y[2];
    for (int i = 0; i < 2; i++) {
        const uint32_t a = g_cannon[i].actor(c);
        if (!a) {
            if (g_cannon[i].wanted(c, now)) {
                Spawner none;
                g_cannon[i].id = spawn(c, none, Monster{0x3D, -1, 0xFFFFFFFF}, hull, 0, false, 0, false);
                g_cannon[i].next = now + 5;
                floor_y[i] = (float)deck_y;
            }
            continue;
        }
        // the deck (or roof) under it (the hull's own collision, once it is where it is put), from above its castles
        if (static int every = 0; ++every % 15 == 0) {
            static const uint32_t buf = mem::host_alloc(0x10);
            const V3 w = to_world(pos_of(hull), hull_yaw, {guns[i].at.x, kDeckY + 800, guns[i].at.z});
            st32(buf, u32_bits((float)w.x)), st32(buf + 4, u32_bits((float)w.y)), st32(buf + 8, u32_bits((float)w.z));
            guest_call(c, GC(0x02549F4C), {buf});
            if ((float)c->f[1].ps0 != (float)w.y) floor_y[i] = (float)(c->f[1].ps0 - ldf32(hull + kPos + 4));
        }
        const V3 w = to_world(pos_of(hull), hull_yaw, {guns[i].at.x, floor_y[i], guns[i].at.z});
        st32(a + kPos, u32_bits((float)w.x)), st32(a + kPos + 4, u32_bits((float)w.y)), st32(a + kPos + 8, u32_bits((float)w.z));
        const bool mine = g_manned == i, p2 = !mine && g_p2_gun == i, crew = !mine && !p2 && g_crew_aims[i];
        st16(a + kAngleY, (uint16_t)(mine ? yaw : p2 ? g_p2_yaw : crew ? g_crew_yaw[i] : (int16_t)(hull_yaw + guns[i].face)));
        st16(a + 0x328, (uint16_t)(mine ? 0x4000 - up : p2 ? 0x4000 - g_p2_up : crew ? 0x4000 - g_crew_up[i] : 0x3400));
        st32(a + 0x39C, 0);
    }
    if (!deck) { g_cannon_btn = 0; return; }
    const int by = gun_by(hull, guns, link);
    const uint32_t button = by >= 0 && (proc == 0x4 || proc == 0x6) ? bomb_button() : 0;
    g_cannon_btn = g_manned >= 0 ? bomb_button() : button;
    if (g_manned < 0) {
        if (by >= 0 && (proc == 0x4 || proc == 0x6) && (pressed & (button | input::kA))) {
            g_manned = by, g_take_pad = true, locked = 0xFFFFFFFF, up = 0x0C00, stand = to_local(hull, link);
            yaw = (int16_t)(hull_yaw + guns[by].face);  // out over the bow, or the stern
            if (guns[by].lift) {  // up onto the roof, behind the gun
                const Gun& g = guns[by];
                stand = {g.at.x, floor_y[by] + 5.0, g.at.z + (g.face ? 170 : -170)};
                put_link(link, to_world(pos_of(hull), hull_yaw, stand));
                st32(link + 0x340, 0);
            }
            fired = now;  // the press that manned it does not also fire
            trace("extras: Link mans the %s cannon", by == 0 ? "bow" : guns[by].lift ? "roof" : "stern");
        }
        return;
    }
    // aiming: the stick (3/4 of a turn a second at full tilt, 40 degrees down to 60 up), or the locked monster
    const V3 from{ldf32(link + kPos), ldf32(link + kPos + 4) + 100, ldf32(link + kPos + 8)};
    if (pressed & input::kZL) locked = next_monster(c, from, locked);
    static Lay lay;
    if (const uint32_t a = locked == 0xFFFFFFFF ? 0 : guest_call(c, GC(0x025DE50C), {locked})) {
        lay_on(a, locked, from, lay, yaw, up);
    } else {
        locked = 0xFFFFFFFF;
        yaw = (int16_t)(yaw - g_pad_lx.load() * 0x2A0);
        up = (int16_t)std::clamp(up + (int)(g_pad_ly.load() * 0x100), -kDown, 0x2AAA);
    }
    st16(link + 0x322, (uint16_t)yaw), st16(link + kAngleY, (uint16_t)yaw);
    // held at the gun: put back on his spot (position and old position) if a hit knocked him off it
    if (const V3 l = to_local(hull, link); std::hypot(l.x - stand.x, l.z - stand.z) > 20) {
        const V3 w = to_world(pos_of(hull), hull_yaw, stand);
        put_link(link, w);
        st32(link + 0x340, 0);
    }
    g_cannon_x = (float)from.x, g_cannon_y = (float)from.y, g_cannon_z = (float)from.z;
    g_cannon_yaw = yaw, g_cannon_up = up, g_cannon_cam = true;
    if (!(pressed & (g_cannon_btn.load() | input::kA)) || now < fired + kReload[upgrade(kUpReload)]) return;
    fired = now;
    fire(c, sv, link, from, yaw, up, locked);
}
// The ship's crew: each gun neither Link nor the second gunner is at is laid on the nearest monster within 6000 and fires
// every 4 s (its own powder), if that is out over its own side; it points as laid (g_crew_yaw / _up, which the gun loop shows)
void crew(Cpu* c, uint32_t link, uint32_t ship, const Gun* guns, double now) {
    static double fired[2];
    static Lay lay[2];
    for (int i = 0; i < 2; i++) {
        g_crew_aims[i] = false;
        const uint32_t a = i == g_manned || i == g_p2_gun ? 0 : g_cannon[i].actor(c);
        if (!a) continue;
        const V3 from{ldf32(a + kPos), ldf32(a + kPos + 4) + 100, ldf32(a + kPos + 8)};
        const uint32_t id = nearest_monster(c, from);
        const uint32_t m = id == 0xFFFFFFFF ? 0 : guest_call(c, GC(0x025DE50C), {id});
        if (!m || std::hypot(ldf32(m + kPos) - from.x, ldf32(m + kPos + 8) - from.z) > 6000) continue;
        lay_on(m, id, from, lay[i], g_crew_yaw[i], g_crew_up[i]);
        // only out over its own side of the ship (within 90 degrees of the way it rests): never across its deck
        if (std::abs((int16_t)(g_crew_yaw[i] - (int16_t)(ld16(ship + kAngleY) + guns[i].face))) > 0x4000) continue;
        g_crew_aims[i] = true;
        if (now >= fired[i] + kCrewReload[upgrade(kUpReload)]) fired[i] = now, fire(c, 0, link, from, g_crew_yaw[i], g_crew_up[i], id);
    }
}
// The second gunner: with a second controller (kept apart from the first, input::set_second_player), player 2 has the
// gun Link is not at: its left stick turns and raises it, ZL locks it on the nearest monster (again, the next; off after the last), A or ZR fires,
// from the same Bomb Bag. Player 1 sails or plays on as ever; the camera stays his
void second_gunner(Cpu* c, uint32_t sv, uint32_t link, uint32_t hull, const Gun* guns, double now) {
    static int16_t yaw = 0, up = 0x0C00;
    static uint32_t locked = 0xFFFFFFFF, prev = 0;
    static double fired = 0;
    static int gun = -1;
    static Lay lay;
    input::PadState p2;
    if (!hull || !input::read_second(p2)) { g_p2_gun = -1, gun = -1; return; }
    const int want = g_manned == 1 ? 0 : 1;
    const int16_t hull_yaw = (int16_t)ld16(hull + kAngleY);
    if (gun != want) gun = want, yaw = (int16_t)(hull_yaw + guns[gun].face), up = 0x0C00, locked = 0xFFFFFFFF;
    const uint32_t a = g_cannon[gun].actor(c);
    if (!a) { g_p2_gun = -1; return; }
    const uint32_t pressed = p2.buttons & ~prev;
    prev = p2.buttons;
    const V3 from{ldf32(a + kPos), ldf32(a + kPos + 4) + 100, ldf32(a + kPos + 8)};
    if (pressed & input::kZL) locked = next_monster(c, from, locked);
    if (const uint32_t m = locked == 0xFFFFFFFF ? 0 : guest_call(c, GC(0x025DE50C), {locked})) {
        lay_on(m, locked, from, lay, yaw, up);
    } else {
        locked = 0xFFFFFFFF;
        yaw = (int16_t)(yaw - p2.lx * 0x2A0);
        up = (int16_t)std::clamp(up + (int)(p2.ly * 0x100), -kDown, 0x2AAA);
    }
    g_p2_gun = gun, g_p2_yaw = yaw, g_p2_up = up;
    if (!(pressed & (input::kA | input::kZR)) || now < fired + kReload[upgrade(kUpReload)]) return;
    fired = now;
    fire(c, sv, link, from, yaw, up, locked);
    trace("extras: the second gunner fires the %s gun", gun == 0 ? "bow" : "other");
}

// Online co-op: the world the players share (docs/multiplayer.md; the session, the players and the join are coop_sync.cpp).
// Tetra's ship: the host's place, heading and sail go out every step and a guest's ship is put there (the Big ship
// block). Cannon shots: a shot fired on one game (its pivot and aim, in the ship's frame on board) is fired on the others
// from the crew's powder. Sea events: only the host rolls; each sign, start and end goes to the guests, whose events run
// from those alone (direct). Monsters: each one the host's mods make goes out with a number (and its parent's) and is made
// on the guests, where their own mods' spawns take it (coop_adopt); every step the host's place and heading of each go
// out and a guest's copies are put there, 100 ms behind; one gone on the host goes on the guests; one killed on a guest
// (gone there first) goes on the host too: the shooter is favoured.
struct Shared { uint16_t n; uint32_t id; int16_t proc; bool adopted; Monster m; uint16_t up; int8_t hp = -128; uint8_t anm_seq = 0; };
std::vector<Shared> g_coop;  // the host's monsters out, or a guest's copies of them
bool g_coop_remote_shot = false;
struct Bytes {  // a message's fields, in order
    std::string b;
    template <class T> Bytes& put(T v) { b.append((const char*)&v, sizeof v); return *this; }
};
struct Reader {
    const std::string& b;
    size_t o = 0;
    template <class T> T get() { T v{}; if (o + sizeof v <= b.size()) memcpy(&v, b.data() + o, sizeof v); o += sizeof v; return v; }
};
bool coop_shared(const Monster& m) {  // not the ships and their parts, which each game makes for itself
    return m.proc != 0x3D && m.proc != kPirates.proc && m.proc != 0x1BF && m.proc != kRaftCraft.m.proc && m.proc != kBeedleCraft.m.proc;
}
void coop_spawn_msg(int to, const Shared& s, float x, float y, float z, int16_t yaw) {
    const Monster& m = s.m;
    mods::coop::net::send(to, mods::coop::net::kSpawn,
                          Bytes{}.put(s.n).put(s.up).put(m.proc).put(m.subtype).put(m.param).put(m.angle_x).put(x).put(y).put(z).put(yaw).b);
}
uint32_t coop_spawned(Cpu* c, Monster m, uint32_t id, uint32_t parent, float x, float y, float z, int16_t yaw) {
    if (id == 0xFFFFFFFF || !mods::coop::sync::host() || !coop_shared(m)) return id;
    static uint16_t next = 0;
    uint16_t up = 0;
    for (const Shared& s : g_coop) if (parent && s.id == parent) up = s.n;
    g_coop.push_back({++next, id, m.proc, false, m, up});
    coop_spawn_msg(-1, g_coop.back(), x, y, z, yaw);
    (void)c;
    return id;
}
uint32_t coop_adopt(Monster m, Spawner& s) {
    for (Shared& x : g_coop)
        if (!x.adopted && x.proc == m.proc) {
            x.adopted = true, s.ids.push_back(x.id);
            return x.id;
        }
    return 0xFFFFFFFF;  // (not made on the host yet: asked again later, as when there is no ground)
}
void coop_fired(Cpu* c, V3 from, int16_t yaw, int16_t up) {
    if (g_coop_remote_shot || mods::coop::net::role() == mods::coop::net::Role::kOff) return;
    const uint32_t hull = g_bigship.actor(c);
    const bool aboard = hull && std::hypot(from.x - ldf32(hull + kPos), from.z - ldf32(hull + kPos + 8)) < 3000;
    const V3 at = aboard ? to_local(hull, from) : from;
    const int16_t a = aboard ? (int16_t)(yaw - (int16_t)ld16(hull + kAngleY)) : yaw;
    mods::coop::net::send(-1, mods::coop::net::kFired, Bytes{}.put<uint8_t>(aboard).put((float)at.x).put((float)at.y).put((float)at.z).put(a).put(up).b);
}
// The host's monsters, type by type (docs/multiplayer.md): each step, with its place, its health (+0x3A1), its mode
// fields, its animation's frame and rate (mDoExt_McaMorf: +0x98 rate, +0x9C frame) and the last animation its anm_init
// set; a guest's copy is put in that state and makes no choices of its own (its mode procedure is not run, or its
// fields are overwritten each step); a hit on it there is sent to the host, whose health counts. On the host each
// monster goes for the nearest player: around its execute, Link's place is that player's (every one of these reads
// Link's place through play +0x5B2C / +0x5B34), then his own again.
struct CoopKind {
    int16_t proc;
    uint32_t mode, mode2;   // its mode fields (0 none); the first s32 or (mode_s16) s16, the second s16
    bool mode_s16;
    uint32_t morf;          // its mDoExt_McaMorf pointer (0 none)
    uint32_t anm_init;      // its anm_init(this, idx, f1 morf, loop, f2 speed, flag) (0 none)
};
constexpr CoopKind kCoopKinds[] = {
    {0xE4, 0x3CC, 0, false, 0x3F0, 0},                 // Gyorg (daGy): mode; its one animation
    {0xE1, 0x688, 0, false, 0x6A8, 0},                 // Big Octo (daDaiocta): mode
    {0xB3, 0x3C8, 0x3CC, false, 0, 0},                 // a cannon boat (daOship): mode, submode
    {0xF1, 0x5BA, 0x5BC, true, 0x3DC, 0x02134108},     // Phantom Ganon (daFganon): action, mode
    {0xD9, 0x1B32, 0x1B34, true, 0x3DC, 0x020F9AF4},   // Molgera (daBwd): state, substate
    {0xD1, 0x45B, 0x462, false, 0x3D8, 0x023C9AD8},    // a Seahat (ph_class): u8 mode, s16 submode; its body's morf
};
const CoopKind* coop_kind(int16_t proc) {
    for (const CoopKind& k : kCoopKinds) if (k.proc == proc) return &k;
    return nullptr;
}
Shared* coop_find(uint32_t actor) {
    if (!actor) return nullptr;
    for (Shared& s : g_coop) if (s.id == ld32(actor + 4)) return &s;
    return nullptr;
}
// a guest's copy of the host's monster: no choices of its own
bool coop_puppet(uint32_t actor) { return mods::coop::sync::guest() && coop_find(actor); }
// the last animation each of the host's monsters set (its anm_init), by actor id
struct CoopAnm { uint8_t idx, loop, flag, seq; float morf, speed; };
std::unordered_map<uint32_t, CoopAnm> g_coop_anm;
// the host: Link stands where the player a monster goes for is, for its execute (nothing when that is Link himself)
std::unordered_map<uint32_t, int> g_coop_target;  // by actor id: the player it goes for
struct CoopStandIn {
    uint32_t link = 0;
    float was[3];
    explicit CoopStandIn(uint32_t actor) {
        if (!mods::coop::sync::host() || !coop_find(actor)) return;
        const uint32_t l = ld32(kPlay + 0x5B34);
        if (!in_mem2(l)) return;
        const float at[3] = {(float)ldf32(actor + kPos), (float)ldf32(actor + kPos + 4), (float)ldf32(actor + kPos + 8)};
        int& target = g_coop_target.try_emplace(ld32(actor + 4), -1).first->second;
        float to[3];
        mods::coop::sync::nearest(at, l, target, to);
        if (target == mods::coop::net::my_id()) return;
        link = l;
        for (int i = 0; i < 3; i++) was[i] = (float)ldf32(l + kPos + 4 * i), st32(l + kPos + 4 * i, u32_bits(to[i]));
    }
    ~CoopStandIn() { if (link) for (int i = 0; i < 3; i++) st32(link + kPos + 4 * i, u32_bits(was[i])); }
};
// a guest's copy into the host's mode: by the type's own mode change where it has one (its init procedure runs)
void coop_set_mode(Cpu* c, uint32_t a, const CoopKind& k, uint32_t mode, uint16_t mode2) {
    static constexpr uint32_t kGyorgInit[] = {0x02167258, 0x021695C0, 0x02169B68, 0x02169B48, 0x02169EE8,
                                              0x0216A674, 0x02169AA8, 0x0216810C, 0x02167FE0, 0x02167F30};
    const uint32_t now = k.mode_s16 ? ld16(a + k.mode) : k.proc == 0xD1 ? ld8(a + k.mode) : ld32(a + k.mode);
    if (now != mode) {
        if (k.proc == 0xE4 && mode < 10) guest_call(c, GC(kGyorgInit[mode]), {a});
        else if (k.proc == 0xE1) guest_call(c, GC(0x0211889C), {a, 0, mode});  // daDaiocta modeProc(init)
        else if (k.proc == 0xB3) guest_call(c, GC(0x023C58DC), {a, 0, mode});  // daOship modeProc(init)
        else if (k.mode_s16) st16(a + k.mode, (uint16_t)mode);
        else if (k.proc == 0xD1) st8(a + k.mode, (uint8_t)mode);
        else st32(a + k.mode, mode);
    }
    if (k.mode2) k.proc == 0xB3 ? st32(a + k.mode2, mode2) : st16(a + k.mode2, mode2);
}

// the boat's own cannon (daShip's execute creates its ball at 02479680, as fire() does): the ball made, sent once its
// speeds are set (the next mods' step), and fired the same on the others
struct BoatShot { uint32_t ball = 0; float pos[3]; int16_t angle[3]; } g_boat_shot;
void coop_boat_shot(Cpu* c, uint32_t link) {
    namespace net = mods::coop::net;
    const uint32_t ball = std::exchange(g_boat_shot.ball, 0);
    if (!ball || net::role() == net::Role::kOff || !in_mem2(ball)) return;
    Bytes b;
    for (float v : g_boat_shot.pos) b.put(v);
    for (int16_t v : g_boat_shot.angle) b.put(v);
    b.put((float)ldf32(ball + 0x370)).put((float)ldf32(ball + 0x340)).put((float)ldf32(ball + 0x374));
    net::send(-1, net::kBoatShot, b.b);
    (void)c, (void)link;
}
void boat_shot_from(Cpu* c, uint32_t link, Reader& r) {
    static const uint32_t buf = mem::host_alloc(0x20);  // pos at +0, angle at +0x10
    for (int i = 0; i < 3; i++) st32(buf + 4 * i, u32_bits(r.get<float>()));
    for (int i = 0; i < 3; i++) st16(buf + 0x10 + 2 * i, (uint16_t)r.get<int16_t>());
    const float f = r.get<float>(), y = r.get<float>(), g = r.get<float>();
    const uint32_t layer = guest_call(c, GC(0x025DED64));
    guest_call(c, GC(0x025DEAB4), {ld32(link + 0x2C)});
    st32(c->r[1] - 0x40 + 8, 0);
    const uint32_t ball = guest_call(c, GC(0x025D5928), {0x126, 0x80020004, buf, (uint32_t)(int8_t)ld8(link + kRoom), buf + 0x10, 0, 0xFFFFFFFF, 0});
    guest_call(c, GC(0x025DEAB4), {layer});
    if (!in_mem2(ball)) return;
    guest_call(c, GC(0x020CB89C), {ball, 8});
    st32(ball + 0x370, u32_bits(f)), st32(ball + 0x340, u32_bits(y)), st32(ball + 0x374, u32_bits(g));
    guest_call(c, GC(0x025E1988), {0x2852});
}
void coop_event(int op, int k, double now, int to) {
    if (!mods::coop::sync::host()) return;
    const double left = op == 0 ? g_warn_end - now : k == kNight || k == kBlood ? 1e9 : g_until[k] - now;
    mods::coop::net::send(to, mods::coop::net::kSeaEvent,
                          Bytes{}.put<uint8_t>(op).put<uint8_t>(k).put(left).put(g_warn_x).put(g_warn_z).put(g_warn_far).put<uint8_t>(g_giant_spell).b);
}
// the host's snapshots: a then b, t of the way (over 1: on past b)
template <class T> T mix(T a, T b, float t) { return (T)(a + (b - a) * t); }
constexpr size_t kRecord = 2 + 12 + 2 + 2 + 1 + 4 + 2 + 4 + 4 + 4 + 4 + 4;  // a monster's record in kMonsters
bool coop_host_ship(V3& pos, int16_t& yaw, bool& sail) {
    std::string a, b;
    float t;
    if (!mods::coop::sync::host_state(mods::coop::sync::kShip, a, b, t) || a.size() < 15 || b.size() < 15) return false;
    Reader ra{a}, rb{b};
    float pa[3], pb[3];
    for (int i = 0; i < 3; i++) pa[i] = ra.get<float>(), pb[i] = rb.get<float>();
    const int16_t ya = ra.get<int16_t>(), yb = rb.get<int16_t>();
    pos = {mix(pa[0], pb[0], t), mix(pa[1], pb[1], t), mix(pa[2], pb[2], t)};
    yaw = (int16_t)(ya + (int16_t)(yb - ya) * t), sail = rb.get<uint8_t>();
    return true;
}
// each step in play, after the mods
void coop_world(Cpu* c, uint32_t link, bool settled, bool riding, double now) {
    namespace net = mods::coop::net;
    namespace sync = mods::coop::sync;
    if (net::role() == net::Role::kOff) { g_coop.clear(); return; }
    static std::vector<sync::WorldMsg> held;  // (until this game is settled in its stage)
    for (auto& m : sync::take_world()) held.push_back(std::move(m));
    if (!sync::arrived()) held.clear();
    coop_boat_shot(c, link);
    for (size_t i = 0; settled && i < held.size(); i++) {
        const sync::WorldMsg& m = held[i];
        Reader r{m.body};
        if (m.type == net::kFired) {
            const bool aboard = r.get<uint8_t>();
            const V3 at{r.get<float>(), r.get<float>(), r.get<float>()};
            int16_t yaw = r.get<int16_t>();
            const int16_t up = r.get<int16_t>();
            const uint32_t hull = g_bigship.actor(c);
            if (aboard && !hull) continue;
            if (aboard) yaw = (int16_t)(yaw + (int16_t)ld16(hull + kAngleY));
            g_coop_remote_shot = true;
            fire(c, 0, link, aboard ? to_world(pos_of(hull), (int16_t)ld16(hull + kAngleY), at) : at, yaw, up, 0xFFFFFFFF);
            g_coop_remote_shot = false;
        } else if (m.type == net::kDamage && sync::host()) {  // a guest's hit on one of ours
            const uint16_t n = r.get<uint16_t>();
            const int dmg = r.get<uint8_t>();
            for (const Shared& sh : g_coop)
                if (const uint32_t a = sh.n == n ? guest_call(c, GC(0x025DE50C), {sh.id}) : 0) {
                    const int hp = (int8_t)ld8(a + 0x3A1) - dmg;
                    if (hp <= 0) delete_actor(c, sh.id);  // (gone: to the guests as killed)
                    else st8(a + 0x3A1, (uint8_t)hp);
                }
        } else if (m.type == net::kBoatShot) {
            boat_shot_from(c, link, r);
        } else if (m.type == net::kSeaEvent) {
            const int op = r.get<uint8_t>(), k = r.get<uint8_t>();
            const double left = r.get<double>(), wx = r.get<double>(), wz = r.get<double>(), far = r.get<double>();
            const bool giants = r.get<uint8_t>();
            if (k >= kCount) continue;
            if (op == 0) g_warn = k, g_warn_end = now + left, g_warn_x = wx, g_warn_z = wz, g_warn_far = far;
            else if (op == 1) {
                if (g_warn == k) g_warn = -1, wind(c, 0);
                begin(k, now);
                g_until[k] = now + left, g_giant_spell = giants;
            } else if (g_until[k] > 0) finish(c, k, now, "the host's ended");
        } else if (m.type == net::kSpawn) {
            const uint16_t n = r.get<uint16_t>(), up = r.get<uint16_t>();
            Monster mo{r.get<int16_t>(), r.get<int8_t>(), r.get<uint32_t>(), r.get<int16_t>()};
            const float x = r.get<float>(), y = r.get<float>(), z = r.get<float>();
            const int16_t yaw = r.get<int16_t>();
            uint32_t parent = 0;
            for (const Shared& s : g_coop) if (up && s.n == up) parent = s.id;
            if (up && !parent) continue;  // (its parent is gone here)
            const uint32_t id = create_at(c, mo, link, x, y, z, yaw, parent);
            if (id != 0xFFFFFFFF) g_coop.push_back({n, id, mo.proc, false, mo, up});
            trace("extras: co-op: the host's monster %u (proc %#x) made here, id %#x", n, mo.proc, id);
        } else if (m.type == net::kArrived && sync::host()) {  // a guest joined: what is going on now, to it
            for (int k = 0; k < kCount; k++) if (g_until[k] > now) coop_event(1, k, now, m.from);
            if (g_warn >= 0) coop_event(0, g_warn, now, m.from);
            for (const Shared& s : g_coop)
                if (const uint32_t a = guest_call(c, GC(0x025DE50C), {s.id}))
                    coop_spawn_msg(m.from, s, ldf32(a + kPos), ldf32(a + kPos + 4), ldf32(a + kPos + 8), (int16_t)ld16(a + kAngleY));
            trace("extras: co-op: player %d arrived: %zu monsters sent", m.from, g_coop.size());
        } else if (m.type == net::kKill || m.type == net::kHit) {  // gone on the host; or (the host) killed on a guest
            const uint16_t n = r.get<uint16_t>();
            for (size_t j = 0; j < g_coop.size(); j++)
                if (g_coop[j].n == n) {
                    if (guest_call(c, GC(0x025DE50C), {g_coop[j].id})) delete_actor(c, g_coop[j].id);
                    if (m.type == net::kKill) g_coop.erase(g_coop.begin() + j);
                    break;
                }
        }
    }
    if (settled) held.clear();
    // the monsters: the host's out; a guest's put where the host's are (a guest's copies all go with its stage)
    static uint32_t last_link = 0;
    if (link != last_link && sync::guest()) g_coop.clear();
    last_link = link;
    std::string a, b;
    float t = 0;
    const bool have = sync::guest() && sync::host_state(sync::kMonsters, a, b, t);
    Bytes out;
    for (size_t j = 0; j < g_coop.size();) {
        const Shared& s = g_coop[j];
        const uint32_t m = guest_call(c, GC(0x025DE50C), {s.id});
        if (!m) {  // gone: on the host, from the guests too; on a guest, killed here, from the host too
            g_coop_anm.erase(s.id);
            if (sync::host() || settled) net::send(-1, sync::host() ? net::kKill : net::kHit, Bytes{}.put(s.n).b);
            g_coop.erase(g_coop.begin() + j);
            continue;
        }
        j++;
        const CoopKind* kind = coop_kind(s.proc);
        const uint32_t morf = kind && kind->morf ? ld32(m + kind->morf) : 0;
        if (sync::host()) {  // its record: n, place, two angles, health, mode, mode2, frame, rate, last animation
            if (out.b.size() >= kRecord * 30) continue;
            const CoopAnm an = g_coop_anm.count(s.id) ? g_coop_anm[s.id] : CoopAnm{0xFF, 0, 0, 0, 0, 0};
            out.put(s.n).put((float)ldf32(m + kPos)).put((float)ldf32(m + kPos + 4)).put((float)ldf32(m + kPos + 8))
                .put((int16_t)ld16(m + 0x322)).put((int16_t)ld16(m + kAngleY)).put((int8_t)ld8(m + 0x3A1))
                .put(!kind || !kind->mode ? 0u : kind->mode_s16 ? (uint32_t)ld16(m + kind->mode) : kind->proc == 0xD1 ? (uint32_t)ld8(m + kind->mode) : ld32(m + kind->mode))
                .put(kind && kind->mode2 ? (uint16_t)(kind->proc == 0xB3 ? ld32(m + kind->mode2) : ld16(m + kind->mode2)) : (uint16_t)0)
                .put(morf ? (float)ldf32(morf + 0x9C) : 0.f).put(morf ? (float)ldf32(morf + 0x98) : 0.f)
                .put(an.idx).put(an.loop).put(an.flag).put(an.seq).put(an.morf).put(an.speed);
            continue;
        }
        if (!have) continue;
        struct Rec { float p[3]; int16_t yaw, shape; int8_t hp; uint32_t mode; uint16_t mode2; float frame, rate; CoopAnm an; };
        auto find = [n = s.n](const std::string& snap, Rec& o) {
            for (Reader r{snap}; r.o + kRecord <= snap.size();) {
                const uint16_t k = r.get<uint16_t>();
                for (float& v : o.p) v = r.get<float>();
                o.yaw = r.get<int16_t>(), o.shape = r.get<int16_t>(), o.hp = r.get<int8_t>(), o.mode = r.get<uint32_t>(), o.mode2 = r.get<uint16_t>();
                o.frame = r.get<float>(), o.rate = r.get<float>();
                o.an.idx = r.get<uint8_t>(), o.an.loop = r.get<uint8_t>(), o.an.flag = r.get<uint8_t>(), o.an.seq = r.get<uint8_t>();
                o.an.morf = r.get<float>(), o.an.speed = r.get<float>();
                if (k == n) return true;
            }
            return false;
        };
        Rec ra, rb;
        if (!find(b, rb)) continue;
        if (!find(a, ra)) ra = rb;
        for (int i = 0; i < 3; i++) {
            const float v = mix(ra.p[i], rb.p[i], t);
            st32(m + kPos + 4 * i, u32_bits(v)), st32(m + 0x300 + 4 * i, u32_bits(v));
        }
        st16(m + 0x322, (uint16_t)(int16_t)(ra.yaw + (int16_t)(rb.yaw - ra.yaw) * t)), st16(m + kAngleY, (uint16_t)(int16_t)(ra.shape + (int16_t)(rb.shape - ra.shape) * t));
        // a hit here since the host's health was last put in: to the host, whose health counts
        if (Shared& sh = g_coop[j - 1]; sh.hp != -128 && (int8_t)ld8(m + 0x3A1) < sh.hp)
            net::send(0, net::kDamage, Bytes{}.put(sh.n).put<uint8_t>((uint8_t)(sh.hp - (int8_t)ld8(m + 0x3A1))).b);
        st8(m + 0x3A1, (uint8_t)rb.hp), g_coop[j - 1].hp = rb.hp;
        if (!kind) continue;
        if (kind->mode) coop_set_mode(c, m, *kind, rb.mode, rb.mode2);
        if (Shared& sh = g_coop[j - 1]; kind->anm_init && rb.an.idx != 0xFF && rb.an.seq != sh.anm_seq) {
            sh.anm_seq = rb.an.seq;
            c->f[1].ps0 = rb.an.morf, c->f[2].ps0 = rb.an.speed;
            guest_call(c, GC(kind->anm_init), {m, rb.an.idx, rb.an.loop, rb.an.flag});
        }
        if (morf) {
            st32(morf + 0x98, u32_bits(rb.rate));
            if (std::fabs((float)ldf32(morf + 0x9C) - rb.frame) > 2) st32(morf + 0x9C, u32_bits(rb.frame));
        }
    }
    if (sync::host()) net::put_state(sync::kMonsters, out.b);
    // the players push each other: this game moves only its own Link out of the others' room (radius 35 each)
    if (riding) return;
    const V3 me = pos_of(link);
    for (int id = 0; id < net::kMaxPlayers; id++)
        if (float p[3]; sync::place(id, p)) {
            const double dx = me.x - p[0], dz = me.z - p[2], d = std::hypot(dx, dz);
            if (d > 0.01 && d < 70 && std::fabs(me.y - p[1]) < 80) put_link(link, {p[0] + dx / d * 70, me.y, p[2] + dz / d * 70});
        }
}
}  // namespace

// ---- the game thread, once per full logic step ----

void step(Cpu* c) {
    mods::coop::sync::poll(c, on<kCoop>());  // (at the title too: the session's messages)
    bool keys[256];
    input::held_keys(keys);
    static bool turbo = false;
    if (const bool t = on<kTurbo>() && (keys[kVK_ANSI_T] || (input::live_buttons() & 0x10000000u)); t != turbo) gx2::set_uncapped(turbo = t);  // (live pad bit 28: a test driver's T)

    static uint32_t last_link = 0;
    static uint16_t life = 0, rupees = 0;
    const uint32_t link = ld32(kLinkPtr);
    const bool fresh = link != last_link;  // a file loaded, a stage entered: start over
    last_link = link;
    std::string stage((const char*)mem::ptr(kStage), strnlen((const char*)mem::ptr(kStage), 8));
    g_beedle_actor = g_beedle_lion = 0, g_beedle_aboard = false;  // (set again below while Beedle's ship is out)
    if (!in_mem2(link) || stage.empty() || stage == "sea_T" || stage == "Name" || stage == "ENDumi") {  // title, file select
        g_manned = -1, g_take_pad = false, g_cannon_btn = 0, g_cannon_cam = false, g_file_at = -1;
        return;
    }
    const uint32_t sv = save();
    static double settle = 0;  // no spawning while a stage is still being entered (the spawns would land in the old one)
    if (fresh) {
        g_night = Spawner{}, g_sea = Spawner{}, g_armada = Spawner{}, g_boarders = Spawner{}, g_gyctrl = 0xFFFFFFFF, g_ghost = g_bigship = g_phantom = g_leviathan = g_molgera = g_beedle = g_tow_rope = g_cyclone = g_whirl = g_helmaroc = Single{};
        g_cannon[0] = g_cannon[1] = Single{}, g_manned = -1, g_take_pad = false, g_cannon_btn = 0, g_cannon_cam = false;
        g_balls.clear();
        g_gyorg_ids.clear(), g_giant_ids.clear(), g_big_hits.clear();
        settle = game_time() + 2;
        if (g_warn >= 0) wind(c, 0), g_warn = -1;  // a sea event does not outlive its stage
        g_until[kSea] = g_until[kPhantom] = g_until[kLeviathan] = g_until[kMolgera] = g_until[kStalker] = g_until[kCalm] = g_until[kCyclone] = g_until[kMaelstrom] = g_until[kArmada] = g_until[kOcto] = g_until[kHelmaroc] = 0;
        g_armada_for = g_octo_for = -1, g_whirl_fading = false, g_octo_gyorgs = 0, g_octo = Single{}, g_octo_dead = false;
        remove_molgera(c);
    }
    // nor while the next stage is asked for (dComIfGp's next stage enabled, play +0x514C): through the Big ship's
    // cabin door its actors are gone before Link is, and a spawn then halted the game
    const bool settled = !fresh && game_time() >= settle && !ld8(kPlay + 0x514C);
    g_big_hits.clear();  // refilled below by the mods whose monsters hit bigger
    g_phantom_actor = 0;

    const uint16_t max_life = ld16(sv), cur = ld16(sv + 2);
    if (!fresh && cur < life) trace("extras: Link is hit: life %u -> %u (quarter hearts)", life, cur);
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
    g_step_time = now;
    // on the water: riding the boat (daPy_lk_c::mCurProc +0x65F0, SHIP_READY..SHIP_RESTART but not SHIP_GET_OFF),
    // swimming (mModeFlg +0x6A70, ModeFlg_SWIM) or on the Big ship's deck, which is the sea for every mod; the sea
    // stage also holds the islands
    const int proc = (int)ld32(link + 0x65F0);
    const bool riding = proc >= 0x86 && proc <= 0x91 && proc != 0x90, swimming = ld32(link + 0x6A70) & 0x40000;
    // the craft in tow (the raft before Beedle's ship); changed while one is out, that one goes (and its rope)
    // (test aid: WWHD_TEST_SHIP_OWNED=1 gives the file Tetra's ship as it loads)
    if (fresh && getenv("WWHD_TEST_SHIP_OWNED")) st8(sv + 0x772, ld8(sv + 0x772) | 1);
    const bool own_ship = on<kBigShip>() && owns_ship();  // she sails in place of a craft in tow
    const Craft* tow = own_ship ? nullptr : on<kRaft>() ? &kRaftCraft : on<kBeedle>() ? &kBeedleCraft : nullptr;
    if (tow && tow != g_craft && g_beedle.actor(c)) {
        guest_call(c, GC(0x025D57E4), {g_beedle.id}), g_beedle = Single{};
        if (g_tow_rope.actor(c)) guest_call(c, GC(0x025D57E4), {g_tow_rope.id}), g_tow_rope = Single{};
    }
    if (tow) g_craft = tow;
    const uint32_t beedle = tow && stage == "sea" ? g_beedle.actor(c) : 0;
    const uint32_t hull = !beedle && own_ship && stage == "sea" ? g_bigship.actor(c) : 0;
    const bool deck = !riding && !swimming && ((hull && on_deck(hull, link)) || (beedle && on_beedle(beedle, link)));
    // on a ship's deck Link moves with it at 30 Hz while it moves (true 60), and for a second after: its bobbing carries
    // him a little, now and then, and each carry dropped him to 30 Hz for a few passes and back to 60, a shake (at 60 Hz
    // he is drawn half a step ahead of the 30 Hz world). On a deck at rest (at anchor) he is at 60 Hz, as on land.
    static uint32_t ship_was[6];
    static double ship_moved = -1;
    if (const uint32_t ship = beedle ? beedle : hull) {
        const uint32_t now_at[6] = {ld32(ship + kPos), ld32(ship + kPos + 4), ld32(ship + kPos + 8), ld16(ship + kAngleY),
                                    ld16(ship + 0x328), ld16(ship + 0x32C)};  // (and its rocking: shape_angle x, z)
        if (memcmp(now_at, ship_was, sizeof now_at)) ship_moved = now, memcpy(ship_was, now_at, sizeof now_at);
    }
    if (deck && now - ship_moved < 1) true60::link_rides(8);
    const bool water = riding || swimming || deck;
    const bool night = hour >= 19 || hour < 5, sea = stage == "sea" && water, boat = riding || deck;
    static const double kEvery[] = {25, 12, 5};  // seconds between spawns: Sometimes, Often, Relentless
    static const size_t kMax[] = {2, 4, 7};      // monsters out at once
    direct(c, sv, link, now, night, water, boat, settled, stage);
    // for a test driver steering through WWHD_LIVE_PAD: where Link is (and his height, heading, the camera's heading),
    // five times a second
    static const bool driven = getenv("WWHD_LIVE_PAD") != nullptr;
    static double told = 0;
    if (driven && now >= told)
        told = now + 0.2, trace("extras: at %.0f %.0f %.0f yaw %d cam %d proc %#x %s%s hp %d", ldf32(link + kPos), ldf32(link + kPos + 4),
                                ldf32(link + kPos + 8), (int16_t)ld16(link + kAngleY), g_cam_yaw.load(), proc, stage.c_str(),
                                riding ? " riding" : swimming ? " swimming" : deck ? " deck" : "", ld16(sv + 2));
    // Dead calm: the wind held all but still (dKyw_custom_windpower) on the sea, set again every 2 s in case the Wind
    // Waker's song puts it back, and handed back (0) as it ends; not while the calm before another event holds it
    {
        static bool calmed = false;
        static int every = 0;
        const bool calm = live(kCalm, now) && sea && g_warn < 0;
        if (calm != calmed || (calm && ++every % 60 == 0)) wind(c, calm ? 0.02f : 0), calmed = calm;
    }
    // Fair wind: sailing, the wind blows the way the boat heads. The event wind (dKyw_evt_wind_set 0257E684, angles x and
    // y, and its go 0257E6C4), which the wind's move then takes as it is: its vector (cos x cos y, sin x, cos x sin y),
    // so y is 0x4000 less the heading; handed back as it ends (its flag, g_env_light +0xA2D, cleared)
    {
        static bool fair = false;
        const uint32_t korl = ld32(kPlay + 0x5B3C);
        const bool want = on<kFairWind>() && riding && stage == "sea" && in_mem2(korl) && !ld8(kPlay + 0x5292);
        if (want) {
            guest_call(c, GC(0x0257E684), {0, (uint32_t)(uint16_t)(0x4000 - (int16_t)ld16(korl + kAngleY))});
            guest_call(c, GC(0x0257E6C4));
        } else if (fair) {
            st8(guest_call(c, GC(0x02555D0C)) + 0xA2D, 0);
        }
        fair = want;
    }
    // Faster sailing (the upgrade): under sail (SHIP_PADDLE, 0x88) the boat is carried on along its heading by a share of
    // the speed it made this step (speedF +0x370), after its execute; its own collision puts it back if that is into land
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); proc == 0x88 && stage == "sea" && in_mem2(korl) && !ld8(kPlay + 0x5292))
        if (const double k = kSailBonus[upgrade(kUpSail)] * ldf32(korl + 0x370); k > 0) {
            const double t = (int16_t)ld16(korl + kAngleY) * M_PI / 32768;
            st32(korl + kPos, u32_bits((float)(ldf32(korl + kPos) + sin(t) * k)));
            st32(korl + kPos + 8, u32_bits((float)(ldf32(korl + kPos + 8) + cos(t) * k)));
        }
    // stormy weather: the rain count (dKyw_rain_set, 0257E7C0; 250 is a downpour) set every step while it rains, eased
    // up and down over 30 s, and 0 once at the end
    static float rain = 0;
    if (const float to = live(kStorm, now) && (night || g_choice[kStorm].load() == 1) ? 250.f : 0.f; rain > 0 || to > 0) {
        rain = to > rain ? std::min(to, rain + 250 / 900.f) : std::max(to, rain - 250 / 900.f);
        guest_call(c, GC(0x0257E7C0), {(uint32_t)rain});
    }
    // thick fog, also before and during a sea event: dKy_fog_startendz_set (0255671C; start, end, ratio in f1..f3),
    // which actors call for their own fog, every step: the world fades out from 100 to 2500 units around the camera,
    // blended with the game's own fog by the ratio, which eases up and down over 30 s
    // Fog and the Deep: further than 15000 from the island in Link's sea square the sea darkens, up to 40000; the
    // whole picture (dKy_set_allcol_ratio 02560444) and the sky (dKy_set_vrboxcol_ratio 0255A51C) are dimmed, factors
    // the environment gathers from its callers each frame, and a haze from 300 to 6000 blends in; eased over 20 s
    static float deep = 0;
    {
        const double x = ldf32(link + kPos), z = ldf32(link + kPos + 8);
        const double isle = std::hypot(x - std::round(x / 1e5) * 1e5, z - std::round(z / 1e5) * 1e5);
        const float to = live(kDeep, now) && stage == "sea" ? (float)std::clamp((isle - 15000) / 25000, 0.0, 1.0) : 0.f;
        deep = to > deep ? std::min(to, deep + 1 / 600.f) : std::max(to, deep - 1 / 600.f);
    }
    // Blood moon: eased in and out over 90 s, the environment's add colours (g_env_light's, which the game adds before
    // its clamp and never clears: dKy_actor_addcol_set 025553B8 for actors and the BG, dKy_vrbox_addcol_set 02556B38
    // for sky, haze and fog; r, g, b and a factor) red, set to 0 once as it ends; and the picture a little darker
    static float blood = 0;
    {
        const float to = live(kBlood, now) && night ? 1.f : 0.f, was = blood;
        blood = to > blood ? std::min(to, blood + 1 / 2700.f) : std::max(to, blood - 1 / 2700.f);
        if (blood > 0 || was > 0)
            for (uint32_t f : {0x025553B8u, 0x02556B38u}) c->f[1].ps0 = blood, guest_call(c, GC(f), {90, (uint32_t)-110, (uint32_t)-110});
    }
    if (deep > 0 || blood > 0) {
        c->f[1].ps0 = (1 - 0.45 * deep) * (1 - 0.2 * blood);
        guest_call(c, GC(0x02560444));
        c->f[1].ps0 = 1 - 0.55 * deep;
        guest_call(c, GC(0x0255A51C));
    }
    static float fog = 0;
    const bool sea_event = g_warn >= 0 || (g_until[kSea] > now && !always(kSea)) || (g_until[kPhantom] > now && !always(kPhantom)) ||
                           (g_until[kLeviathan] > now && !always(kLeviathan)) || (g_until[kMolgera] > now && !always(kMolgera)) ||
                           (g_until[kStalker] > now && !always(kStalker)) || (g_until[kCalm] > now && !always(kCalm));
    static bool fogged = false;
    const float to = (live(kFog, now) && (night || g_choice[kFog].load() == 1)) || sea_event ? 1.f : 0.f;
    fog = to > fog ? std::min(to, fog + 1 / 900.f) : std::max(to, fog - 1 / 900.f);  // over 30 s, as the warning
    if (fog > 0 || deep > 0 || fogged) {  // the thicker of the two, and once more at 0 to hand the game its fog back
        const bool thick = fog >= deep * 0.9f;
        // with a boss out (and no Haunted sea or Thick fog) thinner, from 1500 to 9000: the deck cannon's and the
        // helm's cameras stand far back, and a titanic Phantom is 3000 off and 3500 up
        const bool boss = g_until[kSea] <= now && !live(kFog, now) &&
                          (g_until[kPhantom] > now || g_until[kLeviathan] > now || g_until[kMolgera] > now);
        c->f[1].ps0 = !thick ? 300 : boss ? 1500 : 100, c->f[2].ps0 = !thick ? 6000 : boss ? 9000 : 2500;
        c->f[3].ps0 = thick ? fog : deep * 0.9f;
        guest_call(c, GC(0x0255671C));
        fogged = fog > 0 || deep > 0;
    }
    // endless swimming: the air meter (dComIfG_play_c::mAirMeter, 900 = full, +0x5B4C) kept full while Link swims
    if (on<kSwim>() && (ld32(link + 0x6A70) & 0x40000)) st32(kPlay + 0x5B4C, 900);
    // Night of the dead: on land at night (the sea stage's islands count)
    if ((live(kNight, now) || (on<kNight>() && live(kBlood, now))) && night && !water && settled) {
        const int level = g_choice[kNight].load();
        g_night.prune(c);
        static const Monster kDead[] = {kRedead, kRedead, kStalfos, kPoe};
        if (now >= g_night.next && g_night.ids.size() < kMax[level]) {
            spawn(c, g_night, kDead[rand() % 4], link, 450 + rand() % 350, true, 30, true);
            g_night.next = now + kEvery[level] * (live(kBlood, now) ? 0.5 : 1) * (0.6 + rand() / (double)RAND_MAX * 0.8);
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
                spawn(c, g_sea, kOship, link, 3000 + rand() % 2500, false, 0, false, 0, true);
            } else if (rand() % (giants ? 4 : 2) == 0) {  // (from under the water, where they are not seen coming)
                const uint32_t id = spawn(c, g_sea, kGyorg, link, 900 + rand() % 600, false, -40, false, g_gyctrl, true);
                if (id != 0xFFFFFFFF) g_gyorg_ids.push_back(id);
            } else {  // Seahats (one in three a giant), Kargarocs; at night also Poes drifting over the water; all made
                      // behind him, out of the camera's view, so none appears before his eyes
                static const Monster kDay[] = {kSeahat, kSeahat, kKargaroc}, kNightSea[] = {kSeahat, kKargaroc, kPoe, kPoe};
                const Monster m = giants ? kSeahat : night ? kNightSea[rand() % 4] : kDay[rand() % 3];
                const bool poe = m.proc == kPoe.proc;
                const uint32_t id = spawn(c, g_sea, m, link, poe ? 500 + rand() % 400 : 900 + rand() % 600, 1, poe ? 120 : 300, false, 0, true);
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
            g_ghost.id = spawn(c, none, ghost, link, 4500 + rand() % 2000, -1, 0, false, 0, true);
            g_ghost.next = now + 5;
        }
        for (uint32_t id : g_sea.ids)
            if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}); a && (ld16(a + 8) == kGyorg.proc || ld16(a + 8) == kSeahat.proc)) {
                float scale = kHitScale;
                for (const auto& [gid, gs] : g_giant_ids) if (gid == id && gs > kGiant) scale = gs;
                g_big_hits.push_back({a, a + (ld16(a + 8) == kGyorg.proc ? 0x1000u : 0x1600u), scale});
            }
        // Seahats keep to the anchor they were made at (+0x448/+0x450, their leash) and only attack a Link with the
        // ship-ride status (site hooks below): theirs is wherever he is now
        for (uint32_t id : g_sea.ids)
            if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}); a && ld16(a + 8) == kSeahat.proc)
                st32(a + 0x448, ld32(link + kPos)), st32(a + 0x450, ld32(link + kPos + 8));
        for (const auto& [id, scale] : g_giant_ids)
            if (const uint32_t ph = guest_call(c, GC(0x025DE50C), {id}))
                for (uint32_t off : {0x12C4u, 0x12CCu})
                    if (const uint32_t model = ld32(ph + off); in_mem2(model))
                        for (int i = 0; i < 3; i++) st32(model + 0xBC + 4 * i, u32_bits(scale));
    }
    // the Gyorgs hunt Link (Haunted sea's, Big Octo's): without their controller's path and battle state they would only
    // dive and circle (daGy_c mode at +0x3CC: 0 dive, 1 circle, 6 circle with others), so those are turned into an
    // attack on the player (modeAttackPlayerInit, 02169B48, mode 3), which gives up while he rides the boat (or stands
    // on a ship): then, as the Leviathan does, they circle the boat (their controller's clear-line flag for them set,
    // +0x42E + the child index +0x3C8) and each breaches and rams it every 5-9 s (modeAttackInit 02169B68, mode 2), from
    // a dive or its circle, kept within 2000 of him so a sailing boat does not leave them behind; attacks, damage and deletion as the game decides
    if (sea && settled && !mods::coop::sync::guest()) {  // (a guest's are the host's: their modes come from it)
        static std::unordered_map<uint32_t, double> ram_at;
        for (uint32_t id : g_gyorg_ids) {
            const uint32_t gy = id == g_gyctrl || id == g_leviathan.id ? 0 : guest_call(c, GC(0x025DE50C), {id});
            if (!gy) continue;
            const uint32_t mode = ld32(gy + 0x3CC);
            if (!boat) {
                if (mode == 0 || mode == 1 || mode == 6) guest_call(c, GC(0x02169B48), {gy});
                continue;
            }
            if (const uint32_t ctrl = ld32(gy + 0x3D4); in_mem2(ctrl) && ld32(gy + 0x3C8) < 8) st8(ctrl + 0x42E + ld32(gy + 0x3C8), 1);
            // (only at the boat he sits in: on a ship's deck a ram threw him into the sea, so there they circle and he can
            // walk it and shoot them)
            if (double& at = ram_at[id]; riding && (mode == 0 || mode == 1 || mode == 6) && now >= at) {  // (diving or circling)
                if (at > 0) guest_call(c, GC(0x02169B68), {gy}), trace("extras: a Gyorg breaches to ram the boat");
                at = now + 5 + rand() % 5;
            }
            if (static double told = 0; now - told > 3)  // (what each is doing, now and then: the trace)
                told = now, trace("extras: Gyorg %#x mode %u, %.0f off", id, mode, std::hypot(ldf32(gy + kPos) - ldf32(link + kPos), ldf32(gy + kPos + 8) - ldf32(link + kPos + 8)));
            if (const double far = std::hypot(ldf32(gy + kPos) - ldf32(link + kPos), ldf32(gy + kPos + 8) - ldf32(link + kPos + 8)); far > 2000)
                for (uint32_t o : {kPos, kPos + 8})
                    st32(gy + o, u32_bits((float)(ldf32(link + o) + (ldf32(gy + o) - ldf32(link + o)) * 2000 / far)));
        }
        if (g_gyorg_ids.empty()) ram_at.clear();
    }
    // Phantom Ganon: one near a sailing Link, at the chosen size, or (the Phantom stalker) his own size behind Link on an
    // island at night: his own execute eases the actor's scale (+0x330) to 1, so it is
    // set after it every step; his colliders' centres follow the model and their sizes are scaled by the hooks below
    const bool stalked = live(kStalker, now) && stage == "sea" && !boat && (night || !always(kStalker));
    // the Shadow Beast: a Bokoblin on the ground behind Link, drawn as the beast (mods/shadow_beast.cpp)
    const bool shadow = stalked && g_choice[kStalker].load() == 0 && beast::available();
    const bool darknut = stalked && g_choice[kStalker].load() == 2 && darknut::available();
    if (!shadow) beast::set_body(0);
    if (!darknut) darknut::set_stalker(0);
    if (darknut && settled) {  // TP's Darknut: a WW Darknut on the ground behind Link, drawn and fought as TP's
        if (g_phantom.wanted(c, now)) {
            Spawner none;
            g_phantom.id = spawn(c, none, kDarknutTn, link, 900 + rand() % 400, 1, 0, true);
            g_phantom.next = now + 20;
        }
        darknut::set_stalker(g_phantom.actor(c));
    } else if (shadow && settled) {
        if (g_phantom.wanted(c, now)) {
            Spawner none;
            static const bool ahead = getenv("WWHD_TEST_BEAST_AHEAD") != nullptr;  // test aid: in front of Link, in view
            g_phantom.id = spawn(c, none, kBokoblin, link, ahead ? 500 : 900 + rand() % 400, ahead ? -1 : 1, 0, true);
            g_phantom.next = now + 20;
        }
        beast::set_body(g_phantom.actor(c));
    } else if (((live(kPhantom, now) && sea && boat) || stalked) && settled) {
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
            g_big_hits.push_back({pg, pg + 0x1000, size});  // his sword and body colliders as big as he is, and his energy
            g_phantom_actor = pg, g_phantom_size = size;    // ball's (SetR, below); his effects too (hook_025A847C)
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
            // he flies and fights around his home point (fopAc_ac_c::home.pos, +0x2EC): Link, wherever he goes; a titanic
            // one keeps his distance, as big as he is: his home 3000 out from Link the way he already is from him, and
            // never nearer than that (his dives at Link would bring him overhead)
            double hx = ldf32(link + kPos), hz = ldf32(link + kPos + 8);
            if (size > 1) {
                const double dx = ldf32(pg + kPos) - hx, dz = ldf32(pg + kPos + 8) - hz, d = std::hypot(dx, dz);
                if (d > 1 && d < 3000)
                    st32(pg + kPos, u32_bits((float)(hx + dx / d * 3000))), st32(pg + kPos + 8, u32_bits((float)(hz + dz / d * 3000)));
                if (d > 1) hx += dx / d * 3000, hz += dz / d * 3000;
            }
            st32(pg + 0x2EC, u32_bits((float)hx));
            st32(pg + 0x2F4, u32_bits((float)hz));
        }
    }
    beast::step(c, link, now);
    darknut::replace_all(on<kTpDarknuts>());
    darknut::step(c, link);
    coop::step(c, link, on<kCoop>());
    coop::sync::step(c, link, hull);
    // Leviathan: one Gyorg (made as the others are, a child of the Haunted sea's GyCtrl, which is made here too if
    // needed) at 8x: setMtx (0216731C) copies the actor's scale (+0x330) into its model each execute, its spheres are
    // sized through the SetR hooks (g_big_hits) but its body collider's radius (+0x9B4) is a plain store each execute,
    // multiplied here after it; it circles and rams the boat (below); its health is counted at its hit test (sites
    // 02168488, 021685B0)
    g_leviathan_actor = g_leviathan.actor(c);  // the sites count its health wherever it is
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
            g_leviathan.id = spawn(c, none, kGyorg, link, 2500, 1, -40, false, g_gyctrl, true);
            if (g_leviathan.id != 0xFFFFFFFF) g_gyorg_ids.push_back(g_leviathan.id);
            g_leviathan.next = now + 10;
        }
        g_leviathan_actor = g_leviathan.actor(c);
        static uint32_t counted = 0;  // the Leviathan its health was set for
        if (g_leviathan_actor && counted != g_leviathan.id)
            counted = g_leviathan.id, g_leviathan_pool = kLeviathanHealth, g_leviathan_slain = false, st8(g_leviathan_actor + 0x3A1, 100);
        // slain: the game's own death (its burst and cry, and it sinks) plays for 4 s at its size, then it goes and
        // the event ends with it
        static double died = -1;
        if (counted == g_leviathan.id && g_leviathan_slain && died < 0 && !mods::coop::sync::guest()) {  // (a guest's: gone with the host's)
            died = now;
            trace("extras: the Leviathan is slain");
            loot(c, 200);
        }
        if (died >= 0 && (now - died > 4 || !g_leviathan_actor)) {
            died = -1, g_leviathan_slain = false;
            finish(c, kLeviathan, now, "the Leviathan is slain");
            g_leviathan.next = now + 600;  // not again at once (Always)
            g_leviathan_actor = 0;
        }
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
    // Molgera: a sea mob that hunts Link (below), 2.5x, its bite reaching 1300; no sand floor (+0x1B5C set before it
    // makes one), no forced boss camera (+0x3EC8 0, which would make its Execute force "WindBoss"); hook_020FF80C keeps
    // it from swallowing Link and shows it at its size in flight. It never stays without the event (switched off, or
    // Always and Link has landed)
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
        if (const uint32_t a = g_molgera_actor = g_molgera.actor(c); a && !mods::coop::sync::guest()) {  // (a guest's: the host's)
            st8(a + 0x1B5C, 1), st32(a + 0x3EC8, 0);
            st32(GD(kMolgeraScale), u32_bits(1.3f * 2.5f)), st32(GD(kMolgeraPush), 0), st32(GD(kMolgeraBite), u32_bits(1300.f));
            // It makes itself at the world's origin (its boss room's centre) and is put far off ahead of Link, then
            // hunts him as a Gyorg does, in its own flight through the sand, here the sea (state 10 "fly"), its head
            // 80-300 above the sea so it shows: it circles him 3000 out for 6-10 s (nearer, its body sweeps the Big ship's deck),
            // now and then (one time in three) swinging wide, 6000 out (substate 1, the FLY animation,
            // steering at its target +0x1B38 that we set ahead of it on the circle; its timers +0x1B50/52 held so it
            // stays; at its own 40 a step, or faster than his boat while it is left behind, hook_020FF80C), then charges
            // (substate 2 by its own timer: jaws open, steering at Link's eyes every step), lunges within 1200 and bites
            // (state 5: hook_020FF80C turns a bite that catches him into a miss, which sends it back to waiting, state
            // 1), and flies on to circle again; a charge that passes him (substate 3, its dive) goes back to circling.
            // It hunts him until he makes land (near an island) or the event's time is out.
            static uint32_t placed = 0;
            static double waited = 0, phase_at = 0, died = -1, blast = 0;
            static bool charging = false;
            static int dir = 1;              // which way round it circles
            static double ring = 3000;       // and how far out
            const double yaw = (int16_t)ld16(link + kAngleY) * M_PI / 32768;
            const double d = std::hypot(ldf32(a + kPos) - ldf32(link + kPos), ldf32(a + kPos + 8) - ldf32(link + kPos + 8));
            auto hunt = [a] { g_molgera_pitch = ld16(a + 0x320), st16(a + 0x1B32, 10), st16(a + 0x1B34, 0), st16(a + 0x1B52, 0); };
            auto circle = [&] { charging = false, phase_at = now + 6 + rand() % 5, dir = rand() % 2 ? 1 : -1, ring = chance(1 / 3.0) ? 6000 : 3000; };
            auto target = [a](double x, double y, double z) {
                st32(a + 0x1B38, u32_bits((float)x)), st32(a + 0x1B3C, u32_bits((float)y)), st32(a + 0x1B40, u32_bits((float)z));
            };
            if (placed != g_molgera.id) {
                placed = g_molgera.id, g_molgera_gave_up = false, died = -1, g_molgera_dying = false;
                // its head and the ring of positions and angles its body follows (+0x674, 256 x 12; +0x1274, 256 x 6),
                // all at one point, so the body is not strung out back to where it was
                st32(a + kPos, u32_bits((float)(ldf32(link + kPos) + sin(yaw) * 5000))), st32(a + kPos + 4, u32_bits(-60.f));
                st32(a + kPos + 8, u32_bits((float)(ldf32(link + kPos + 8) + cos(yaw) * 5000)));
                for (uint32_t i = 0; i < 256; i++) {
                    for (uint32_t k = 0; k < 12; k += 4) st32(a + 0x674 + 12 * i + k, ld32(a + kPos + k));
                    for (uint32_t k = 0; k < 6; k += 2) st16(a + 0x1274 + 6 * i + k, ld16(a + 0x320 + k));
                }
                hunt(), circle();
                trace("extras: Molgera surfaces far off and hunts Link");
            }
            const int state = ld16(a + 0x1B32) == 13 ? 10 : ld16(a + 0x1B32), sub = ld16(a + 0x1B34);
            const double heading = (int16_t)ld16(a + 0x322) * M_PI / 32768;
            if (state != 1) waited = now;
            if (died >= 0) {  // slain: it dives under, bursting along its way (its own death's blasts and sound)
                if (state != 10 || sub != 1) hunt();
                st16(a + 0x1B50, 100), st16(a + 0x1B52, 200);
                target(ldf32(a + kPos) + sin(heading) * 1500, -4000, ldf32(a + kPos + 8) + cos(heading) * 1500);
                if (now - blast >= 0.13) {
                    blast = now;
                    effect(c, 0x825D, {ldf32(a + kPos) + rand() % 300 - 150, ldf32(a + kPos + 4) + rand() % 300 - 150, ldf32(a + kPos + 8) + rand() % 300 - 150}, 5);
                    guest_call(c, GC(0x025E1988), {0x5801});
                }
            } else if (state == 10) {
                if (sub == 1 || sub == 2) {
                    st16(a + 0x1B50, 150);
                    st32(a + kPos + 4, u32_bits(std::clamp((float)ldf32(a + kPos + 4), 80.f, 300.f)));  // head and body above the waves
                }
                if (sub == 2) st32(a + 0x370, u32_bits(34.f));
                if (sub == 1 && !charging) {
                    st16(a + 0x1B52, 200);
                    if (g_molgera_gave_up) {  // off and away from him
                        target(ldf32(a + kPos) + sin(heading) * 5000, 150, ldf32(a + kPos + 8) + cos(heading) * 5000);
                    } else {  // on the circle about him, ahead of where it is
                        const double b = atan2(ldf32(a + kPos) - ldf32(link + kPos), ldf32(a + kPos + 8) - ldf32(link + kPos + 8)) + dir * 1.0;
                        target(ldf32(link + kPos) + sin(b) * ring, 150, ldf32(link + kPos + 8) + cos(b) * ring);
                        if (now >= phase_at) charging = true, st16(a + 0x1B52, 0), trace("extras: Molgera charges");
                    }
                }
                if (sub == 3) hunt(), circle();  // its charge passed him: round again
                // left behind, it comes on faster than his boat can sail (a tenth over its top speed, 55, with the
                // sailing upgrade's share): it never gives up, until he makes land or its time is out
                g_molgera_pace = d > ring + 1000 ? 1.1 * 55 * (1 + kSailBonus[upgrade(kUpSail)]) / 40 : 1;
                if (sub == 2 && d < 1200 && !g_molgera_gave_up) {
                    st16(a + 0x1B32, 5), st16(a + 0x1B34, 0), st32(a + 0x370, 0);
                }
            } else if (state == 1 && now - waited > 0.5) {  // after a bite: on, to circle again
                hunt(), circle();
                trace("extras: Molgera flies on after its bite");
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
            // its health: in the game only its tongue, pulled out by the hookshot, can be hurt, and it dies in its boss
            // demo. Here every hit on the nine spheres of its head, hip and fins (+0x1E78, 0x12C apart, set every step,
            // hit by bombs, arrows and the sword) counts, the strongest of a step (they all see one hit): ChkTgHit
            // 025162A4, GetTgHitObj 02516300 (its AT power u8 +0x14), ClrTgHit 0251621C. When it is gone, so is
            // Molgera, and the event ends
            static uint32_t counted = 0;
            if (counted != g_molgera.id) counted = g_molgera.id, g_molgera_pool = kMolgeraHealth;
            int damage = 0;
            for (uint32_t i = 0; i < 9; i++) {
                const uint32_t sph = a + 0x1E78 + i * 0x12C;
                if (!guest_call(c, GC(0x025162A4), {sph})) continue;
                if (const uint32_t h = guest_call(c, GC(0x02516300), {sph}); in_mem2(h)) damage = std::max(damage, std::max<int>(1, ld8(h + 0x14)));
                guest_call(c, GC(0x0251621C), {sph});
            }
            static double last_hit = -1;  // one hit per 0.3 s: a bomb's burst, a sword's swing last a few steps
            if (died >= 0) damage = 0;
            if (damage && now - last_hit >= 0.3) trace("extras: Molgera is hit for %d (%d of %d left)", damage, g_molgera_pool - damage, kMolgeraHealth);
            if ((damage && now - last_hit >= 0.3 && (last_hit = now, g_molgera_pool -= damage) <= 0) || (died < 0 && g_molgera_pool <= 0)) {
                trace("extras: Molgera is slain");
                loot(c, 300);
                died = blast = now, g_molgera_dying = true;
                effect(c, 0x8265, pos_of(a), 2.5f);  // its last hit's burst
                guest_call(c, GC(0x025E1988), {0x591B});
            } else if (died >= 0 && now - died > 1.8) {  // under: gone, and the event ends
                died = -1, g_molgera_dying = false;
                finish(c, kMolgera, now, "Molgera is slain");
                g_molgera.next = now + 600;  // not again at once (Always)
            } else if (always(kMolgera) && g_molgera_gave_up && d > 5000) {  // no event to end: it goes, and comes back later
                remove_molgera(c);
                g_molgera.next = now + 300;
            }
        }
    }
    // Helmaroc King: the Ganon's Tower boss (daBdk, proc 0xEE, param 0) at sea. Its create puts it at its tower's
    // centre in its tower flight (action +0x3DA 0x64, state +0x3DC) and starts the tower's battle music; here it is
    // flown by hand, after its execute (position, old position, home, heading), so only its wings and its own stomps
    // are its: it circles 2000 out and 900 up, and every 8-12 s dives through Link. Its timer +0x3D8 is held under
    // 100 (at 100 it sets a zone switch and deletes the gulls); any other action (its tower landings, the Skull Hammer's
    // down and its demos) is turned back into the flight. Hits on its head, crest, body and feet (+0x13F0, +0x151C,
    // +0x1648, +0x1774, +0x18A0, 0x12C apart) count as Molgera's do, against kHelmarocHealth. After 100 s unbeaten, with
    // Link riding the boat, its next dive snatches the boat: it holds it under its feet for 2 s, then drops
    // him (a stage change) on another square of the sea, in the open water beside his boat or, half the time, ashore
    // on that square's island (a second stage change once its room is there, onto the first land found on the way in)
    {
        static int hop_room = -1;      // the island drop's second hop: the room, and until when it is tried
        static double hop_until = -1;
        static V3 hop_from, hop_isle;  // where the boat came in, the island's centre
        static float hop_highest = 0;
        if (hop_until > 0 && now >= hop_until) hop_until = -1, trace("extras: no land found for the Helmaroc King's drop (highest %.0f)", hop_highest);
        if (const uint32_t korl = ld32(kPlay + 0x5B3C); hop_until > 0 && settled && !fresh && in_mem2(korl) && now >= hop_until - 18) {
            static const uint32_t buf = mem::host_alloc(0x10);
            const double len = std::hypot(hop_isle.x - hop_from.x, hop_isle.z - hop_from.z);
            float highest = -1e9f;
            for (double t = 0; t < len; t += 400) {
                const double x = hop_from.x + (hop_isle.x - hop_from.x) * t / len, z = hop_from.z + (hop_isle.z - hop_from.z) * t / len;
                st32(buf, u32_bits((float)x)), st32(buf + 4, u32_bits(900.f)), st32(buf + 8, u32_bits((float)z));
                guest_call(c, GC(0x02549F4C), {buf});  // (from 900, as the Big ship's land check: the start height back is nothing)
                highest = std::max(highest, (float)c->f[1].ps0 == 900.f ? -1e9f : (float)c->f[1].ps0);
                if (const float ground = (float)c->f[1].ps0; ground > 20 && ground != 900.f) {
                    const float at[3] = {(float)x, ground + 5, (float)z};
                    const float ship[3] = {(float)ldf32(korl + kPos), (float)ldf32(korl + kPos + 4), (float)ldf32(korl + kPos + 8)};
                    std::string why;
                    if (ss::warp(c, "sea", hop_room, at, (int16_t)(atan2(hop_isle.x - x, hop_isle.z - z) * 32768 / M_PI), ship, false, why))
                        hop_until = -1, trace("extras: the Helmaroc King drops Link ashore at %.0f %.0f %.0f", x, ground, z);
                    break;
                }
            }
            hop_highest = highest;
        }
        if (g_helmaroc.id != 0xFFFFFFFF && !live(kHelmaroc, now) && g_helmaroc.actor(c)) guest_call(c, GC(0x025D57E4), {g_helmaroc.id});
        if (live(kHelmaroc, now) && sea && settled) {
            if (g_helmaroc.wanted(c, now)) {
                Spawner none;
                g_helmaroc.id = spawn(c, none, Monster{0xEE, -1, 0}, link, 4000, -1, 1500, false, 0, true);
                g_helmaroc.next = now + 10;
            }
            if (const uint32_t a = g_helmaroc.actor(c)) {
                enum Flight { kCircle, kDive, kPass, kCarry };
                static uint32_t placed = 0;
                static Flight flight = kCircle;
                static double began = 0, phase_at = 0, last = 0, angle = 0, pecked = 0, last_hit = -1;
                static V3 at, heading;
                static int pool = kHelmarocHealth;
                const uint32_t korl = ld32(kPlay + 0x5B3C);
                const V3 l = pos_of(link);
                if (placed != g_helmaroc.id) {
                    placed = g_helmaroc.id, flight = kCircle, began = last = now, phase_at = now + 8 + rand() % 5, pool = kHelmarocHealth;
                    angle = atan2(ldf32(a + kPos) - l.x, ldf32(a + kPos + 8) - l.z);
                    at = {l.x + sin(angle) * 4000, 1500, l.z + cos(angle) * 4000};
                    trace("extras: the Helmaroc King comes down out of the sky");
                }
                const double dt = std::min(0.1, now - last);
                last = now;
                if (const int16_t act = (int16_t)ld16(a + 0x3DA); act != 0x64) st16(a + 0x3DA, 0x64), st16(a + 0x3DC, 0);
                if ((int16_t)ld16(a + 0x3D8) >= 90) st16(a + 0x3D8, 0);
                const double to_link = std::hypot(l.x - at.x, l.z - at.z);
                auto toward = [&](V3 goal, double speed) {
                    const V3 d{goal.x - at.x, goal.y - at.y, goal.z - at.z};
                    const double n = std::max(1.0, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
                    heading = {d.x / n, d.y / n, d.z / n};
                    const double step = std::min(n, speed * dt);
                    at = {at.x + heading.x * step, at.y + heading.y * step, at.z + heading.z * step};
                };
                // (test aids: WWHD_HELMAROC_SNATCH=<s> its snatch after that long, WWHD_HELMAROC_DROP=sea|ashore where)
                static const double snatch_after = getenv("WWHD_HELMAROC_SNATCH") ? atof(getenv("WWHD_HELMAROC_SNATCH")) : 100;
                const bool snatcher = now - began > snatch_after && riding && in_mem2(korl);
                switch (flight) {
                case kCircle: {  // round him, 2000 out and 900 up (in the camera's view), closing on the circle from wherever it is
                    angle += 1900.0 / 2000 * dt;
                    toward({l.x + sin(angle) * 2000, 900, l.z + cos(angle) * 2000}, 3200);
                    if (now >= phase_at) flight = kDive, trace("extras: the Helmaroc King dives%s", snatcher ? " to snatch the boat" : "");
                    break;
                }
                case kDive:  // straight at him, low over his head
                    toward({l.x, l.y + 250, l.z}, 2900);
                    if (to_link < 450) {
                        if (snatcher) {
                            flight = kCarry, phase_at = now + 2;
                            trace("extras: the Helmaroc King snatches the boat");
                        } else {
                            if (now - pecked > 3) pecked = now, st16(sv + 2, (uint16_t)std::max(0, ld16(sv + 2) - 4));  // a heart
                            flight = kPass, phase_at = now + 1.5;
                        }
                    }
                    break;
                case kPass:  // on past him, climbing again
                    toward({at.x + heading.x * 3000, 900, at.z + heading.z * 3000}, 2900);
                    if (now >= phase_at) flight = kCircle, phase_at = now + 8 + rand() % 5, angle = atan2(at.x - l.x, at.z - l.z);
                    break;
                case kCarry: {  // down on the boat, its feet on it (the boat draws where its own execute left it, so
                                // it is not lifted: the stage change's fade is the flight)
                    if (in_mem2(korl)) toward({ldf32(korl + kPos), ldf32(korl + kPos + 4) + 450, ldf32(korl + kPos + 8)}, 2900);
                    if (now < phase_at) break;
                    // dropped on another square (not this one, not Forsaken Fortress's): its open water 30000 from the
                    // island, or 14000 out, by boat, for the hop ashore
                    const int here = sea_room((float)l.x, (float)l.z);
                    int room;
                    do room = 1 + rand() % 49; while (room == here || room == 1);
                    const V3 isle{((room - 1) % 7 - 3) * 1e5, 0, ((room - 1) / 7 - 3) * 1e5};
                    static const char* drop = getenv("WWHD_HELMAROC_DROP");
                    const bool ashore = drop ? drop[0] == 'a' : rand() % 2;
                    const double b = rand() / (double)RAND_MAX * 2 * M_PI, out = ashore ? 14000 : 30000;
                    const float to[3] = {(float)(isle.x + sin(b) * out), 0, (float)(isle.z + cos(b) * out)};
                    const float ship[3] = {to[0] + (ashore ? 0 : 700.f), 0, to[2]};
                    std::string why;
                    if (ss::warp(c, "sea", room, to, (int16_t)(atan2(-sin(b), -cos(b)) * 32768 / M_PI), ship, ashore, why)) {
                        trace("extras: the Helmaroc King drops Link %s in room %d", ashore ? "by an island" : "in the open sea", room);
                        if (ashore) hop_room = room, hop_until = now + 25, hop_from = {to[0], 0, to[2]}, hop_isle = isle;
                        finish(c, kHelmaroc, now, "Link was snatched");
                    } else if (now - phase_at > 3) {  // not free to go (an event): it lets go of the boat
                        trace("extras: the Helmaroc King lets go (%s)", why.c_str());
                        finish(c, kHelmaroc, now, "it let go");
                    }
                    break;
                }
                }
                for (uint32_t o : {kPos, 0x300u, 0x2ECu})
                    st32(a + o, u32_bits((float)at.x)), st32(a + o + 4, u32_bits((float)at.y)), st32(a + o + 8, u32_bits((float)at.z));
                const uint16_t yaw = (uint16_t)(int16_t)(atan2(heading.x, heading.z) * 32768 / M_PI);
                st16(a + kAngleY, yaw), st16(a + 0x322, yaw);
                st8(a + kRoom, ld8(link + kRoom));
                // its health (as Molgera's, above)
                int damage = 0;
                for (uint32_t sph : {0x13F0u, 0x151Cu, 0x1648u, 0x1774u, 0x18A0u}) {
                    if (!guest_call(c, GC(0x025162A4), {a + sph})) continue;
                    if (const uint32_t h = guest_call(c, GC(0x02516300), {a + sph}); in_mem2(h)) damage = std::max(damage, std::max<int>(1, ld8(h + 0x14)));
                    guest_call(c, GC(0x0251621C), {a + sph});
                }
                if (damage && now - last_hit >= 0.3) {
                    last_hit = now, pool -= damage;
                    trace("extras: the Helmaroc King is hit for %d (%d of %d left)", damage, pool, kHelmarocHealth);
                    if (pool <= 0) {
                        loot(c, 300);
                        effect(c, 0x8265, pos_of(a), 2.5f);
                        guest_call(c, GC(0x025E1988), {0x591B});
                        finish(c, kHelmaroc, now, "the Helmaroc King is brought down");
                    }
                }
            }
        }
    }
    // Wandering cyclones: Cyclos's cyclone (daTornado, Ytrnd00, proc 0x1BB, param 0) made 12000 ahead of a sailing Link,
    // wandering across his course at 18 a step (the boat makes 40 under full sail): it is put on its own course after its
    // execute, which would home in on him, and its home (+0x2EC, which it keeps within 7500 of) with it. It catches the
    // boat from within 10000, too far to steer clear of: hook_024C8A58 hides the ship-ride status from it beyond 3000.
    // Once it has the boat (the boat's +0x700 its id) it is left alone: it orbits him in, lifts him and throws him to
    // another sea square (a stage change). Made even with Cyclos beaten (its event bit 0x2710 read as unset, site hooks)
    g_cyclone_actor = 0;
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); live(kCyclone, now) && sea && settled && in_mem2(korl)) {
        static V3 at;
        static double course = 0;
        static uint32_t placed = 0;
        if (g_cyclone.wanted(c, now)) {
            Spawner none;
            g_cyclone.id = spawn(c, none, Monster{0x1BB, -1, 0}, link, 12000, -1, 0, false, 0, true);
            g_cyclone.next = now + 10;
        }
        if (const uint32_t t = g_cyclone_actor = g_cyclone.actor(c)) {
            if (placed != g_cyclone.id) {
                placed = g_cyclone.id, at = pos_of(t);
                course = (int16_t)ld16(link + kAngleY) * M_PI / 32768 + (rand() % 2 ? 1 : -1) * (M_PI / 2 + (rand() / (double)RAND_MAX - 0.5));
                trace("extras: a cyclone wanders across Link's course");
            }
            if (ld32(korl + 0x700) != g_cyclone.id) {
                at.x += sin(course) * 18, at.z += cos(course) * 18;
                for (uint32_t o : {kPos, 0x2ECu}) st32(t + o, u32_bits((float)at.x)), st32(t + o + 8, u32_bits((float)at.z));
                st8(t + kRoom, ld8(link + kRoom));
            }
            if (std::hypot(at.x - ldf32(link + kPos), at.z - ldf32(link + kPos + 8)) > 25000) finish(c, kCyclone, now, "it wandered off");
        }
    }
    // Maelstrom: the whirlpool of Big Octo's fight (daObjAuzu, proc 0x11B; param 0x001100FF: type 1, which needs no
    // event bit, growing only when told, +0x434, 0.02 a step, no switch) opened 3000 off a sailing Link: from within
    // 5000 it takes the boat (its +0x708 the whirlpool's id), which then circles it with no steering, drawn slowly in.
    // Wrenching the stick from side to side (its travel since the last step, summed) breaks him free; after 45 s held it
    // takes two hearts and lets go. Under 500 from its centre the boat would be dragged down and the stage reloaded
    // at a spawn point the open sea may not have: it lets go well before, at 1200
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); g_whirl.id != 0xFFFFFFFF && (g_whirl_fading || !live(kMaelstrom, now)) && now >= g_whirl.next) {
        if (g_whirl.actor(c)) guest_call(c, GC(0x025D57E4), {g_whirl.id});  // faded away (or switched off): gone
        g_whirl = Single{}, g_whirl_fading = false;
        if (always(kMaelstrom)) g_whirl.next = now + 300;  // (Always: the next one a while later)
    } else if (live(kMaelstrom, now) && in_mem2(korl) && settled) {
        static uint32_t placed = 0;
        static double caught_at = -1, wrench = 0;
        static float lx = 0, ly = 0;
        if (g_whirl_fading) {
        } else if (g_whirl.wanted(c, now)) {  // (beside the boat, wherever Link is: on the craft in tow it holds them both)
            Spawner none;
            g_whirl.id = spawn(c, none, Monster{0x11B, -1, 0x001100FF}, korl, 3000, 0, 0, false, 0, true);
            g_whirl.next = now + 10;
        } else if (const uint32_t w = g_whirl.actor(c)) {
            if (placed != g_whirl.id) placed = g_whirl.id, caught_at = -1, wrench = 0, trace("extras: the sea begins to turn: a whirlpool opens");
            st8(w + 0x434, 1), st8(w + kRoom, ld8(link + kRoom));
            const bool caught = ld32(korl + 0x708) == g_whirl.id;
            if (caught && caught_at < 0) caught_at = now, trace("extras: the whirlpool has the boat");
            if (caught) wrench += std::fabs(g_pad_lx.load() - lx) + std::fabs(g_pad_ly.load() - ly);
            lx = g_pad_lx.load(), ly = g_pad_ly.load();
            const double d = std::hypot(ldf32(korl + kPos) - ldf32(w + kPos), ldf32(korl + kPos + 8) - ldf32(w + kPos + 8));
            if (caught && wrench >= 25) {
                finish(c, kMaelstrom, now, "Link breaks free");
            } else if (caught && (now - caught_at > 45 || d < 1200)) {
                st16(sv + 2, (uint16_t)std::max(0, ld16(sv + 2) - 8));
                finish(c, kMaelstrom, now, "it takes two hearts and lets go");
            }
        }
    }
    // Pirate armada: four cannon boats (daOship, a plain one: angle.x 0xFFFF, model type 0xFF; with 0x00FF it is the
    // gold-ship kind) come one after another, 30 s apart, each from its own bearing 9000 off (ahead, then to either
    // side), and each is sailed by us on its own (without a path nothing moves one, and its execute never writes its
    // x, z or heading: pos, current.angle.y +0x322 for its colliders, shape_angle.y for its model and guns): it closes
    // on Link at 24 a step (speedF 8 for its wake, which also holds its fire) until 4000 off, where it heaves to
    // (speedF 0) and fires by its own modes; further than 6000 again and it comes on after him. A boat sunk leaves the
    // others where they are. All four come and sunk: the loot, and the event ends
    if (live(kArmada, now) && sea && settled) {
        struct Raider { uint32_t id; V3 at; double made; bool hove = false; };
        static std::vector<Raider> raiders;
        static double next = 0, wait = 0;
        constexpr int kBoats = 4;
        constexpr double kBearing[kBoats] = {0, 0.7, -0.7, 0.3};
        if (g_armada_for != g_until[kArmada] && now >= wait)
            g_armada_for = g_until[kArmada], g_armada_made = 0, raiders.clear(), next = now, trace("extras: a pirate armada comes over the horizon");
        if (g_armada_for == g_until[kArmada]) {
            if ((int)g_armada_made < kBoats && now >= next) {
                const double b = (int16_t)ld16(link + kAngleY) * M_PI / 32768 + kBearing[g_armada_made];
                const V3 at{ldf32(link + kPos) + sin(b) * 9000, 0, ldf32(link + kPos + 8) + cos(b) * 9000};
                if (const uint32_t id = spawn(c, g_armada, kOship, link, 9000, -1, 0, false, 0, true); id != 0xFFFFFFFF)
                    raiders.push_back({id, at, now}), g_armada_made++, trace("extras: cannon boat %d of the armada comes", (int)g_armada_made);
                next = now + 30;
            }
            g_armada.prune(c);
            std::erase_if(raiders, [&](const Raider& r) {  // (sunk: gone, once it has had time to come to life)
                return now - r.made > 3 && std::find(g_armada.ids.begin(), g_armada.ids.end(), r.id) == g_armada.ids.end();
            });
            if ((int)g_armada_made >= kBoats && raiders.empty()) {
                loot(c, 200);
                g_armada_for = -1, g_armada_made = 0, wait = now + 300;  // (Always: the next one a while later)
                finish(c, kArmada, now, "the armada is sunk");
            }
        }
        for (Raider& r : raiders)
            if (const uint32_t a = guest_call(c, GC(0x025DE50C), {r.id})) {
                const double d = std::hypot(ldf32(link + kPos) - r.at.x, ldf32(link + kPos + 8) - r.at.z);
                r.hove = d < (r.hove ? 6000 : 4000);
                const double course = atan2(ldf32(link + kPos) - r.at.x, ldf32(link + kPos + 8) - r.at.z);
                if (!r.hove) r.at.x += sin(course) * 24, r.at.z += cos(course) * 24;
                const uint16_t yaw = (uint16_t)(int16_t)(course * 32768 / M_PI);
                st32(a + kPos, u32_bits((float)r.at.x)), st32(a + kPos + 8, u32_bits((float)r.at.z));
                st16(a + 0x322, yaw), st16(a + kAngleY, yaw);
                st32(a + 0x370, u32_bits(r.hove ? 0.f : 8.f));
                st8(a + kRoom, ld8(link + kRoom));
            }
    }
    // Big Octo, its own fight with Gyorgs as well: daDaiocta (proc 0xE1, param 0xFFFFFF00: type 0, four eyes, its own
    // reach, no swallow destination, no switch) made near a sailing Link. It loads its archive itself and makes its eyes
    // (0xE2) and whirlpool (Auzu, its id at +0x2844) itself, then waits hidden 3000 under the sea for a Link in reach,
    // so once it has had a moment to come to life it is told to rise (its modeProc 0211889C: init, mode 1). Then all is
    // the game's: its whirlpool sweeps the boat round it, its swallow, its eyes dying to arrows, the hookshot, the
    // Boomerang and bombs, and its death (hook_0211BFFC marks it: the loot; the event ends once it is gone, or 15 s on)
    if (live(kOcto, now) && sea && settled) {
        static double made_at = 0;
        static bool risen = false;
        static double died = -1;
        if (g_octo_for != g_until[kOcto]) g_octo_for = g_until[kOcto], g_octo = Single{}, risen = false, g_octo_dead = false, g_octo_gyorgs = 5, died = -1;
        if (g_octo_dead && died < 0) died = now, loot(c, 300), trace("extras: Big Octo is slain");
        if (died >= 0 && (!g_octo.actor(c) || now - died > 15)) finish(c, kOcto, now, "Big Octo is slain");
        if (!risen && g_octo.wanted(c, now)) {
            Spawner none;
            g_octo.id = spawn(c, none, Monster{0xE1, -1, 0xFFFFFF00}, link, 2500, -1, 0, false, 0, true);
            g_octo.next = now + 10, made_at = now;
        }
        if (const uint32_t o = g_octo_actor = g_octo.actor(c)) {
            st8(o + kRoom, ld8(link + kRoom));
            if (!risen && now - made_at > 2) {
                guest_call(c, GC(0x0211889C), {o, 0, 1}), risen = true;
                trace("extras: the sea opens and Big Octo rises");
            }
        }
    }
    // Something in the Lion: a laugh (CV_G_SHIP_LAUGH 0x4925), and for a few seconds the boat swerves (its yaw rate
    // m03B8, +0x6A0) and races (speedF +0x370 35, its steering's speed limit m03E0 +0x6C4 raised so it
    // is not cut back), each step after its execute, whatever the stick and the wind say
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); in_mem2(korl) && riding) {
        static double for_event = -1;
        static int16_t swerve = 0;
        if (g_until[kWrongLion] > now) {
            if (for_event != g_until[kWrongLion]) {
                for_event = g_until[kWrongLion], swerve = rand() % 2 ? 0x300 : -0x300;
                guest_call(c, GC(0x025E1988), {0x4925});
                trace("extras: something in the Lion laughs and takes the helm");
            }
            st16(korl + 0x6A0, (uint16_t)swerve);
            st32(korl + 0x6C4, u32_bits(40.f)), st32(korl + 0x370, u32_bits(35.f));
            if (static int every = 0; ++every % 45 == 0) swerve = (int16_t)-swerve;  // weaving
        }
    }
    // the Gyorgs with Big Octo: their controller first (a Gyorg's create fails while its parent is not yet found)
    if (g_octo_gyorgs > 0 && sea && settled) {
        if (g_gyctrl == 0xFFFFFFFF || !guest_call(c, GC(0x025DE50C), {g_gyctrl})) {
            static double asked = 0;
            if (now - asked > 2) {
                asked = now;
                Spawner none;
                g_gyctrl = spawn(c, none, kGyCtrl, link, 0, false, 0, false);
                g_gyorg_ids.push_back(g_gyctrl);
            }
        } else {
            for (; g_octo_gyorgs > 0; g_octo_gyorgs--)
                if (const uint32_t id = spawn(c, g_sea, kGyorg, link, 900 + rand() % 400, 0, -40, false, g_gyctrl, true); id != 0xFFFFFFFF) g_gyorg_ids.push_back(id);
        }
    }
    // The drowned and the Boarding party: monsters made on the deck Link stands on (spawn's ground check finds it, and
    // nothing over the water), carried by it as he is (their dBgS_Acch rides a moving BG through MoveBgCrrPos too).
    // The drowned: a ReDead every 12 s, three at most. Boarders: Bokoblins (daBk, proc 0xBD, 0xFFFFFF00: type 0, which
    // goes for Link, no path or switch), two from each cannon boat that comes within 4500 of the ship (the armada heaves to at 4000), once; told to
    // attack at once (daBk's search_sp, 1046232D, which each Bokoblin's create sets back to 0)
    if (const uint32_t ship = beedle ? beedle : hull; ship && deck && settled) {
        if (live(kDrowned, now) && (night || g_until[kDrowned] > now)) {
            static double next = 0;
            g_night.prune(c);
            if (now >= next && g_night.ids.size() < (beedle && g_craft == &kRaftCraft ? 2u : 3u)) {  // (the raft: two at most)
                spawn(c, g_night, kRedead, link, 200 + rand() % 250, 1, 30, true, 0, false, true);  // behind him: over the rail unseen
                next = now + 12;
            }
        }
        if (on<kBoarding>()) {
            static std::vector<uint32_t> boarded;  // the boats that have sent theirs
            std::vector<uint32_t> boats = g_sea.ids;
            boats.insert(boats.end(), g_armada.ids.begin(), g_armada.ids.end());
            for (uint32_t id : boats)
                if (const uint32_t a = guest_call(c, GC(0x025DE50C), {id}); a && ld16(a + 8) == kOship.proc &&
                    std::find(boarded.begin(), boarded.end(), id) == boarded.end() &&
                    std::hypot(ldf32(a + kPos) - ldf32(ship + kPos), ldf32(a + kPos + 8) - ldf32(ship + kPos + 8)) < 4500) {
                    boarded.push_back(id);
                    for (int i = 0; i < 2; i++) spawn(c, g_boarders, Monster{0xBD, -1, 0xFFFFFF00}, link, 200 + rand() % 250, 1, 30, true, 0, false, true);
                    trace("extras: a cannon boat comes alongside and Bokoblins leap aboard");
                }
            g_boarders.prune(c);
            if (!g_boarders.ids.empty()) st8(GD(0x1046232D), 1);
        }
    }
    // the event's bosses are kept in Link's room (+0x326): made in the room he was in, they went with it when it unloaded
    // behind him as he sailed on (Phantom Ganon's event ended "he is gone")
    for (Single* b : {&g_phantom, &g_leviathan, &g_molgera})
        if (const uint32_t a = b->actor(c)) st8(a + kRoom, ld8(link + kRoom));
    horror_audio(c, link, now, sea_event || (night && ((g_until[kNight] > now && !always(kNight)) || (on<kBlood>() && g_until[kBlood] > now && !always(kBlood)))), on<kAudio>());
    // the GamePad since the last step
    static uint32_t pad_prev = 0;
    const uint32_t pad_hold = g_pad_hold.load(), pressed = g_pad_seen.exchange(0) & ~pad_prev;
    pad_prev = pad_hold;
    // Big ship: Tetra's ship, a boat of its own. It is made at anchor 3500 off the King of Red Lions' left (his -x) once
    // he is out at sea (not near an island, no land under it), lies where it is left and moves only from its helm; gone
    // (its room unloaded as he sailed away, a stage change) or left more than 20000 behind, it is made at anchor near him
    // again on his next outing (once he has been ashore), not on this one. Stopped beside it with it on his left, B (SHIP_GET_OFF, which jumps toward his -x) leaps Link onto its deck;
    // A at its rail by him takes him back in the boat; A while swimming by its side climbs aboard. While it sails from its
    // helm, the King of Red Lions comes along beside it (on its +x flank). It floats as the game floats it (its origin at
    // the waterline, the deck 400 above the sea).
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); own_ship && stage == "sea" && in_mem2(korl) && settled) {
        constexpr double kLionX = 1050, kLionZ = 250;  // his place beside it while it sails (its +x flank, by the main deck)
        const int16_t yaw = (int16_t)ld16(korl + kAngleY);
        auto put = [](uint32_t h, V3 p, int16_t a) {
            st32(h + kPos, u32_bits((float)p.x)), st32(h + kPos + 4, u32_bits((float)p.y)), st32(h + kPos + 8, u32_bits((float)p.z));
            st16(h + 0x322, (uint16_t)a), st16(h + kAngleY, (uint16_t)a);
        };
        // land under its outline, there (ground check from 900 up, under its own sail and crow's nest, which the
        // check would find: 02549F4C returns the start height when it finds nothing, below 5 over water)
        auto land = [c](V3 o, int16_t a) {
            static const uint32_t buf = mem::host_alloc(0x10);
            for (V3 l : {V3{0, 0, 2700}, V3{0, 0, -2200}, V3{1000, 0, 250}, V3{-1200, 0, 250}}) {
                const V3 w = to_world(o, a, l);
                st32(buf, u32_bits((float)w.x)), st32(buf + 4, u32_bits(900.f)), st32(buf + 8, u32_bits((float)w.z));
                guest_call(c, GC(0x02549F4C), {buf});
                if (const float g = (float)c->f[1].ps0; g != 900.f && g >= 5) {
                    trace("extras: Big ship: land %.0f at local %.0f %.0f", g, l.x, l.z);
                    return true;
                }
            }
            return false;
        };
        auto isle = [](V3 p) { return std::hypot(p.x - std::round(p.x / 1e5) * 1e5, p.z - std::round(p.z / 1e5) * 1e5); };
        // online co-op: a guest's ship is the host's, made where it is and put there every step (100 ms behind) in place
        // of the helm, its sail as the host's; never left behind
        V3 hp{};
        int16_t hy = 0;
        bool hsail = false;
        const bool follow = mods::coop::sync::guest() && coop_host_ship(hp, hy, hsail);
        static V3 home;       // where it is made at anchor, and its heading
        static int16_t home_yaw = 0;
        static V3 left_at;    // where it was last seen, and its heading: remade there when its room's unloading took it
        static int16_t left_yaw = 0;
        static bool left_one = false;
        // when it was made: left behind or gone, it waits for his next outing, once he has been ashore since (off the
        // water over 3 s, as the director has it: the jump from its rail into the boat is not ashore)
        static double made_at = -1;
        if (fresh) left_one = false, made_at = -1;
        if (g_cabin_return.pending) left_at = g_cabin_return.ship, left_yaw = g_cabin_return.yaw, left_one = true, g_cabin_return.pending = false;
        const double away = hull ? std::hypot(ldf32(hull + kPos) - ldf32(link + kPos), ldf32(hull + kPos + 8) - ldf32(link + kPos + 8)) : 0;
        if (hull && away < 15000)  // kept in Link's room while he is near, so the room it was made in can unload
            st8(hull + kRoom, ld8(link + kRoom)), left_at = pos_of(hull), left_yaw = (int16_t)ld16(hull + kAngleY), left_one = true;
        if (hull && riding && away > 20000 && !follow) {
            // with its children, which its delete leaves (Psail, its flag, the catapult Touseki, the wheel Kaji, the
            // door: their ids at +0x478..+0x498, decomp +0x2E8..+0x308): the sail and the catapult read the ship
            // through a static pointer every step and crashed with it gone; and our deck cannons
            for (uint32_t o = 0x478; o <= 0x498; o += 8) guest_call(c, GC(0x025D57E4), {ld32(hull + o)});
            guest_call(c, GC(0x025D57E4), {g_bigship.id});
            for (Single& s : g_cannon)
                if (s.actor(c)) guest_call(c, GC(0x025D57E4), {s.id}), s = Single{};
            g_bigship = Single{}, left_one = false;
            trace("extras: Big ship left far behind: it waits for his next outing");
        } else if (!hull) {
            const bool back = left_one && std::hypot(left_at.x - ldf32(link + kPos), left_at.z - ldf32(link + kPos + 8)) < 15000;
            static const double off = getenv("WWHD_BIGSHIP_OFF") ? atof(getenv("WWHD_BIGSHIP_OFF")) : 3500;  // (test aid)
            V3 to = back ? left_at : to_world(pos_of(korl), yaw, {-off, 0, 0});
            to.y = ldf32(korl + kPos + 4);
            if ((follow || ((riding || back) && (back || (made_at < g_out_since && isle(to) >= 6000 && !land(to, yaw))))) && g_bigship.wanted(c, now)) {
                made_at = now;
                Spawner none;
                g_bigship.id = spawn(c, none, kPirates, korl, 0, false, 0, false);
                g_bigship.next = now + 5;
                home = follow ? hp : to, home_yaw = follow ? hy : back ? left_yaw : yaw;
            }
        } else {
            static uint32_t placed = 0;
            if (placed != g_bigship.id) placed = g_bigship.id, put(hull, home, home_yaw), trace("extras: Big ship lies at anchor (%.0f %.0f)", home.x, home.z);
            if (static uint32_t told = 0; told != g_bigship.id && now - made_at > 3) {  // its children, where they sit (the trace)
                told = g_bigship.id;
                for (uint32_t o = 0x478; o <= 0x498; o += 8)
                    if (const uint32_t k = guest_call(c, GC(0x025DE50C), {ld32(hull + o)})) {
                        const V3 l = to_local(hull, k);
                        trace("extras: Big ship child +%#x proc %#x at %.0f %.0f %.0f", o, ld16(k + 8), l.x, l.y, l.z);
                    }
            }
            // out of the boat beside it: the get-off jump (SHIP_GET_OFF, its jump phase link +0x69C0 1), with its deck within
            // 1600 on either side, is made a leap onto the nearest spot of its main deck (x +-250), then he falls the last
            // 30 onto it
            static Leap leap;
            if (leap.step < 0 && proc == 0x90 && ld32(link + 0x69C0) == 1) {
                const V3 l = to_local(hull, link);
                const V3 t{std::clamp(l.x, -250.0, 250.0), kDeckY + 30, std::clamp(l.z, -1500.0, 1800.0)};
                if (std::hypot(t.x - l.x, t.z - l.z) < 1600) {
                    leap.start(link, hull, t);
                    trace("extras: Link leaps onto the Big ship's deck (%.0f off)", std::hypot(t.x - l.x, t.z - l.z));
                }
            }
            leap.carry(link);
            // back into the boat from the deck: A at the gunwale by him, standing or walking (procShipReady_init
            // 023ECA94, the game's own boarding: a hop onto his ledge)
            if (deck && (proc == 0x4 || proc == 0x6) && (pressed & input::kA) && std::hypot(ldf32(link + kPos) - ldf32(korl + kPos), ldf32(link + kPos + 8) - ldf32(korl + kPos + 8)) < 700) {
                guest_call(c, GC(0x023ECA94), {link});
                trace("extras: Link gets back in the boat from the deck");
            }
            // out of the water onto the deck: A while swimming by its side puts Link just inside the gunwale, falling
            // (procFall_init 023F6564 (1, 0): no speed): every copy of his position (put_link) and the fall's start (+0x6A48/C)
            if (swimming && (pressed & input::kA)) {
                const V3 l = to_local(hull, link);
                if (std::fabs(l.x) > 380 && std::fabs(l.x) < 1000 && l.z > -1100 && l.z < 950) {
                    const V3 w = to_world(pos_of(hull), (int16_t)ld16(hull + kAngleY), {l.x > 0 ? 300.0 : -300.0, kDeckY + 40, l.z});
                    put_link(link, w);
                    st32(link + 0x340, 0), st32(link + 0x6A48, u32_bits((float)w.y)), st32(link + 0x6A4C, u32_bits((float)w.y));
                    c->f[1].ps0 = 0;
                    guest_call(c, GC(0x023F6564), {link, 1});
                    trace("extras: Link climbs out of the water onto the deck");
                }
            }
            // (test aid: live pad bit 27 drops Link onto her deck at WWHD_TEST_DECK="x z" (or x,z), her frame, default the wheel)
            static bool drop_was = false;
            if (const bool drop = input::live_buttons() & 0x08000000u; drop && !drop_was && leap.step < 0) {
                double x = 0, z = -780;
                if (const char* e = getenv("WWHD_TEST_DECK")) sscanf(e, "%lf%*[ ,]%lf", &x, &z);
                const V3 w = to_world(pos_of(hull), (int16_t)ld16(hull + kAngleY), {x, kDeckY + 400, z}  /* from above whichever deck is there */);
                put_link(link, w);
                st32(link + 0x340, 0), st32(link + 0x6A48, u32_bits((float)w.y)), st32(link + 0x6A4C, u32_bits((float)w.y));
                c->f[1].ps0 = 0;
                guest_call(c, GC(0x023F6564), {link, 1});
                trace("extras: (test) Link dropped onto the Big ship's deck at %.0f %.0f", x, z);
            }
            drop_was = input::live_buttons() & 0x08000000u;
            // a guest joining the host on her deck: set down beside where the host stands, in her frame
            if (float l[3]; mods::coop::sync::take_deck_drop(l)) {
                const V3 w = to_world(pos_of(hull), (int16_t)ld16(hull + kAngleY), {l[0], l[1] + 60, l[2]});
                put_link(link, w);
                st32(link + 0x340, 0), st32(link + 0x6A48, u32_bits((float)w.y)), st32(link + 0x6A4C, u32_bits((float)w.y));
                c->f[1].ps0 = 0;
                guest_call(c, GC(0x023F6564), {link, 1});
                trace("extras: co-op: Link set down on the host's deck at %.0f %.0f", l[0], l[2]);
            }
            // The helm, the ship's wheel (Kaji, at z -858) on the stern castle: A by it takes it (the stick, A and B are
            // then the helm's, filter_pad). The stick up raises the sail, down lowers it (0.7 s from furled to full);
            // the wind sets how fast she sails (below), and she turns quicker the faster she goes. The hull is
            // moved here, Link standing on it carried by the game's moving-BG rule, the King of Red Lions brought along
            // beside it (eased to his place on its flank, its heading), and the camera is the boat's own
            // (dCamera_c::SetTypeForce 02514EE4 by name, every step, on dCam_getBody 024F8044; WWHD_HELM_CAM=<camera
            // type> tries another). B lets go: the ship slows to a stop. Land under its outline stops it dead (it runs
            // aground, the sail down).
            static bool helm = false;
            static double speed = 0, sail = 0;
            static int helm_land = 0;
            static uint32_t sailed = 0;  // the ship its speed is for
            if (sailed != g_bigship.id) sailed = g_bigship.id, helm = false, g_at_helm = false, speed = sail = 0;
            constexpr V3 kHelm{0, 0, -880};  // the wheel, from either side of it
            const V3 me = to_local(hull, link);
            if (!follow && !helm && deck && g_manned < 0 && (proc == 0x4 || proc == 0x6) && (pressed & input::kA) && std::hypot(me.x - kHelm.x, me.z - kHelm.z) < 200) {
                helm = true, g_take_pad = true, g_at_helm = true;
                trace("extras: Link takes the Big ship's helm");
            } else if (helm && (!deck || !g_take_pad || (pressed & input::kB) || ld8(kPlay + 0x5292))) {
                helm = false, g_take_pad = false, g_at_helm = false, sail = 0;
                trace("extras: Link lets go of the helm");
            }
            if (follow) {
                put(hull, hp, hy);
            } else if (helm || speed > 0.2) {
                if (helm) sail = std::clamp(sail + g_pad_ly.load() / 20.0, 0.0, 1.0);
                if (static double t = 0; now - t >= 1) t = now, trace("extras: Big ship sails: helm %d, sail %.2f, speed %.1f, heading %d", helm, sail, speed, (int16_t)ld16(hull + kAngleY));
                // a heavy ship: it gathers way slowly and carries it, its top speed the boat's 40 (with the sailing
                // upgrade's share) running before the wind, a third of that into it, slack in a calm
                // (dKyw_get_wind_vec 0257DAA8, its power dKyw_get_wind_pow 02578348); the wheel turns the rudder over,
                // and the rudder turns her, slowly from a standstill
                int16_t a = (int16_t)ld16(hull + kAngleY);
                const uint32_t wv = guest_call(c, GC(0x0257DAA8));
                guest_call(c, GC(0x02578348));
                const double power = c->f[1].ps0;
                const double along = in_mem2(wv) ? sin(a * M_PI / 32768) * ldf32(wv) + cos(a * M_PI / 32768) * ldf32(wv + 8) : 0;
                const double top = 40 * (1 + kSailBonus[upgrade(kUpSail)]) * (0.35 + 0.65 * (along + 1) / 2) * std::clamp(power / 0.5, 0.1, 1.0);
                speed += (sail * top - speed) * (sail * top > speed ? 0.02 : 0.012);
                static double rudder = 0;
                rudder += ((helm ? g_pad_lx.load() : 0) - rudder) * 0.08;
                a = (int16_t)(a - rudder * 0xA0 * std::min(1.0, 0.25 + speed / 30));
                if (static double t = 0; now - t >= 1) t = now, trace("extras: Big ship wind %.2f power %.2f top %.1f rudder %.2f", along, power, top, rudder);
                if (helm) {
                    static const uint32_t cam_name = [] {
                        const uint32_t b = mem::host_alloc(0x20);
                        const char* e = getenv("WWHD_HELM_CAM");
                        snprintf((char*)mem::ptr(b), 0x20, "%s", e ? e : "Boat");
                        return b;
                    }();
                    if (const uint32_t cam = guest_call(c, GC(0x024F8044)); in_mem2(cam)) guest_call(c, GC(0x02514EE4), {cam, cam_name, 0});
                }
                V3 p = pos_of(hull);
                p.x += sin(a * M_PI / 32768) * speed, p.z += cos(a * M_PI / 32768) * speed;
                if (++helm_land % 10 == 0 && land(p, a)) {
                    speed = 0, sail = 0;
                    trace("extras: the Big ship runs aground");
                } else {
                    put(hull, p, a);
                    if (helm) st16(link + 0x322, (uint16_t)a), st16(link + kAngleY, (uint16_t)a);
                    if (!riding) {  // he comes along beside it, eased into his place there
                        const V3 w = to_world(p, a, {kLionX, 0, kLionZ}), at = pos_of(korl);
                        const double k = std::hypot(w.x - at.x, w.z - at.z) > 1500 ? 0.1 : 1.0;
                        st32(korl + kPos, u32_bits((float)(at.x + (w.x - at.x) * k))), st32(korl + kPos + 8, u32_bits((float)(at.z + (w.z - at.z) * k)));
                        st16(korl + 0x322, (uint16_t)a), st16(korl + kAngleY, (uint16_t)a), st32(korl + 0x370, 0);
                    }
                }
            }
            // its sail is shown set or furled: daSail's l_HIO byte (1046D78E, 1 eases it full, 0 furls it) is read every
            // step by every sail; set while it sails at more than a crawl, with the ship's sailing sound (its m2CE, +0x3E6)
            static bool sail_set = false;
            const double going = helm || speed > 0.2 ? speed : 0;
            if (going > 12) sail_set = true;
            else if (going < 6) sail_set = false;
            if (follow) sail_set = hsail;
            if (mods::coop::sync::host()) {  // out to the guests: her place, heading and sail
                std::string b(15, '\0');
                const float f[3] = {(float)ldf32(hull + kPos), (float)ldf32(hull + kPos + 4), (float)ldf32(hull + kPos + 8)};
                const int16_t a = (int16_t)ld16(hull + kAngleY);
                memcpy(&b[0], f, 12), memcpy(&b[12], &a, 2), b[14] = sail_set;
                mods::coop::net::put_state(mods::coop::sync::kShip, b);
            }
            st8(GD(0x1046D78E), sail_set), st8(hull + 0x3E6, sail_set);
        }
    }
    // Beedle's ship in tow: made behind the King of Red Lions at sea, it follows him on its tow-shaft (put there each
    // step by its own execute, hook_02363164). Its deck, a moving BG, is registered as his (cBgS::Release 020087EC,
    // Regist 020086D4 on dComIfG_Bgsp, 025200D4() + 0x12A0, with his process id): his own ground and wall checks pass
    // through it as through himself (backing into it), Link's do not. Stopped, B (SHIP_GET_OFF) leaps Link back onto its
    // deck; A at its bow, by none of its guns and within 700 of him, leaps him forward into the boat
    // (procShipReady_init 023ECA94 as he lands beside it); A while swimming by its side climbs aboard
    if (const uint32_t korl = ld32(kPlay + 0x5B3C); tow && stage == "sea" && in_mem2(korl) && settled) {
        if (!beedle) {
            if (g_tow_rope.actor(c)) guest_call(c, GC(0x025D57E4), {g_tow_rope.id}), g_tow_rope = Single{};
            if (riding && g_beedle.wanted(c, now)) {
                Spawner none;
                g_beedle.id = spawn(c, none, g_craft->m, korl, 0, false, 0, false);
                g_beedle.next = now + 5;
            }
        } else {
            g_beedle_actor = beedle, g_beedle_lion = korl, g_beedle_aboard = deck;
            // kept in the room the stage is in now (dStage_roomControl_c::mStayNo, 1047E6C8), and its deck's polygons
            // with it (dBgW's room +0xB8, which dBgS::Regist sets from the actor's room once): made on the boat, they
            // had the boat's room, which may be none, and Link standing on the deck took it as his own (a salvage
            // point's switch check then halted the game, d_save.cpp "0 <= zoneNo")
            if (const int8_t stay = (int8_t)ld8(GD(0x1047E6C8)); stay >= 0) {
                st8(beedle + kRoom, (uint8_t)stay);
                if (const uint32_t bgw = ld32(beedle + 0x6E4); in_mem2(bgw)) st16(bgw + 0xB8, (uint16_t)stay);
            }
            static uint32_t owned = 0;  // the ship whose BG is his
            if (const uint32_t bgw = ld32(beedle + 0x6E4); owned != g_beedle.id && in_mem2(bgw)) {
                owned = g_beedle.id, g_beedle_mtx = 0;
                const uint32_t bgs = guest_call(c, GC(0x025200D4)) + 0x12A0;
                guest_call(c, GC(0x020087EC), {bgs, bgw});
                const uint32_t failed = guest_call(c, GC(0x020086D4), {bgs, bgw, ld32(korl + 4), beedle});
                trace("extras: %s in tow behind the boat (its deck his%s)", g_craft == &kRaftCraft ? "the raft" : "Beedle's ship", failed & 0xFF ? ": regist failed" : "");
            }
            // the tow-shaft drawn: the game's rope (daHimo3, RopeR, proc 0x1BF; param 0xFF006401: type 1, its claw at the
            // far end, 100 segments of 20, no path) in its mode 2 (byte +0x3DA), straight from where it is to a point
            // (+0x2624), here from his stern to the ship's bow, kept in his room; the segments it uses (+0x1828) as many as
            // that takes, so none hang past the bow (it lays them out in its next execute)
            if (const uint32_t r = g_tow_rope.actor(c)) {
                const V3 a = to_world(pos_of(korl), (int16_t)ld16(korl + kAngleY), {0, 60, -150});
                const V3 b = to_world(pos_of(beedle), (int16_t)ld16(beedle + kAngleY), {0, g_craft->top + 40, g_craft->bow});
                st32(r + kPos, u32_bits((float)a.x)), st32(r + kPos + 4, u32_bits((float)a.y)), st32(r + kPos + 8, u32_bits((float)a.z));
                st32(r + 0x2624, u32_bits((float)b.x)), st32(r + 0x2628, u32_bits((float)b.y)), st32(r + 0x262C, u32_bits((float)b.z));
                st8(r + kRoom, ld8(link + kRoom)), st8(r + 0x3DA, 2);
                st32(r + 0x1828, (uint32_t)std::clamp((int)(std::hypot(b.x - a.x, b.y - a.y, b.z - a.z) / 20) + 2, 2, 100));
            } else if (g_tow_rope.wanted(c, now)) {
                Spawner none;
                g_tow_rope.id = spawn(c, none, Monster{0x1BF, -1, 0xFF006401}, korl, 0, false, 60, false);
                g_tow_rope.next = now + 5;
            }
            static Leap leap;
            static bool to_boat = false;  // the leap is into the boat
            // out of the boat: the get-off jump (its jump phase, link +0x69C0 1) a leap back onto its deck, before the hut
            const double isle = std::hypot(ldf32(link + kPos) - std::round(ldf32(link + kPos) / 1e5) * 1e5,
                                           ldf32(link + kPos + 8) - std::round(ldf32(link + kPos + 8) / 1e5) * 1e5);
            if (leap.step < 0 && proc == 0x90 && ld32(link + 0x69C0) == 1 && isle >= 6000) {  // (by an island, ashore)
                leap.start(link, beedle, {0, g_craft->top + 30, g_craft->onto_z}), to_boat = false;
                trace("extras: Link leaps back onto the craft in tow");
            }
            // into the boat: A at its front (toward him), by none of its guns
            if (leap.step < 0 && deck && g_manned < 0 && (proc == 0x4 || proc == 0x6) && (pressed & input::kA) &&
                gun_by(beedle, g_craft->guns, link) < 0 && to_local(beedle, link).z > g_craft->front) {
                leap.start(link, korl, {0, 60, -120}), to_boat = true;
                trace("extras: Link leaps forward into the boat");
            }
            if (leap.carry(link) && to_boat) guest_call(c, GC(0x023ECA94), {link});
            // out of the water: A while swimming by its side puts him just inside it, falling onto its deck
            // (procFall_init 023F6564 (1, 0)): position, old position (+0x300) and the fall's start (+0x6A48/C)
            if (swimming && (pressed & input::kA)) {
                const V3 l = to_local(beedle, link);
                const Craft& k = *g_craft;
                if (std::fabs(l.x) < k.half_x + 300 && std::fabs(l.z) < k.half_z + 300) {
                    const V3 w = to_world(pos_of(beedle), (int16_t)ld16(beedle + kAngleY),
                                          {std::clamp(l.x, -k.climb_x, k.climb_x), k.top + 60, std::clamp(l.z, k.climb_z0, k.climb_z1)});
                    put_link(link, w);
                    st32(link + 0x340, 0), st32(link + 0x6A48, u32_bits((float)w.y)), st32(link + 0x6A4C, u32_bits((float)w.y));
                    c->f[1].ps0 = 0;
                    guest_call(c, GC(0x023F6564), {link, 1});
                    trace("extras: Link climbs out of the water onto the craft in tow");
                }
            }
            if (static double t = 0; now - t >= 1) {  // where Link is about it, once a second (the trace)
                t = now;
                const V3 l = to_local(beedle, link);
                trace("extras: in tow: Link proc %#x deck %d swim %d at %.0f %.0f %.0f, cannon %d", proc, deck, swimming, l.x, l.y, l.z, g_manned);
            }
        }
    } else if (const uint32_t a = g_beedle.actor(c); a && !tow) {
        guest_call(c, GC(0x025D57E4), {g_beedle.id});  // switched off: it goes, and its tow rope
        if (g_tow_rope.actor(c)) guest_call(c, GC(0x025D57E4), {g_tow_rope.id}), g_tow_rope = Single{};
    }
    // the Big ship's helm let go if its block no longer runs (a craft in tow switched on, the Big ship off, mid-helm)
    if (g_at_helm && (tow || !on<kBigShip>())) g_at_helm = false, g_take_pad = false;
    // the guns of the ship out (not the Big ship deleted above this step: its cannons would be made again on it)
    {
        const uint32_t ship = beedle ? beedle : g_bigship.id == 0xFFFFFFFF ? 0 : hull;
        const Gun* guns = beedle ? g_craft->guns : kBigShipGuns;
        deck_cannons(c, sv, link, ship, guns, beedle ? g_craft->top : kDeckY, deck, proc, pressed, now);
        input::set_second_player(on<kGunner>());
        if (on<kGunner>() && settled) second_gunner(c, sv, link, ship, guns, now);
        else g_p2_gun = -1;
        // the ship's crew: a test aid now (WWHD_TEST_CREW=1), so a test clears the sea quickly; in play a second player
        // mans the other gun (Second gunner)
        static const bool crewed = getenv("WWHD_TEST_CREW") != nullptr;
        if (ship && crewed && settled && !mods::coop::sync::guest()) crew(c, link, ship, guns, now);  // (a guest sees the host's crew fire)
        else g_crew_aims[0] = g_crew_aims[1] = false;
    }
    if (static double t = 0; hull && now - t >= 1) {  // where Link is about the Big ship, once a second (the trace)
        t = now;
        const V3 l = to_local(hull, link);
        trace("extras: Big ship: Link proc %#x deck %d swim %d at %.0f %.0f %.0f, cannon %d, bombs %u", proc, deck, swimming, l.x, l.y, l.z, g_manned, ld8(sv + 0x6A));
    }
    // The ship upgrade shop: as a talk with Zunari (daNpc_Rsh1_c, proc 0x160, Windfall; his mode +0xAC4 is 2 while he
    // talks) ends, the panel opens, unless he is about to hand something over (+0xAC3 4: the sail's demo) or Link has no
    // sail yet. A buy: the rupees (sv +4) taken, the level set in the save data, the rupee sound
    {
        static const uint32_t out = mem::host_alloc(4);
        static uint8_t talk_was = 0;
        st32(out, 0);
        guest_call(c, GC(0x025D5578), {0x160, out});
        const uint32_t z = ld32(out);
        const uint8_t talk = in_mem2(z) ? ld8(z + 0xAC4) : 0;
        bool has_sail = false;
        for (int i = 0; i < 21; i++) has_sail |= ld8(sv + 0x3C + i) == 0x78;
        // (test aid: WWHD_TEST_SHOP=1 opens it 5 s after a file loads, and again on F8)
        static const bool test_shop = getenv("WWHD_TEST_SHOP") != nullptr;
        static double test_at = -1;
        static bool f8_was = false;
        bool keys[256];
        input::held_keys(keys);
        if (fresh && test_shop) test_at = now + 5;
        // (test aid: WWHD_TEST_UPGRADES=<hex byte> sets the levels as a file loads, two bits each: sailing, guns, shots)
        if (const char* e = getenv("WWHD_TEST_UPGRADES"); e && fresh) st8(upgrade_byte(), (uint8_t)strtoul(e, nullptr, 16));
        const bool test_now = (test_at > 0 && now >= test_at) || (test_shop && keys[kVK_F8] && !f8_was);
        f8_was = keys[kVK_F8];
        if (test_now) test_at = -1;
        if (on<kUpgrades>() && ((talk_was == 2 && talk != 2 && ld8(z + 0xAC3) != 4 && has_sail) || test_now)) {
            g_shop_news = "", g_shop_sel = 0, g_buy = -1;
            overlay::show_panel(shop_draw);
            trace("extras: Zunari's ship fittings open (%u rupees, upgrades %#x)", ld16(sv + 4), ld8(upgrade_byte()));
        }
        talk_was = talk;
        for (int i = 0; i < 3; i++) g_shop_level[i] = (ld8(upgrade_byte()) >> (2 * i)) & 3;
        g_shop_rupees = wallet();
        if (const int b = g_buy.exchange(-1); b >= 0) {
            const int lv = g_shop_level[b];
            if (lv >= 3) g_shop_news = "That one is as good as it gets.";
            else if (wallet() < kUpPrice[lv]) g_shop_news = "Not enough rupees.";
            else {
                pay(kUpPrice[lv]);
                st8(upgrade_byte(), (uint8_t)((ld8(upgrade_byte()) & ~(3 << (2 * b))) | ((lv + 1) << (2 * b))));
                guest_call(c, GC(0x025E1988), {0x836});
                g_shop_news = "Fitted! Save your game to keep it.";
                trace("extras: bought %s level %d for %d rupees", kUpName[b], lv + 1, kUpPrice[lv]);
            }
        }
    }
    // Tetra's cabin: her deck's door is the game's own exit into a cabin stage, whose way out goes back to wherever
    // that stage's own exits lead. Going in from her deck (the next stage asked for, another stage), where she lies and
    // where the boat is are kept; coming out (the next stage asked for again), Link is sent back instead, by the boat
    // beside her (the turn restart, in it), and she is made again where she lay. In the cabin our panel opens as he
    // arrives (B closes it; F10 opens it again). Its bomb store: a u16 in the reserve block (sv +0x770), kept with the
    // Quest Log
    {
        struct Cabin { std::string stage; int room; V3 boat, ship; int16_t ship_yaw; } static cabin{};
        static double arrived = -1;
        static bool f10_was = false, souped = false;
        bool keys[256];
        input::held_keys(keys);
        const bool going = ld8(kPlay + 0x514C);
        std::string next((const char*)mem::ptr(kPlay + 0x5140), strnlen((const char*)mem::ptr(kPlay + 0x5140), 8));
        if (const uint32_t korl = ld32(kPlay + 0x5B3C); going && stage == "sea" && hull && deck && next != "sea" && cabin.stage.empty() && in_mem2(korl)) {
            cabin = {next, sea_room((float)ldf32(hull + kPos), (float)ldf32(hull + kPos + 8)), pos_of(korl), pos_of(hull), (int16_t)ld16(hull + kAngleY)};
            trace("extras: Link goes below into %s (room %d)", next.c_str(), cabin.room);
        }
        if (fresh && stage != cabin.stage) cabin.stage.clear();  // (a file or state loaded, gone elsewhere)
        if (!cabin.stage.empty() && stage == cabin.stage) {
            if (fresh) arrived = now + 2, souped = false;
            if ((arrived > 0 && now >= arrived) || (keys[kVK_F10] && !f10_was && !overlay::captures()))
                arrived = -1, g_cabin_sel = 0, g_cabin_do = -1, g_shop_news = "", overlay::show_panel(cabin_draw);
            if (going) {  // out of the cabin: back to her, in the boat beside her
                const float boat[3] = {(float)cabin.boat.x, (float)cabin.boat.y, (float)cabin.boat.z};
                ss::redirect(c, "sea", cabin.room, boat, cabin.ship_yaw, boat, true);
                g_cabin_return = {cabin.ship, cabin.ship_yaw, true};
                trace("extras: Link comes up from the cabin to the boat beside her");
                cabin.stage.clear();
            }
            g_cabin_bombs = ld16(sv + 0x770);
            const uint16_t bombs = ld8(sv + 0x6A), bag = ld8(sv + 0x70), store = ld16(sv + 0x770);
            switch (const int act = g_cabin_do.exchange(-1)) {
            case 0: case 1:  // the clock to 6:00 or 19:00 (15 an hour; dKy_instant_timechg 0256076C, a day on if earlier)
                c->f[1].ps0 = act == 0 ? 90.0 : 285.0;
                guest_call(c, GC(0x0256076C));
                g_shop_news = act == 0 ? "You sleep until morning." : "You sleep until nightfall.";
                trace("extras: Link sleeps in the cabin (%s)", act == 0 ? "morning" : "nightfall");
                break;
            case 2: {  // every empty bottle (0x50, items 14..17) Elixir Soup (0x55)
                int filled = 0;  // (through the game's own bottle fill, setEmptyBottleItemIn 025B5448, which minds the item buttons)
                for (int i = 14; i < 18 && !souped; i++)
                    if (ld8(sv + 0x3C + i) == 0x50) guest_call(c, GC(0x025B5448), {sv + 0x3C, 0x55}), filled++;
                if (filled) guest_call(c, GC(0x025E1988), {0x827});
                g_shop_news = souped ? "The pot is empty until your next visit." : filled ? "Your bottles are full of hot soup." : "You have no empty bottle.";
                souped = souped || filled;
                trace("extras: cabin soup: %d bottles filled", filled);
                break;
            }
            case 3:  // bombs from the bag into the store
                st16(sv + 0x770, (uint16_t)std::min(9999, store + bombs)), st8(sv + 0x6A, 0);
                g_shop_news = bombs ? "Bombs stowed." : "You have no bombs to stow.";
                break;
            case 4:  // and back, as many as the bag holds
                if (const uint16_t take = std::min<uint16_t>(store, bag > bombs ? bag - bombs : 0); take) {
                    st16(sv + 0x770, (uint16_t)(store - take)), st8(sv + 0x6A, (uint8_t)(bombs + take));
                    g_shop_news = "Bombs taken.";
                } else {
                    g_shop_news = !bag ? "You have no Bomb Bag." : store ? "Your Bomb Bag is full." : "The store is empty.";
                }
                break;
            default: break;
            }
        }
        f10_was = keys[kVK_F10];
    }
    // Tetra's ship for sale at the Bomb Shop (stage Obombh, the shopkeeper daNpc_Bms1_c, proc 0x15B): the first time
    // in a visit that a message closes with him within 600 of Link (a talk with him), unless she is Link's already
    {
        static bool offered = false;
        static uint8_t mesg_was = 0;
        const uint8_t mesg = ld8(kPlay + 0x5BB2);  // dComIfGp_getMesgStatus
        if (fresh) offered = false;
        if (on<kBigShip>() && stage == "Obombh" && !owns_ship() && !offered && mesg_was && !mesg) {
            static const uint32_t out = mem::host_alloc(4);
            st32(out, 0);
            guest_call(c, GC(0x025D5578), {0x15B, out});
            if (const uint32_t m = ld32(out); in_mem2(m) && std::hypot(ldf32(m + kPos) - ldf32(link + kPos), ldf32(m + kPos + 8) - ldf32(link + kPos + 8)) < 600) {
                offered = true, g_shop_news = "", g_ship_buy = false;
                overlay::show_panel(ship_sale_draw);
                trace("extras: Tetra's ship is offered at the Bomb Shop (%u rupees)", ld16(sv + 4));
            }
        }
        mesg_was = mesg;
        if (g_ship_buy.exchange(false) && !owns_ship()) {
            if (wallet() < kShipPrice) g_shop_news = "Not enough rupees.";
            else {
                pay(kShipPrice), st8(sv + 0x772, ld8(sv + 0x772) | 1);
                guest_call(c, GC(0x025E1988), {0x836});
                g_shop_news = "Sold! She waits for you out at sea. Save your game to keep her.";
                trace("extras: Tetra's ship bought for %d rupees", kShipPrice);
            }
        }
        if (stage == "Obombh") g_shop_rupees = wallet(), g_ship_sold = owns_ship();
    }
    // Warp: F9 (the toggle on) opens the places; the one picked is made the next stage change, Link on his boat (or
    // test aid: WWHD_TEST_WARP=<place, part of its name> once, 5 s after a file loads). Going on to a stall: once
    // the actor is there (in the room just entered, within 20 s) a second, on foot 250 before it, the boat where it is
    {
        static bool f9_was = false;
        static double test_at = -1, hop_until = -1;
        static int16_t hop_to = 0;
        static const char* test_warp = getenv("WWHD_TEST_WARP");
        bool keys[256];
        input::held_keys(keys);
        if (on<kWarp>() && keys[kVK_F9] && !f9_was && !overlay::captures()) g_warp_sel = 0, g_warp_go = -1, overlay::show_panel(warp_draw);
        f9_was = keys[kVK_F9];
        if (fresh && test_warp && test_at == -1) test_at = now + 5;
        // (and a test driver's: live pad bit 29 warps to the first open-sea place)
        static bool live_was = false;
        if (const bool live_now = input::live_buttons() & 0x20000000u; live_now && !live_was)
            for (int i = 0; i < (int)std::size(kPlaces); i++)
                if (strstr(kPlaces[i].name, "Open sea")) { g_warp_go = i; break; }
        live_was = input::live_buttons() & 0x20000000u;
        if (test_at > 0 && now >= test_at) {
            test_at = -2;
            for (int i = 0; i < (int)std::size(kPlaces); i++)
                if (strstr(kPlaces[i].name, test_warp)) { g_warp_go = i; break; }
        }
        if (const int i = g_warp_go.exchange(-1); i >= 0) {
            const Place& p = kPlaces[i];
            const float at[3] = {p.x, 0, p.z};
            std::string why;
            if (ss::warp(c, "sea", sea_room(p.x, p.z), at, (int16_t)0x8000, at, true, why)) {
                hop_to = p.near, hop_until = p.near ? now + 25 : -1;
                trace("extras: warp to %s (room %d)", p.name, sea_room(p.x, p.z));
            } else {
                ss::notice("Warp: " + why);
                trace("extras: no warp to %s: %s", p.name, why.c_str());
            }
        }
        if (hop_until > 0 && now < hop_until && settled && !fresh) {
            static const uint32_t out = mem::host_alloc(4);
            st32(out, 0);
            guest_call(c, GC(0x025D5578), {(uint32_t)(uint16_t)hop_to, out});
            const uint32_t a = ld32(out), korl = ld32(kPlay + 0x5B3C);
            std::string why;
            if (in_mem2(a) && in_mem2(korl)) {
                const double t = (int16_t)ld16(a + kAngleY) * M_PI / 32768;
                const float at[3] = {(float)(ldf32(a + kPos) + sin(t) * 250), (float)ldf32(a + kPos + 4), (float)(ldf32(a + kPos + 8) + cos(t) * 250)};
                const float boat[3] = {(float)ldf32(korl + kPos), (float)ldf32(korl + kPos + 4), (float)ldf32(korl + kPos + 8)};
                if (ss::warp(c, "sea", (int8_t)ld8(a + kRoom), at, (int16_t)(ld16(a + kAngleY) + 0x8000), boat, false, why))
                    hop_until = -1, trace("extras: on to beside it (proc %#x at %.0f %.0f)", hop_to, at[0], at[2]);
            }
        }
    }
    // test aid: WWHD_TEST_SHOT_EVERY=<seconds>, a screenshot that often (a test's flipbook), each marked in the trace
    static const double shot_every = getenv("WWHD_TEST_SHOT_EVERY") ? atof(getenv("WWHD_TEST_SHOT_EVERY")) : 0;
    if (static double next_shot = 0; shot_every > 0 && now >= next_shot) {
        next_shot = now + shot_every;
        screenshot::request();
        trace("extras: frame");
    }
    // test aid: WWHD_TEST_STAGE="<stage> <room> <point>" goes there (the next stage asked for) once, 5 s after a file loads
    if (static const char* e = getenv("WWHD_TEST_STAGE"); e) {
        static double at = -1;
        if (fresh && at == -1) at = now + 5;
        if (at > 0 && now >= at && !ld8(kPlay + 0x514C)) {
            at = -2;
            char name[8] = {};
            int room = 0, point = 0;
            sscanf(e, "%7s %d %d", name, &room, &point);
            for (int i = 0; i < 8; i++) st8(kPlay + 0x5140 + i, (uint8_t)name[i]);
            st16(kPlay + 0x5148, (uint16_t)point), st8(kPlay + 0x514A, (uint8_t)room), st8(kPlay + 0x514B, 0xFF);
            st8(kPlay + 0x514D, 0), st8(kPlay + 0x514C, 1);
            trace("extras: (test) to %s room %d point %d", name, room, point);
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
    coop_world(c, link, settled, riding, now);
}

// camera_draw (024FFC40; the camera process: eye +0xDC, center +0xE8): at a manned deck cannon the camera looks out over
// it along its aim, from 1400 behind and 600 up, as the boat's cannon camera does; at the Big ship's helm it stands far
// back and high, so the whole ship is in view: the eye is put a fixed 3600 (WWHD_HELM_CAM_DIST) behind its look-at
// point the way it already looks, and 40% of that above it (a fixed distance, so it is the same however often it runs)
void camera_draw(uint32_t cam) {
    if (!in_mem2(cam)) return;
    g_cam_yaw = (int16_t)(atan2(ldf32(cam + 0xE8) - ldf32(cam + 0xDC), ldf32(cam + 0xF0) - ldf32(cam + 0xE4)) * 32768 / M_PI);
    // the free camera: F7 lets it go (and takes it back); from where the game's camera was, it flies at 40 units a draw
    // (160 with ZL), turning 2 degrees a draw at full tilt, and pitching at most 85 degrees up or down
    {
        static bool f7 = false;
        static double ex, ey, ez, yaw, pitch;
        bool keys[256];
        input::held_keys(keys);
        if (keys[kVK_F7] && !f7 && (g_free_cam || on<kPhoto>())) {
            g_free_cam = !g_free_cam;
            if (g_free_cam) {
                ex = ldf32(cam + 0xDC), ey = ldf32(cam + 0xE0), ez = ldf32(cam + 0xE4);
                const double dx = ldf32(cam + 0xE8) - ex, dy = ldf32(cam + 0xEC) - ey, dz = ldf32(cam + 0xF0) - ez;
                yaw = atan2(dx, dz), pitch = atan2(dy, std::hypot(dx, dz));
            }
            trace("extras: free camera %s", g_free_cam ? "on" : "off");
        }
        f7 = keys[kVK_F7];
        if (g_free_cam && !on<kPhoto>()) g_free_cam = false;
        if (g_free_cam) {
            const uint32_t b = g_free_buttons.load();
            const double v = (b & input::kZL) ? 160 : 40;
            yaw -= g_free_rx.load() * 0.035, pitch = std::clamp(pitch + g_free_ry.load() * 0.035, -1.48, 1.48);
            const double fx = sin(yaw) * cos(pitch), fy = sin(pitch), fz = cos(yaw) * cos(pitch);
            const double lx = g_free_lx.load(), ly = g_free_ly.load();
            ex += (fx * ly - cos(yaw) * lx) * v, ez += (fz * ly + sin(yaw) * lx) * v;
            ey += fy * ly * v + ((b & input::kR) ? v : 0) - ((b & input::kL) ? v : 0);
            st32(cam + 0xDC, u32_bits((float)ex)), st32(cam + 0xE0, u32_bits((float)ey)), st32(cam + 0xE4, u32_bits((float)ez));
            st32(cam + 0xE8, u32_bits((float)(ex + fx * 1000))), st32(cam + 0xEC, u32_bits((float)(ey + fy * 1000))), st32(cam + 0xF0, u32_bits((float)(ez + fz * 1000)));
            return;
        }
    }
    if (g_cannon_cam) {  // a manned deck cannon: as on the boat, from behind it out along its aim, far back
        const double t = g_cannon_yaw.load() * M_PI / 32768, e = g_cannon_up.load() * M_PI / 32768;
        const float x = g_cannon_x, y = g_cannon_y, z = g_cannon_z, fx = (float)sin(t), fz = (float)cos(t);
        st32(cam + 0xDC, u32_bits(x - fx * 1400)), st32(cam + 0xE0, u32_bits(y + 600)), st32(cam + 0xE4, u32_bits(z - fz * 1400));
        st32(cam + 0xE8, u32_bits(x + fx * 2500)), st32(cam + 0xEC, u32_bits(y + (float)(2500 * tan(e)))), st32(cam + 0xF0, u32_bits(z + fz * 2500));
        return;
    }
    if (!g_at_helm) return;
    static const float dist = [] { const char* e = getenv("WWHD_HELM_CAM_DIST"); return e ? (float)atof(e) : 3600.f; }();
    const float dx = (float)ldf32(cam + 0xDC) - (float)ldf32(cam + 0xE8), dz = (float)ldf32(cam + 0xE4) - (float)ldf32(cam + 0xF0);
    const float h = std::hypot(dx, dz);
    if (h < 1) return;
    st32(cam + 0xDC, u32_bits((float)ldf32(cam + 0xE8) + dx / h * dist));
    st32(cam + 0xE0, u32_bits((float)ldf32(cam + 0xEC) + dist * 0.4f));
    st32(cam + 0xE4, u32_bits((float)ldf32(cam + 0xF0) + dz / h * dist));
}

// each GamePad read (any thread)
void filter_pad(input::PadState& s) {
    if (overlay::blocks_input()) return;      // the settings overlay has the pad
    if (s.buttons & input::kPlus) g_take_pad = false;  // the pause menu: the cannon lets go (deck_cannons sees it)
    g_pad_hold = s.buttons;
    g_pad_seen |= s.buttons;
    g_pad_lx = s.lx, g_pad_ly = s.ly;
    if (g_free_cam) {  // the free camera has the pad; Link stands still
        g_free_lx = s.lx, g_free_ly = s.ly, g_free_rx = s.rx, g_free_ry = s.ry, g_free_buttons = s.buttons;
        s.lx = s.ly = s.rx = s.ry = 0, s.buttons &= input::kPlus;
        return;
    }
    // a cannon manned or the helm taken: the stick, A, ZL (the cannon's lock-on), the Bomb Bag's button and B are theirs
    if (g_take_pad) s.lx = s.ly = 0, s.buttons &= ~(g_cannon_btn.load() | input::kA | input::kB | input::kZL);
    else s.buttons &= ~g_cannon_btn.load();
}
// Ship upgrades in Beedle's own shops (daNpc_Bs1_c, proc 0x159): a slot of his stock made one of ours as his shop
// lays out its shelf (createShopList 02212BA0, the shop's index at +0x99F; its stock arrays are pointers to 20-byte
// records {ItemData*, show msg, buy msg, sold-out msg, count}, ItemData {item, must not own, must own, checks}). His
// ships on sea squares 26 and 38 (shop 5): the Red Potion's slot sells Faster sailing; on squares 10, 12, 27, 32 and 44
// (shop 3): the slot that becomes 30 Bombs once Link has the Bomb Bag sells the next cannon fitting (Faster guns or
// Bigger shots, the lower first). Each is shown with a real item's model (the Sail, 30 Bombs) and speaks with messages
// only it uses (the Red Potion's, and the 10 Arrows' of shop 4, which no ship on the sea opens): their UTF-16 texts,
// in the loaded message file, are written over in place as the game fetches them (getMessage 025F50E8; its out +0x118
// the text's index, +8 the file, 0273A2E8 the text) and their price (the buy message's attribute record, out +0x22C,
// s16 +4); while the slot is not ours on the shelf, the originals are written back. A buy: Bs1 pays the price as for
// any item; checkItemGet's answer (02215474) sends ours down the plain execItemGet path, where (02215484) the level is
// fitted instead and the item given is one that does nothing (0x13). Bought at the top level already: paid back
struct Fitting {
    uint32_t data = 0, rec = 0;  // our ItemData and record (guest memory)
    uint16_t show, buy;          // its messages
    uint8_t item;                // the model shown
};
Fitting g_fit_sail{0, 0, 0xF6D, 0xF6E, 0x78}, g_fit_guns{0, 0, 0xF69, 0xF6A, 0x0E};
void make_fitting(Fitting& f) {
    if (f.rec) return;
    f.data = mem::host_alloc(16), f.rec = mem::host_alloc(20);
    st32(f.data, f.item), st32(f.data + 4, f.item), st32(f.data + 8, 0), st32(f.data + 12, 0x20000000);  // (checks: rupees only)
    st32(f.rec, f.data), st32(f.rec + 4, f.show), st32(f.rec + 8, f.buy), st32(f.rec + 12, 0xF65), st32(f.rec + 16, 0xFFFFFFFF);
}
// the fitting a cannon slot sells: the lower of the guns and the shots (the guns first); -1 both at the top
int cannon_fitting() {
    const int g = level(kUpReload), s = level(kUpShot);
    return g >= 3 && s >= 3 ? -1 : g <= s ? kUpReload : kUpShot;
}
int fitting_of(uint32_t rec) {  // (-1 at the top: bought up while on the shelf)
    return rec && rec == g_fit_sail.rec ? (level(kUpSail) < 3 ? kUpSail : -1) : rec && rec == g_fit_guns.rec ? cannon_fitting() : -1;
}
// the Bs1 on the shelf of which a record of ours lies (the selected slot's, or any)
bool shelved(uint32_t bs1, uint32_t rec) {
    for (uint32_t i = 0; i < 3; i++) if (ld32(bs1 + 0x988 + 4 * i) == rec) return true;
    return false;
}
uint32_t beedle_shop(Cpu* c) {
    static const uint32_t out = mem::host_alloc(4);
    st32(out, 0);
    guest_call(c, GC(0x025D5578), {0x159, out});
    return in_mem2(ld32(out)) ? ld32(out) : 0;
}
// our texts: the show message "<red>Name  NN Rupees\n<end><reset>words<end>", the buy "Name  NN Rupees\nWill you buy
// it?\n<choice>I'll buy it\nNo thanks", in no more room than the original's
std::vector<uint16_t> utf16(const std::string& t) { return std::vector<uint16_t>(t.begin(), t.end()); }
std::vector<uint16_t> fitting_text(int up, bool buy) {
    static const char* const kName[] = {"Sail fitting", "Gun fitting", "Shot fitting"};
    static const char* const kWords[] = {"Wind in the sails, fine customer! Faster\nsailing for your ship, fitted as you sail.",
                                          "Your ship's guns reload faster with this!\nPirates fear a quick gunner. Buy it!",
                                          "Bigger shots for your ship's guns: they\nhit harder and burst wider. Buy it!"};
    const int lv = level(up);
    const std::string head = std::string(kName[up]) + " " + std::to_string(lv + 1) + "  " + std::to_string(kUpPrice[lv]) + " Rupees";
    std::vector<uint16_t> v;
    auto add = [&](std::initializer_list<uint16_t> u) { v.insert(v.end(), u); };
    auto text = [&](const std::string& t) { const auto u = utf16(t); v.insert(v.end(), u.begin(), u.end()); };
    if (buy) {
        text(head + "\nWill you buy it?\n");
        add({0x000E, 0x0001, 0x0007, 0x0000});
        text("I'll buy it\nNo thanks");
    } else {
        add({0x000E, 0x0000, 0x0003, 0x0002, 0x0001}), text(head + "\n"), add({0x000E, 0x0000, 0x0003, 0x0002, 0xFFFF});
        add({0x000E, 0x0000, 0x0003, 0x0002, 0x0000}), text(kWords[up]), add({0x000E, 0x0000, 0x0003, 0x0002, 0xFFFF});
    }
    v.push_back(0);
    return v;
}
// a message's length in UTF-16 words, its end included: its tags (0x000E, group, type, a size in bytes, the size's
// params) may hold zeros
size_t text_len(uint32_t at) {
    size_t n = 0;
    for (uint16_t u; (u = ld16(at + 2 * n)) != 0 && n < 4096;) n += u == 0x000E ? 4 + ld16(at + 2 * n + 6) / 2 : 1;
    return n + 1;
}
// a fetched message (`id` its number): ours written in, in no more room than the game's own (whose words are kept, in
// whatever language, to write back while the slot is not ours), and the buy message's price set
void shop_message(Cpu* c, uint32_t out, int id) {
    struct Text { uint16_t id; Fitting* f; bool buy; uint32_t at; std::vector<uint16_t> orig; };
    static Text texts[] = {{0xF6D, &g_fit_sail, false, 0, {}}, {0xF6E, &g_fit_sail, true, 0, {}},
                           {0xF69, &g_fit_guns, false, 0, {}}, {0xF6A, &g_fit_guns, true, 0, {}}};
    Text* t = nullptr;
    for (Text& x : texts) if (x.id == id) t = &x;
    const uint32_t file = ld32(out + 8);
    if (!t || !in_mem2(file) || !in_mem2(ld32(file))) return;
    const uint32_t at = guest_call(c, GC(0x0273A2E8), {ld32(file), ld32(out + 0x118)});
    if (!in_mem2(at)) return;
    if (t->at != at) {  // first seen (or moved): the game's own words, kept
        t->at = at, t->orig.resize(text_len(at));
        for (size_t i = 0; i < t->orig.size(); i++) t->orig[i] = ld16(at + 2 * i);
    }
    const uint32_t bs1 = beedle_shop(c);
    const int up = bs1 && t->f->rec && shelved(bs1, t->f->rec) ? fitting_of(t->f->rec) : -1;
    std::vector<uint16_t> words = up >= 0 ? fitting_text(up, t->buy) : t->orig;
    if (words.size() > t->orig.size()) words = t->orig;  // (no room in this language: its own words, our price)
    for (size_t i = 0; i < t->orig.size(); i++) st16(at + 2 * i, i < words.size() ? words[i] : 0);
    if (const uint32_t rec = ld32(out + 0x22C); t->buy && in_mem2(rec)) {
        static uint16_t own[2] = {0, 0};  // the buy messages' own prices (F6E, F6A)
        uint16_t& price = own[t->id == 0xF6A];
        if (!price) price = ld16(rec + 4);
        st16(rec + 4, (uint16_t)(up >= 0 ? kUpPrice[level(up)] : price));
    }
}
}  // namespace mods::extras

// Beedle's shelf laid out (createShopList 02212BA0, r3 his shop): a slot of his stock array pointed at our record for
// the call (shop 5 the Red Potion's, +8; shop 3 the 30 Bombs' that stands in once Link has the Bomb Bag, +0x14), and back
extern "C" void f_02212BA0_orig(Cpu* c);
extern "C" void hook_02212BA0(Cpu* c) {
    using namespace mods::extras;
    const uint8_t shop = ld8(c->r[3] + 0x99F);
    uint32_t slot = 0, was = 0;
    if (on<kUpgrades>() && shop == 5 && level(kUpSail) < 3) make_fitting(g_fit_sail), slot = GD(0x101BD56C) + 8, was = ld32(slot), st32(slot, g_fit_sail.rec);
    if (on<kUpgrades>() && shop == 3 && cannon_fitting() >= 0) make_fitting(g_fit_guns), slot = GD(0x101BD548) + 0x14, was = ld32(slot), st32(slot, g_fit_guns.rec);
    f_02212BA0_orig(c);
    if (slot) st32(slot, was), mods::trace("extras: Beedle's shop %u shelves a ship fitting", shop);
}
extern "C" void f_025F50E8_orig(Cpu* c);
extern "C" void hook_025F50E8(Cpu* c) {
    const uint32_t out = c->r[4], label = c->r[6];
    f_025F50E8_orig(c);
    if (!mods::extras::g_fit_sail.rec && !mods::extras::g_fit_guns.rec) return;
    // its label: "%05d" of the message's number, the chars or an object whose first word points at them
    auto number = [](uint32_t a) {
        int n = 0;
        for (int i = 0; i < 5; i++) {
            const uint8_t ch = mods::extras::in_mem2(a) ? ld8(a + i) : 0;
            if (ch < '0' || ch > '9') return -1;
            n = n * 10 + (ch - '0');
        }
        return n;
    };
    int id = number(label);
    if (id < 0 && mods::extras::in_mem2(label)) id = number(ld32(label));
    if (static bool told = false; !told && id >= 0) told = true, mods::trace("extras: getMessage label read (%05d)", id);
    const uint32_t r3 = c->r[3];
    if (id >= 0) mods::extras::shop_message(c, out, id);
    c->r[3] = r3;
}
// Bs1's buy (r27 Beedle): the selected slot's record, ours or not
static int bought_fitting(Cpu* c) {
    const uint32_t bs1 = c->r[27];
    const int idx = (int16_t)ld16(bs1 + 0x940);
    return idx >= 0 && idx < 3 ? mods::extras::fitting_of(ld32(bs1 + 0x988 + 4 * idx)) : -1;
}
extern "C" void site_02215474(Cpu* c) {  // checkItemGet's answer: ours takes the execItemGet path
    if (const uint32_t bs1 = c->r[27]; bs1 && (mods::extras::shelved(bs1, mods::extras::g_fit_sail.rec) || mods::extras::shelved(bs1, mods::extras::g_fit_guns.rec))) {
        const int idx = (int16_t)ld16(bs1 + 0x940);
        const uint32_t rec = idx >= 0 && idx < 3 ? ld32(bs1 + 0x988 + 4 * idx) : 0;
        if (rec && (rec == mods::extras::g_fit_sail.rec || rec == mods::extras::g_fit_guns.rec)) c->r[3] = 1;
    }
}
extern "C" void site_02215484(Cpu* c) {  // execItemGet(r3): ours fitted instead (or, at the top already, paid back)
    using namespace mods::extras;
    const uint32_t bs1 = c->r[27];
    const int idx = (int16_t)ld16(bs1 + 0x940);
    const uint32_t rec = idx >= 0 && idx < 3 ? ld32(bs1 + 0x988 + 4 * idx) : 0;
    if (!rec || (rec != g_fit_sail.rec && rec != g_fit_guns.rec)) return;
    const int up = fitting_of(rec);
    if (up < 0 || level(up) >= 3) {
        st32(kPlay + 0x5B48, ld32(kPlay + 0x5B48) + (uint32_t)(int16_t)ld16(kPlay + 0x5BA4));
        mods::trace("extras: a fitting bought at the top level: paid back");
    } else {
        st8(upgrade_byte(), (uint8_t)((ld8(upgrade_byte()) & ~(3 << (2 * up))) | ((level(up) + 1) << (2 * up))));
        mods::trace("extras: bought %s level %d at Beedle's", kUpName[up], level(up));
    }
    c->r[3] = 0x13;
}

// fopAc_Execute's position check (025D4ABC: r30 the actor, its position out of +-1e32, as the game halts next): which
// actor, where, and whether it is one of ours, into the trace (a NaN position halted a seamless run, 2026-10-12)
extern "C" void site_025D4ABC(Cpu* c) {
    using namespace mods::extras;
    const uint32_t a = c->r[30], id = ld32(a + 4);
    const bool sea = std::find(g_sea.ids.begin(), g_sea.ids.end(), id) != g_sea.ids.end(),
               gyorg = std::find(g_gyorg_ids.begin(), g_gyorg_ids.end(), id) != g_gyorg_ids.end();
    mods::trace("extras: HALT: actor proc %#x id %#x at %g %g %g (old %g %g %g)%s%s%s", ld16(a + 8), id, ldf32(a + kPos), ldf32(a + kPos + 4),
                ldf32(a + kPos + 8), ldf32(a + 0x300), ldf32(a + 0x304), ldf32(a + 0x308), sea ? " [Haunted sea]" : "", gyorg ? " [Gyorg]" : "",
                a == g_beedle_actor ? " [craft in tow]" : "");
}

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

// daGy_c::checkTgHit (r30 the Gyorg), after a hit: a Gyorg has 1 health (s8 +0x3A1), an arrow, the hookshot or the
// Boomerang take 2, a bomb sets it to 0; ice and light arrows (+0xE04 9 or 0xB) delete it, the sword does nothing.
// The Leviathan is kept at 100 and the hits are counted into its own health instead (one per 0.3 s: a bomb's burst
// lasts a few steps); when that is gone it is let die as the game kills a Gyorg
static void leviathan_hit(int damage) {
    using namespace mods::extras;
    static double last = -1;
    if (g_step_time - last < 0.3) return;
    last = g_step_time;
    if ((g_leviathan_pool -= damage) <= 0) g_leviathan_slain = true;
}
// its death test (02168488, lbz r11, 0x3A1(r30)), after the health was taken
extern "C" void site_02168488(Cpu* c) {
    using namespace mods::extras;
    const uint32_t gy = c->r[30];
    if (!g_leviathan_actor || gy != g_leviathan_actor) return;
    if (const int hp = (int8_t)ld8(gy + 0x3A1); hp < 100) leviathan_hit(hp <= 0 ? 4 : 100 - hp);
    st8(gy + 0x3A1, g_leviathan_pool > 0 ? 100 : 0);
}
// the ice / light arrow delete test (021685B0, lbz r12, 0xE04(r30)), also where a sword hit ends up (r3 still the
// hitter's collider: its AT type +0x10, the sword 2, its power u8 +0x14)
extern "C" void site_021685B0(Cpu* c) {
    using namespace mods::extras;
    const uint32_t gy = c->r[30];
    if (!g_leviathan_actor || gy != g_leviathan_actor) return;
    const uint8_t frozen = ld8(gy + 0xE04);
    if (frozen == 9 || frozen == 0xB) leviathan_hit(4);
    else if (c->r[3] >= 0x10000000 && c->r[3] < 0x50000000 && (ld32(c->r[3] + 0x10) & 2)) leviathan_hit(std::max<int>(1, ld8(c->r[3] + 0x14)));
    else return;
    st8(gy + 0xE04, g_leviathan_pool > 0 ? 0 : 9);  // still alive, or deleted as an ice arrow does
}

// Molgera's bwd_move (020FF80C): when its bite catches Link (eat_attack counts m3C1E, s16 +0x3E96, up to 1) the
// swallow would order an event, lock the camera and fling him off the boat: here it is a missed bite instead, back to
// waiting (state 1, substate 0, timers +0x1B50 0x7A and +0x1B54 100), before bwd_demo_camera sees it. In its flight
// (state 10) its Execute draws the head at scale 1 whatever l_HIO says, by the state it reads after bwd_move: it is
// shown state 13 (which bwd_move does nothing in) until the next bwd_move, and its body is drawn (+0x1878, which
// Execute clears before bwd_move and the flight's own steering sets)
extern "C" void f_020FF80C_orig(Cpu* c);
extern "C" void hook_020FF80C(Cpu* c) {
    using namespace mods::extras;
    const uint32_t a = c->r[3];
    const bool ours = a == g_molgera_actor && g_molgera_actor;
    if (ours && ld16(a + 0x1B32) == 13) st16(a + 0x1B32, 10);
    f_020FF80C_orig(c);
    if (!ours) return;
    if (ld16(a + 0x3E96) == 1)
        st16(a + 0x3E96, 0), st16(a + 0x1B32, 1), st16(a + 0x1B34, 0), st16(a + 0x1B50, 0x7A), st16(a + 0x1B54, 100);
    if (ld16(a + 0x1B32) == 10 && ld16(a + 0x1B34) == 1) {
        if (g_molgera_pitch >= 0) st16(a + 0x320, (uint16_t)g_molgera_pitch), g_molgera_pitch = -1;
        if (!g_molgera_dying)  // its circling flight makes 40 a step (speed +0x33C, xz): carried on by its pace
            for (uint32_t o : {0u, 8u}) st32(a + kPos + o, u32_bits((float)(ldf32(a + kPos + o) + ldf32(a + 0x33C + o) * (g_molgera_pace - 1))));
    }
    st32(a + 0x1878, 1);
    if (ld16(a + 0x1B32) == 10) st16(a + 0x1B32, 13);
}

// daFganon_Execute (02134B28; r3 the actor): everything Phantom Ganon makes (his energy and dark balls, their bursts,
// the sparks of a hit on him) is made from inside it, through dPa_control_c::set (025A847C; r6 pos, r7 angle, r8 cXyz*
// scale or 0), which copies the scale into the emitter: while he executes it is his size times what was asked. His
// short-lived copies are other fganon actors, normal size
extern "C" void f_02134B28_orig(Cpu* c);
extern "C" void hook_02134B28(Cpu* c) {
    using namespace mods::extras;
    g_fx_size = g_phantom_actor && c->r[3] == g_phantom_actor ? g_phantom_size : 1;
    CoopStandIn s(c->r[3]);
    f_02134B28_orig(c);
    g_fx_size = 1;
}
// only his one-shot bursts: his trailing smoke (legs, arms) and the light arrows' glow at 12x fill the screen and
// slow the game down. r5 the effect id: the smashed and broken energy ball 81F0 / 81EE-81EF, the exploding dark ball
// 8242-8243, a hit's flash and sparks 0x10 / 0x0D (his energy and dark balls' own emitters are scaled in step())
static bool titanic_effect(uint32_t id) {
    return id == 0x81F0 || id == 0x81EE || id == 0x81EF || id == 0x8242 || id == 0x8243 || id == 0x10 || id == 0x0D;
}
extern "C" void f_025A847C_orig(Cpu* c);
extern "C" void hook_025A847C(Cpu* c) {
    using namespace mods::extras;
    mods::coop::particle(c);
    if (g_fx_size > 1 && titanic_effect(c->r[5] & 0xFFFF)) {
        static const uint32_t buf = mem::host_alloc(0x10);
        const uint32_t s = c->r[8];
        for (int i = 0; i < 3; i++) st32(buf + 4 * i, u32_bits(g_fx_size * (s ? (float)ldf32(s + 4 * i) : 1.f)));
        c->r[8] = buf;
    }
    f_025A847C_orig(c);
}

// Seahats (ph_class Execute 023CC070, r30 the Seahat) swoop only while Link has the ship-ride status (play +0x5CD8
// bit 0x10000: its three tests, just after each is loaded): the Haunted sea's come for him swimming or on the Big ship's
// deck too. A process of ours, by id (base_process_class +4)
static void seahat_rides(Cpu* c, int reg) {
    const auto& ids = mods::extras::g_sea.ids;
    if (std::find(ids.begin(), ids.end(), ld32(c->r[30] + 4)) != ids.end()) c->r[reg] |= 0x10000;
}
extern "C" void site_023CC7F0(Cpu* c) { seahat_rides(c, 5); }
extern "C" void site_023CC984(Cpu* c) { seahat_rides(c, 4); }
extern "C" void site_023CCC88(Cpu* c) { seahat_rides(c, 9); }

// daOship_c's execute (023C68B8; r3 the cannon boat): a deck cannon shot counts as the King of Red Lions' cannon firing
// back, which its plFireRepeat (023C7CA8) reads only from him (+0x644 bit 0x20000): it fires back 10-30 steps later and
// its aim, short of Link by 500 a shot until he has fired back 6 times (modeAttackInit), closes in by one (+0xE88, the
// count; +0xE98, the timer). Without this, from the deck it never hit him (44 shots).
extern "C" void f_023C68B8_orig(Cpu* c);
extern "C" void hook_023C68B8(Cpu* c) {
    using namespace mods::extras;
    static std::unordered_map<uint32_t, uint32_t> answered;  // by boat: the deck shots it has answered
    const uint32_t a = c->r[3];
    auto [it, fresh] = answered.try_emplace(a, g_deck_shots);
    if (!fresh && it->second != g_deck_shots) {
        st32(a + 0xE88, ld32(a + 0xE88) + 1);
        st32(a + 0xE98, 10 + rand() % 20);
        it->second = g_deck_shots;
    }
    CoopStandIn s(a);
    f_023C68B8_orig(c);
}

// daOship_c::modeAttackInit (023C6DB4; r3 the cannon boat): its aim (x +0xE0C, z +0xE14) is put short of Link on
// purpose, 300 + 500 a shot he has not fired back (3000 more while he sails or swims); a third of that miss taken off
extern "C" void f_023C6DB4_orig(Cpu* c);
extern "C" void hook_023C6DB4(Cpu* c) {
    using namespace mods::extras;
    const uint32_t a = c->r[3], link = true60::link();
    f_023C6DB4_orig(c);
    if (!link) return;
    for (uint32_t o : {0xE0Cu, 0xE14u}) {
        const float at = ldf32(link + kPos + (o - 0xE0C));
        st32(a + o, u32_bits(at + (ldf32(a + o) - at) * 2.f / 3));
    }
}

// daObj_Ikada's execute (02363164; r3 the actor): the craft in tow is put before it runs (its execute floats it on the
// water and moves its BG from where it is): its distance behind the King of Red Lions (more behind the giant one) on
// the line from it to him, facing him, at its scale, his speed its own for its wake (it makes one above speedF 2);
// fresh (made on him), straight behind him
extern "C" void f_02363164_orig(Cpu* c);
extern "C" void hook_02363164(Cpu* c) {
    using namespace mods::extras;
    if (const uint32_t a = c->r[3], k = g_beedle_lion; a && a == g_beedle_actor && in_mem2(k)) {
        const V3 lion = pos_of(k), p = pos_of(a);
        const double tow = g_craft->tow;
        double dx = lion.x - p.x, dz = lion.z - p.z, d = std::hypot(dx, dz);
        if (d < 100 || d > 3 * tow) {
            const double t = (int16_t)ld16(k + kAngleY) * M_PI / 32768;
            dx = sin(t), dz = cos(t), d = 1;
        }
        // (its old position, +0x300, the same: its collision sweeps from there, and a ship's hull in between held it
        // fast while the boat sailed on)
        for (uint32_t o : {(uint32_t)kPos, 0x300u})
            st32(a + o, u32_bits((float)(lion.x - dx / d * tow))), st32(a + o + 8, u32_bits((float)(lion.z - dz / d * tow)));
        const uint16_t yaw = (uint16_t)(int16_t)(atan2(dx, dz) * 32768 / M_PI);
        st16(a + 0x322, yaw), st16(a + kAngleY, yaw);
        for (int i = 0; i < 3; i++) st32(a + 0x330 + 4 * i, u32_bits(g_craft->scale));
        st32(a + 0x370, u32_bits(std::fabs((float)ldf32(k + 0x370))));
    }
    f_02363164_orig(c);
}
// daObj_Ikada::setMtx (02360420): its model takes the actor's scale, but its BG's matrix (mMtx, which its dBgW points
// at) is position and rotation only: for the craft in tow it is scaled after, so its deck is as big as it looks; and it
// sits its dy lower (its execute sets its height on the water just before)
extern "C" void f_02360420_orig(Cpu* c);
// the matrix in an Ikada its BG points at (its translation the actor's position)
static uint32_t ikada_bg_mtx(uint32_t a) {
    using namespace mods::extras;
    if (const uint32_t bgw = ld32(a + 0x6E4); in_mem2(bgw))
        for (uint32_t o = 0; o < 0x200; o += 4)
            if (const uint32_t m = ld32(bgw + o); m >= a && m < a + 0x2000 && std::fabs(ldf32(m + 0xC) - ldf32(a + kPos)) < 0.5 &&
                std::fabs(ldf32(m + 0x1C) - ldf32(a + kPos + 4)) < 0.5 && std::fabs(ldf32(m + 0x2C) - ldf32(a + kPos + 8)) < 0.5)
                return m;
    return 0;
}
extern "C" void hook_02360420(Cpu* c) {
    using namespace mods::extras;
    const uint32_t a = c->r[3];
    const bool ours = a && a == g_beedle_actor;
    if (ours) st32(a + kPos + 4, u32_bits((float)(ldf32(a + kPos + 4) + g_craft->dy)));
    f_02360420_orig(c);
    if (!ours) return;
    if (!g_beedle_mtx) g_beedle_mtx = ikada_bg_mtx(a);
    if (const uint32_t m = g_beedle_mtx)
        for (uint32_t r = 0; r < 3; r++)
            for (uint32_t k = 0; k < 3; k++) st32(m + 16 * r + 4 * k, u32_bits((float)ldf32(m + 16 * r + 4 * k) * g_craft->scale));
}
// dBgS::GetExitId (024EECC8; r4 the polygon): no exit while Link is aboard Beedle's ship. Its hut's floor is a door into
// Beedle's shop, an exit of the sea room the game's own Beedle's ship lies in: from ours it would load a room that is
// not there (d_s_room.cpp "fileList != 0", a halt)
extern "C" void f_024EECC8_orig(Cpu* c);
extern "C" void hook_024EECC8(Cpu* c) {
    f_024EECC8_orig(c);
    if (mods::extras::g_beedle_aboard) c->r[3] = 0x3F;
}

// daTornado: the wandering cyclone's. Its create (024C99E0) and execute (024C8F68, 024C8FF8) test event bit 0x2710
// (Cyclos beaten: it fades out and goes) just before these sites, r3 the answer, r29 the cyclone: unset for ours
static bool our_cyclone(uint32_t a) { return mods::extras::g_cyclone.id != 0xFFFFFFFF && ld32(a + 4) == mods::extras::g_cyclone.id; }
extern "C" void site_024C99E0(Cpu* c) { if (our_cyclone(c->r[29])) c->r[3] = 0; }
extern "C" void site_024C8F68(Cpu* c) { if (our_cyclone(c->r[29])) c->r[3] = 0; }
extern "C" void site_024C8FF8(Cpu* c) { if (our_cyclone(c->r[29])) c->r[3] = 0; }
// its execute (024C8A58, r3 the cyclone): it takes the boat from within 10000 (with the ship-ride status, play +0x5CD8
// bit 0x10000); ours only from within 3000: beyond, the status is hidden from it while it runs
extern "C" void f_024C8A58_orig(Cpu* c);
extern "C" void hook_024C8A58(Cpu* c) {
    using namespace mods::extras;
    const uint32_t a = c->r[3], ship = ld32(kPlay + 0x5B3C), flags = ld32(kPlay + 0x5CD8);
    const bool hide = a && a == g_cyclone_actor && in_mem2(ship) && ld32(ship + 0x700) != g_cyclone.id &&
                      std::hypot(ldf32(ship + kPos) - ldf32(a + kPos), ldf32(ship + kPos + 8) - ldf32(a + kPos + 8)) > 3000;
    if (hide) st32(kPlay + 0x5CD8, flags & ~0x10000u);
    f_024C8A58_orig(c);
    if (hide) st32(kPlay + 0x5CD8, ld32(kPlay + 0x5CD8) | (flags & 0x10000u));
}
// daShip_c::procTornadoUp (02481C94; r3 the boat): carried up by a cyclone, it follows the cyclone's joints every step
// through mTornadoActor (+0x704, GameCube 0x41C) without asking whether it is still there; one that went (a cyclone of
// the sea's that saw Cyclos beaten and faded, with the boat already in its grip) left it reading address 0x3B4 (a
// halt). Gone: the boat is let go (its tornado id +0x700 and actor cleared) and put back to waiting (procWait_init
// 0247C300)
extern "C" void f_02481C94_orig(Cpu* c);
extern "C" void hook_02481C94(Cpu* c) {
    const uint32_t ship = c->r[3], t = ld32(ship + 0x704);
    if (t && (t < 0x10000000 || t >= 0x50000000 || !guest_call(c, GC(0x025DE50C), {ld32(ship + 0x700)}))) {
        st32(ship + 0x700, 0xFFFFFFFF), st32(ship + 0x704, 0);
        guest_call(c, GC(0x0247C300), {ship});
        mods::trace("extras: the boat's cyclone is gone: it is let go");
        c->r[3] = 1;
        return;
    }
    if (!t) {  // (no cyclone at all)
        guest_call(c, GC(0x0247C300), {ship});
        c->r[3] = 1;
        return;
    }
    f_02481C94_orig(c);
}
// daDaiocta's modeDelete (0211BFFC; r3 Big Octo), its death with all its eyes out: ours is marked (its loot), and dies
// as the game has it
extern "C" void f_0211BFFC_orig(Cpu* c);
extern "C" void hook_0211BFFC(Cpu* c) {
    using namespace mods::extras;
    if (c->r[3] && c->r[3] == g_octo_actor) g_octo_dead = true;
    f_0211BFFC_orig(c);
}

// fopAcM_fastCreate (025D5928): the boat's own cannon making its ball (daShip execute, returning to 02479684), for the
// other players (online co-op)
extern "C" void f_025D5928_orig(Cpu* c);
extern "C" void hook_025D5928(Cpu* c) {
    const bool boat = c->lr == GC(0x02479684) && c->r[3] == 0x126, coop = mods::coop::net::role() != mods::coop::net::Role::kOff;
    const uint32_t pos = c->r[5], angle = c->r[7];
    if (boat && coop) {
        auto& b = mods::extras::g_boat_shot;
        for (int i = 0; i < 3; i++) b.pos[i] = (float)ldf32(pos + 4 * i), b.angle[i] = (int16_t)ld16(angle + 2 * i);
    }
    f_025D5928_orig(c);
    if (boat && coop) mods::extras::g_boat_shot.ball = c->r[3];
}

// Online co-op, the host's monsters (extras.cpp, "The host's monsters, type by type"): each execute with Link standing
// where the player it goes for is (CoopStandIn): Gyorg 021689C8, its controller 0216C13C, Big Octo 0211AE68, Molgera
// 020FA614, a Seahat 023CC070, the Shadow stalker's Bokoblin body 0209BD1C (its mind reads Link's place there too);
// Phantom Ganon and the cannon boats in their hooks above
#define COOP_STAND_IN(addr)                                                   \
    extern "C" void f_##addr##_orig(Cpu* c);                                  \
    extern "C" void hook_##addr(Cpu* c) {                                     \
        mods::extras::CoopStandIn s(c->r[3]);                                 \
        f_##addr##_orig(c);                                                   \
    }
COOP_STAND_IN(021689C8)
COOP_STAND_IN(0216C13C)
COOP_STAND_IN(0211AE68)
COOP_STAND_IN(020FA614)
COOP_STAND_IN(023CC070)
COOP_STAND_IN(0209BD1C)
// a guest's copies choose nothing: Gyorg's mode procedure (modeProcCall 02168938), Big Octo's and a cannon boat's mode
// updates (their modeProc, 0211889C / 023C58DC, with 1: update; a mode's init, 0, is how the host's mode is put in)
extern "C" void f_02168938_orig(Cpu* c);
extern "C" void hook_02168938(Cpu* c) {
    if (!mods::extras::coop_puppet(c->r[3])) f_02168938_orig(c);
}
extern "C" void f_0211889C_orig(Cpu* c);
extern "C" void hook_0211889C(Cpu* c) {
    if (c->r[4] != 1 || !mods::extras::coop_puppet(c->r[3])) f_0211889C_orig(c);
}
extern "C" void f_023C58DC_orig(Cpu* c);
extern "C" void hook_023C58DC(Cpu* c) {
    if (c->r[4] != 1 || !mods::extras::coop_puppet(c->r[3])) f_023C58DC_orig(c);
}
// the animations the host's monsters set (their anm_init: r4 index, f1 morf, r5 loop, f2 speed, r6 flag), for the guests
#define COOP_ANM(addr)                                                                                          \
    extern "C" void f_##addr##_orig(Cpu* c);                                                                    \
    extern "C" void hook_##addr(Cpu* c) {                                                                       \
        using namespace mods::extras;                                                                           \
        if (mods::coop::sync::host() && coop_find(c->r[3])) {                                                   \
            CoopAnm& a = g_coop_anm[ld32(c->r[3] + 4)];                                                         \
            a = {(uint8_t)c->r[4], (uint8_t)c->r[5], (uint8_t)c->r[6], (uint8_t)(a.seq + 1), (float)c->f[1].ps0, (float)c->f[2].ps0}; \
        }                                                                                                       \
        f_##addr##_orig(c);                                                                                     \
    }
COOP_ANM(02134108)
COOP_ANM(020F9AF4)
COOP_ANM(023C9AD8)

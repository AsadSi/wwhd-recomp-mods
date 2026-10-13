// Online co-op, the session (docs/multiplayer.md): the menu's settings, hosting and joining, the other players as
// puppets, the join (the host's portable state loaded on the guest, saving off there) and back to the title when the
// host is gone. The world the players share (Tetra's ship, cannon shots, sea events, the host's monsters) is
// extras.cpp's, through world_messages() / host_state().
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "coop_net.h"

struct Cpu;

namespace mods::coop::sync {

enum Kind : uint8_t { kPose = 1, kShip, kMonsters };  // the unreliable states

struct Settings {
    int role = 0;  // 0 host, 1 join (the catalogue's choice)
    std::string addr = "127.0.0.1", name = "Link";
    int port = net::kPort, tunic = 0;  // tunic: 0 green, 1 red, 2 blue, 3 purple (the Four Swords set)
};
Settings settings();
void set_settings(const Settings& s);  // the menu (any thread); a running session restarts with them
const char* status();
constexpr const char* kTunics[] = {"Green", "Red", "Blue", "Purple"};

// every step, also at the title: the session started or stopped as the toggle says, the socket read and written
void poll(Cpu* c, bool on);
// every step in play, after the mods' monsters: this game's pose out, the other players drawn (Tetra's ship `hull`, 0
// none: whose deck carries poses in its frame)
void step(Cpu* c, uint32_t link, uint32_t hull);

bool host();   // hosting
bool guest();  // joined someone's game (welcomed)
// another player's place in the world as drawn now (for pushing), i 0..kPuppets-1
bool place(int i, float out[3]);
// the guest's join, once its Link has arrived in the host's world: where the host stood on the ship's deck (its frame),
// for extras to set it down there once the ship is made (taken once)
bool take_deck_drop(float local[3]);

// the world's reliable messages to this game (kFired, kSeaEvent, kHit, monster spawns and deaths), in order
struct WorldMsg { int from; net::Msg type; std::string body; };
std::vector<WorldMsg> take_world();
// the host's newest state of a kind, shown 100 ms behind: two snapshots around then and how far between (t 0..1)
bool host_state(Kind k, std::string& a, std::string& b, float& t);

}  // namespace mods::coop::sync

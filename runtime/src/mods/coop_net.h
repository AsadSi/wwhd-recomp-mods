// Online co-op over UDP (docs/online-coop.md): one socket a game, the host's world, guests as thin copies.
// Each packet carries the sender's reliable messages until they are acked (Quake 3's way) and its newest acks; poses
// and the host's ship go unreliably at ~20 Hz and are shown 100 ms behind, between the two snapshots around that time.
#pragma once
#include <cstdint>
#include <string>

struct Cpu;

namespace mods::coop::net {

constexpr int kProto = 2, kPort = 7777, kMaxPlayers = 4;

// the reliable messages (the game's side reads them through on_message)
enum Msg : uint8_t {
    kHello = 1,   // guest -> host: {u16 proto, u32 build, name, u8 tunic} (sent unreliably until answered)
    kWelcome,     // host -> guest: {u8 id, u8 tunic: the one asked for, or the next no one wears}
    kReject,      // host -> guest: text
    kJoinState,   // host -> guest: {u32 total, u32 at, bytes}: the host's portable state, in pieces
    kJoined,      // host -> all: {u8 id, u8 tunic, name}
    kLeft,        // host -> all: {u8 id}
    kBye,         // either way: leaving (sent a few times, unreliably)
    kFired,       // any -> host -> others: a cannon shot (step 3)
    kSeaEvent,    // host -> guests (step 4)
    kHit,         // guest -> host (step 4)
    kSpawn,       // host -> guests: one of the host's monsters made (step 4)
    kKill,        // host -> guests: it is gone
    kEffect,      // any -> host -> others: the particles a player's Link started in a step (relative to him)
    kArrived,     // guest -> host: its Link is in the host's world (the join): the running event and monsters to it
    kBoatShot,    // any -> host -> others: the boat's own cannon fired {f32 pos[3], s16 angle[3], f32 speedF, speed y, gravity}
    kDamage,      // guest -> host: {u16 n, u8 health}: its hit on the host's monster n (the host's health counts)
    kProgress,    // any -> host -> others: save data fields a player changed (coop_progress.h)
};

enum class Role { kOff, kHost, kGuest };
Role role();
int my_id();                     // 0 is the host
const char* status();            // one line for the menu: "Hosting on 7777, 2 players, ping 3 ms"
uint32_t build_hash();           // FNV-1a of this executable

// start or stop; WWHD_COOP=host[:port] or join:<addr>[:port] (with WWHD_COOP_NAME, WWHD_COOP_TUNIC) at the first step
bool start(Role r, const std::string& addr, int port, const std::string& my_name, int my_tunic, std::string& why);
void stop(const char* why);

// queue a reliable message to one player (-1: everyone this game talks to)
void send(int to, Msg type, const std::string& body);
// unreliable: this game's newest snapshot of kind k (a pose, the ship), sent at the next tick to everyone
void put_state(uint8_t kind, const std::string& body);

// each logic step: read the socket, call back, send what is due
struct Handler {
    virtual void message(int from, Msg type, const uint8_t* d, size_t n) = 0;
    virtual void state(int from, uint8_t kind, uint32_t ms, const uint8_t* d, size_t n) = 0;
    virtual void joined(int id) = 0;
    virtual void left(int id, const char* why) = 0;
};
void poll(Handler& h);
uint32_t now_ms();
int ping_ms(int id);
const std::string& name(int id);
int tunic(int id);
bool connected(int id);

}  // namespace mods::coop::net

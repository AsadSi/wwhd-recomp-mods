// Online co-op (docs/multiplayer.md): Link's pose as he is drawn, and puppets that show another player's pose.
// A pose is every joint matrix of every model Link drew in one logic step, relative to his place and heading.
// A puppet is our own copy of each of Link's models (made beside his, on his heap, as he is created), its joint
// matrices written from a pose and entered for drawing after him: no player logic, no process of its own.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Cpu;

namespace mods::coop {

constexpr int kPuppets = 3;  // up to three other players

struct Joint { int16_t q[4], t[3]; uint8_t s; };  // rotation (x32767), translation (x8), uniform scale (x64)
struct Model { uint8_t k = 0, param = 0; std::vector<Joint> j; };  // k: Link's model by creation order
struct Pose {
    char stage[8] = {};
    int8_t room = 0;
    uint8_t flags = 0;           // kOnDeck: pos is ship-local (step 3)
    float pos[3] = {};
    int16_t yaw = 0;
    uint16_t proc = 0, equip = 0;
    std::vector<Model> models;
};

// the bytes of a pose (the recorder's frame and the network's), and back
void pack(const Pose& p, std::string& out);
bool unpack(const uint8_t* d, size_t n, Pose& p, size_t* used = nullptr);

// a pose between two, t 0..1: the place, the heading the short way, and each joint of the models both have
void blend(const Pose& a, const Pose& b, float t, Pose& out);

// the real Link's pose from his last draw (step() asks for a new one each logic step); false until one is taken
bool own_pose(Pose& out);
// what puppet i shows: a pose drawn at a place and heading (nullptr: hidden)
void show(int i, const Pose* p, const float pos[3], int16_t yaw);

// each logic step (extras.cpp): the test aids WWHD_COOP_RECORD=<file> and WWHD_COOP_GHOST=<file>[@dx,dz]
void step(Cpu* c, uint32_t link, bool on);

}  // namespace mods::coop

// Twilight Princess's Darknut in WW (daB_TN's "zako", the Hyrule Castle kind), a TP monster (tp_monsters.h) on a WW
// Darknut (daTn, proc 0xBF) as its body: the body's model, sword and cape are not drawn, its chase and fight mind
// (Tn_move) is replaced by a port of daB_TN's (armoured: walk, sword cuts and shield bash, a piece of armour off each
// hit; unarmoured: the second sword, faster cuts and thrusts), and its execute still walks it, guards, takes hits,
// is stunned and dies as WW's does. Two roles: the Phantom stalker's Darknut (extras.cpp spawns its body) and, with
// the "Twilight Princess Darknuts" mod on, every Darknut the game has.
#pragma once
#include <cstdint>

struct Cpu;

namespace mods::darknut {

bool available();                  // its asset is there (game/tp_darknut.bin)
void set_stalker(uint32_t body);   // the stalker's Darknut body (0 none)
void replace_all(bool on);         // every WW Darknut drawn and fought as TP's
void step(Cpu* c, uint32_t link);  // each logic step
bool hides_weapon(uint32_t actor); // a Darknut's own sword (daBoko), not drawn

}  // namespace mods::darknut

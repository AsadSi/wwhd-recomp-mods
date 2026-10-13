// Shadow stalker: Twilight Princess's Shadow Beast as the Phantom stalker's hunter (mods/extras.cpp sends it out), a
// TP monster (tp_monsters.h). A Bokoblin is its body: its model and weapon are not drawn, its own mind (Bk_move) is
// replaced by a port of daE_S1's, and its execute still walks it, takes Link's hits and, at the end, drowns or dies.
#pragma once
#include <cstdint>

struct Cpu;

namespace mods::beast {

bool available();  // its asset is there (game/tp_shadow_beast.bin)
// the Bokoblin that is its body (0 none)
void set_body(uint32_t body);
// each logic step: follows its body (0 once it is gone: the beast dies and dissolves, or fades out)
void step(Cpu* c, uint32_t link, double now);

}  // namespace mods::beast

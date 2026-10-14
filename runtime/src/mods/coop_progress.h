// Online co-op, the shared progress (docs/multiplayer.md): what one player gets or does in the story is the others'
// too. Each step every game compares its save data (dSv_save_c, zeldaret/tww's layout, which the HD game keeps) with
// the step before and sends what it changed, field by field; the others put it in theirs. Anchor's way (Ship of
// Harkinian), by comparison instead of hooks: the story's event bits, each stage's chests, switches, pickups, visited
// rooms, small keys and dungeon items, the ocean's salvage bits, the items, songs, pearls, Triforce, charts, maps and
// capacities. Not shared (Windwaker-coop's choice): health, rupees, magic, ammunition, bottle contents, bags.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mods::coop::progress {

// another game's changes (net::kProgress), put in at the next step
void received(const uint8_t* d, size_t n);
// every step in play: `live` once this game plays in the host's world (sync::arrived()); its own changes out, the
// others' in. Not live: nothing compared, the others' changes kept for the arrival
void step(bool live);
// the session ended: the others' changes still kept are dropped
void stop();

}  // namespace mods::coop::progress

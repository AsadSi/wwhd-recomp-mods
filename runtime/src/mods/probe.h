// A live probe for reverse engineering (tools/probe.sh): while WWHD_PROBE=<file> is set, the running game reads that
// file's commands each logic step it changed, and writes the answers to <file>.out. No rebuild or restart to look at
// memory or call a function.
//   peek <expr> [bytes]          memory as hex words and floats (64 bytes unless told)
//   str <expr>                   the text at an address
//   call <func> [args...]        a guest function (its code address, as the GC() addresses in the mods), r3 back
//   poke <expr> <value>          a word written
//   names                        the names below with their values
// An expression adds terms with '+' (or '-'): hex numbers, names (link: Link; and what the mods set with name()),
// and [expr], the word at that address. E.g. "peek [[body+2C]+10] 48": Link's body's first joint matrix.
#pragma once
#include <cstdint>

struct Cpu;

namespace mods::probe {

void name(const char* n, uint32_t value);  // a name for expressions (a mod's model, ...)
// a live knob: a word of guest memory (0 at first) named n, for a mod to read each step (ld32) and the probe to set
// ("poke <n> 1"): a switch or a value tried without a rebuild
uint32_t knob(const char* n);
void step(Cpu* c, uint32_t link);          // every logic step (extras.cpp)

}  // namespace mods::probe

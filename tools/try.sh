#!/bin/bash
# Usage: tools/try.sh <feature>   Plays one of the new mods from the haunted sea state (save/haunted.wwstate, on the
# boat NE of Outset): pick Quest Log 1, and the feature's event (if any) starts about 20 s after the file loads.
# Only that feature's mods are on; the spooky events come only when forced. Prints the controls first.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
pgrep -fl "wwhd " >/dev/null && { echo "The game is already running; close it first."; exit 1; }
[ -f "$ROOT/save/haunted.wwstate" ] || { echo "save/haunted.wwstate is missing."; exit 1; }
SPOOKY=(NIGHT_DEAD HAUNTED_SEA STORM FOG GIANT_SEAHATS PHANTOM_GANON HORROR_AUDIO DEEP LEVIATHAN MOLGERA PHANTOM_STALKER BIG_SHIP)
ON=(); EVENT=; NOTE=
SHIP="Big ship: stop the boat beside the ship and press B (Get Out) to jump onto its deck. Walk it, climb its castles,
  use your weapons. Back in the boat: stand at the rail by the boat and press A. Fall in the sea: swim to the hull's
  side and press A to climb up. Sail off alone and it waits at anchor; sail up alongside it, slowly, to hitch it on.
Deck cannons (bow and stern): stand still by one and press A to man it. Left stick aims, ZL locks on the nearest
  monster (again to unlock), A fires (one bomb a shot), B leaves."
case "${1:-}" in
  deck) ON=(BIG_SHIP); NOTE="$SHIP" ;;
  cannon|sea-deck) ON=(BIG_SHIP HAUNTED_SEA); EVENT="sea@+20"; NOTE="$SHIP
Haunted sea comes at about 20 s: get onto the deck and shoot the Seahats, Gyorgs and cannon boats." ;;
  leviathan-deck) ON=(BIG_SHIP LEVIATHAN); EVENT="leviathan@+20"; NOTE="$SHIP
The Leviathan surfaces at about 20 s. 48 health: a cannonball or bomb takes 4, an arrow 2, the sword 1-2.
Killing it gives 200 rupees and a blue potion (into an empty bottle)." ;;
  molgera-deck) ON=(BIG_SHIP MOLGERA); EVENT="molgera@+30"; NOTE="$SHIP
Molgera comes at about 30 s: get onto the deck first. 80 health (a cannonball 4); 300 rupees and a blue potion." ;;
  phantom-deck) ON=(BIG_SHIP PHANTOM_GANON); EVENT="phantom@+30"; NOTE="$SHIP
Phantom Ganon (normal size) comes at about 30 s: fight him from the deck." ;;
  molgera-chase) ON=(MOLGERA); EVENT="molgera@+20"; NOTE="Molgera comes at about 20 s, far off, and hunts you.
Set sail (A) and sail full speed to get away; it now and then dives and bursts up in your way. Or turn and fight it." ;;
  phantom-titanic) ON=(PHANTOM_GANON); EVENT="phantom@+20"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1
  NOTE="Titanic (12x) Phantom Ganon at about 20 s: his dark balls, their bursts and the sparks of hits are his size now." ;;
  anchor-island) ON=(BIG_SHIP); NOTE="$SHIP
Sail toward Outset (south-west): the ship drops anchor by itself before the island; take the boat in, then come back." ;;
  seahats-swim) ON=(HAUNTED_SEA); EVENT="sea@+20"; NOTE="Haunted sea at about 20 s: get out of the boat (B) and swim.
The Seahats should now swoop at you while you swim instead of drifting off." ;;
  *) echo "Usage: tools/try.sh <deck|cannon|leviathan-deck|molgera-deck|phantom-deck|molgera-chase|phantom-titanic|anchor-island|seahats-swim>"; exit 2 ;;
esac
for m in "${SPOOKY[@]}"; do export WWHD_MOD_$m=0; done
for m in "${ON[@]}"; do export WWHD_MOD_$m=1; done
[ -n "$EVENT" ] && export WWHD_SPOOKY="$EVENT"
echo "$NOTE"
echo
echo "Pick Quest Log 1. Trace: $ROOT/build/try-trace.txt"
cd "$ROOT" && WWHD_PORTABLE_LOAD="$ROOT/save/haunted.wwstate" WWHD_MODS_TRACE="$ROOT/build/try-trace.txt" ./build/cmake/wwhd

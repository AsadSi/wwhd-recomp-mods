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
SHIP="Big ship (keys: move WASD, A = K/Space, B = J, ZL = Left Shift, pause/items = Return):
  Stop the boat beside the ship and press B (Get Out) to jump onto its deck. Walk it, climb its castles, use weapons.
  Sail it: on the stern castle (the raised deck at the back), stand by the middle and press A to take the helm.
  Stick up raises the sail, down lowers it, left/right steers (quicker the faster you go); B lets go and it drifts
  to a stop. The Red Lion comes along tied to its side.
  Back in the boat: stand at the rail by the boat and press A. Fall in the sea: swim to the hull's side and press A.
  Sail off alone in the boat and the ship waits at anchor; sail up alongside it, slowly, to hitch it on again.
Deck cannons (bow and stern): stand by one and press A (or your Bomb Bag's button) to man it. Stick aims, ZL locks on
  the nearest monster (again to unlock), A fires (one bomb a shot), B leaves."
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
  bosses) ON=(BIG_SHIP MOLGERA PHANTOM_GANON); EVENT="molgera@+30;phantom@+210"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1; NOTE="$SHIP
Molgera at about 30 s (80 health: 20 cannon hits; 300 rupees and a blue potion), then Titanic Phantom Ganon at about
3.5 min (18 cannon hits drive him off). Get onto the deck first; sail the ship or man a cannon. Your Bomb Bag
is filled up as each one comes; the fog is thinner while they are out." ;;
  titanic-deck) ON=(BIG_SHIP PHANTOM_GANON); EVENT="phantom@+30"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1; NOTE="$SHIP
Titanic (12x) Phantom Ganon comes at about 30 s: get onto the deck first, then fight him from it (cannons, bow,
sword). His dark balls, their bursts and his hit sparks are his size." ;;
  molgera-chase) ON=(MOLGERA); EVENT="molgera@+20"; NOTE="Molgera comes at about 20 s, far off, and hunts you.
Set sail (A) and sail full speed to get away; it sweeps in to bite and turns to come again. Or turn and fight it." ;;
  phantom-titanic) ON=(PHANTOM_GANON); EVENT="phantom@+20"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1
  NOTE="Titanic (12x) Phantom Ganon at about 20 s: his dark balls, their bursts and the sparks of hits are his size now." ;;
  anchor-island) ON=(BIG_SHIP); NOTE="$SHIP
Sail toward Outset (south-west): the ship drops anchor by itself before the island; take the boat in, then come back." ;;
  seahats-swim) ON=(HAUNTED_SEA); EVENT="sea@+20"; NOTE="Haunted sea at about 20 s: get out of the boat (B) and swim.
The Seahats should now swoop at you while you swim instead of drifting off." ;;
  all) ON=(BIG_SHIP HAUNTED_SEA GIANT_SEAHATS LEVIATHAN MOLGERA PHANTOM_GANON HORROR_AUDIO); EVENT="leviathan@+60;molgera@+260;phantom@+460;sea@+640"
  export WWHD_MOD_PHANTOM_GANON_CHOICE=1  # Titanic
  NOTE="$SHIP
Everything at once. The first minute is yours: get on the deck, try the helm and a cannon. Then, one after another:
  at 1 min the Leviathan (48 health), at about 4.5 min Molgera (80 health), at about 8 min Titanic Phantom Ganon, at about
  11 min Haunted sea. Each ends when you kill it, outrun it or after its time. Kills give rupees and a blue potion." ;;
  *) echo "Usage: tools/try.sh <all|bosses|titanic-deck|deck|cannon|leviathan-deck|molgera-deck|phantom-deck|molgera-chase|phantom-titanic|anchor-island|seahats-swim>"; exit 2 ;;
esac
for m in "${SPOOKY[@]}"; do export WWHD_MOD_$m=0; done
for m in "${ON[@]}"; do export WWHD_MOD_$m=1; done
[ -n "$EVENT" ] && export WWHD_SPOOKY="$EVENT"
echo "$NOTE"
echo
TRACE="$ROOT/build/try/${1}-$(date +%H%M%S).txt"; mkdir -p "$ROOT/build/try"
echo "Pick Quest Log 1. Trace: $TRACE"
cd "$ROOT" && WWHD_PORTABLE_LOAD="$ROOT/save/haunted.wwstate" WWHD_MODS_TRACE="$TRACE" ./build/cmake/wwhd

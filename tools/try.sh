#!/bin/bash
# Usage: tools/try.sh <feature>   Plays one of the new mods from the haunted sea state (save/haunted.wwstate, on the
# boat NE of Outset): pick Quest Log 1, and the feature's event (if any) starts about 20 s after the file loads.
# Only that feature's mods are on; the spooky events come only when forced. Prints the controls first.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
pgrep -fl "wwhd " >/dev/null && { echo "The game is already running; close it first."; exit 1; }
[ -f "$ROOT/save/haunted.wwstate" ] || { echo "save/haunted.wwstate is missing."; exit 1; }
SPOOKY=(NIGHT_DEAD HAUNTED_SEA STORM FOG GIANT_SEAHATS PHANTOM_GANON HORROR_AUDIO DEEP LEVIATHAN MOLGERA PHANTOM_STALKER BIG_SHIP BEEDLE_SHIP
        DEAD_CALM PHOTO_MODE SECOND_GUNNER BLOOD_MOON CYCLONES MAELSTROM ARMADA SIREN WRONG_LION DROWNED BOARDING WRECKS CREW TIME)
ON=(); EVENT=; NOTE=; EXTRA=
SHIP="Big ship (keys: move WASD, A = K/Space, B = J, ZL = Left Shift, pause/items = Return):
  Tetra's ship is a boat of its own: once you are out at sea it waits at anchor a little way off your left.
  Sail up beside it so it is on your left, stop, and press B (Get Out) to leap onto its deck. Walk it, climb its castles.
  Sail it: on the stern castle (the raised deck at the back), stand by the wheel and press A to take the helm.
  Stick up raises the sail, down lowers it, left/right steers (quicker the faster you go); B lets go and it drifts
  to a stop. The Red Lion comes along beside it while it sails.
  Back in the boat: stand at the rail by the boat and press A. Fall in the sea: swim to the hull's side and press A.
  It stays where you leave it; leave it far behind and it waits near you again the next time you are at sea.
Deck cannons (bow and stern): stand by one and press A (or your Bomb Bag's button) to man it. Stick aims, ZL locks on
  the nearest monster (again to unlock), A fires (one bomb a shot), B leaves."
BEEDLE="Beedle's ship (keys: move WASD, A = K/Space, B = J, ZL = Left Shift, pause/items = Return):
  The King of Red Lions tows Beedle's shop ship behind him on a rope: sail as always and it swings in behind you.
  Stop and press B (Get Out) to leap back onto its deck. Walk it and fight from it. A at its bow (the end by the
  Lion, away from the bow gun) leaps you forward into the boat. Fall in: swim to its side and press A to climb aboard.
  Guns: one at the bow, one on the hut's roof. Stand by the bow gun, or in front of the hut for the roof gun, and press A
  (or your Bomb Bag's button) to man it: stick aims, ZL locks on the nearest monster, A fires (a bomb a shot), B leaves."
case "${1:-}" in
  plain) NOTE="Every new mod off: plain sailing, to compare (does Link and the boat lag here too?)." ;;
  beedle) ON=(BEEDLE_SHIP); NOTE="$BEEDLE" ;;
  beedle-guns) ON=(BEEDLE_SHIP HAUNTED_SEA); EVENT="sea@+30"; NOTE="$BEEDLE
Haunted sea at about 30 s: get onto its deck and try both guns." ;;
  gunner) ON=(BEEDLE_SHIP SECOND_GUNNER HAUNTED_SEA); EVENT="sea@+30"; NOTE="$BEEDLE
Connect TWO controllers. Player 2 (the second controller) has the roof gun from the start: left stick aims, ZL locks
on, A or ZR fires. Player 1 sails and plays as ever on the first controller. Haunted sea at about 30 s." ;;
  crew) ON=(BEEDLE_SHIP CREW ARMADA); EVENT="armada@+30"; NOTE="$BEEDLE
The ship's crew fires both guns by itself at the nearest monster. A pirate armada comes at about 30 s: stop and watch,
or man one gun yourself (the crew keeps the other)." ;;
  boarding) ON=(BEEDLE_SHIP BOARDING ARMADA); EVENT="armada@+30"; NOTE="$BEEDLE
Get onto the deck before 30 s; then a pirate armada comes. Stay on the deck: when a cannon boat comes within 2500,
two Bokoblins leap aboard." ;;
  drowned) ON=(BEEDLE_SHIP DROWNED); EVENT="drowned@+30"; AT_NIGHT=1; NOTE="$BEEDLE
It is night. Get onto the deck before 30 s: then ReDeads climb aboard, one every 12 s (three at most), for 2 minutes." ;;
  calm) ON=(DEAD_CALM HORROR_AUDIO); EVENT="calm@+20"; NOTE="Set sail (A). At about 20 s the wind dies and a still fog settles; try the Wind Waker (it turns the wind, not the calm)." ;;
  photo) ON=(PHOTO_MODE); NOTE="Press F7 for the free camera: left stick / WASD flies, right stick / arrow keys look, L/R (Q/E) down and up,
ZL (Shift) for speed. F7 again gives the camera back." ;;
  blood) ON=(BLOOD_MOON NIGHT_DEAD HORROR_AUDIO); EVENT="blood@+20"; AT_NIGHT=1; NOTE="It is night. At about 20 s the blood moon: the world turns red over 20 s. Sail to an island (Outset is south-west)
and land: the dead rise twice as fast." ;;
  cyclone) ON=(CYCLONES); EVENT="cyclone@+20"; NOTE="At about 20 s a cyclone appears far ahead and wanders across your course. Steer around it, then try sailing
into it (within 3000 it takes the boat and throws you to another part of the sea)." ;;
  maelstrom) ON=(MAELSTROM); EVENT="maelstrom@+20"; NOTE="At about 20 s a whirlpool opens beside the boat and drags you round. Wrench the stick left-right to break free,
or wait 45 s (it takes two hearts)." ;;
  armada) ON=(ARMADA); EVENT="armada@+20"; NOTE="At about 20 s four cannon boats come in formation from ahead. Sink them all with the boat's cannon (R, then A
to fire) for 200 rupees and a blue potion. Sail away and they follow." ;;
  siren) ON=(SIREN); EVENT="siren@+20"; NOTE="Set sail (A). At about 20 s a harp melody plays and the boat turns by itself toward it: steer against it.
Then let it take you there (about 7000 off) to see what waits." ;;
  lion) ON=(WRONG_LION); EVENT="lion@+20;lion@+40;lion@+60"; AT_NIGHT=1; NOTE="It is night. Set sail (A). At about 20, 40 and 60 s the King of Red Lions laughs and swerves off by himself
for a few seconds." ;;
  wrecks) ON=(WRECKS); EVENT="wrecks@+20"; AT_NIGHT=1; NOTE="It is night. At about 20 s fog rolls in over a graveyard of ships ahead: sail among the sunken hulks (Poes about)." ;;
  tour) ON=(BEEDLE_SHIP SECOND_GUNNER CREW BOARDING PHOTO_MODE DROWNED ARMADA WRONG_LION DEAD_CALM SIREN WRECKS MAELSTROM CYCLONES BLOOD_MOON NIGHT_DEAD HORROR_AUDIO)
  EVENT="drowned@+60;armada@+190;lion@+440;lion@+455;lion@+470;calm@+490;siren@+640;wrecks@+725;maelstrom@+910;cyclone@+1010;blood@+1170"; AT_NIGHT=1
  NOTE="$BEEDLE
Every new mod in one run, at night (about 20 minutes). Times are from when the file loads:
  0:00  Yours: the tow, get on the deck and back, both guns. F7 = free camera (F7 again to stop). A second controller
        is player 2 on the roof gun; the ship's crew fires whichever gun nobody mans.
  1:00  The drowned: be on the deck. ReDeads climb aboard for 2 minutes.
  3:10  Pirate armada: four cannon boats. On the deck, Bokoblins board from any that come close; sink all four for 200.
  7:20  Something in the Lion, three times: be in the boat, sailing. He laughs and swerves off by himself.
  8:10  Dead calm: the wind dies and a still fog settles.
 10:40  Siren song: a harp, the boat turns toward it. Steer against it, or let it take you to the Gyorgs.
 12:05  Shipwreck graveyard: fog and sunken hulks ahead, Poes about.
 15:10  Maelstrom: wrench the stick left-right to break free (or it takes two hearts after 45 s).
 16:50  Wandering cyclone: steer round it, then sail into it to be thrown across the sea.
 19:30  Blood moon: the world turns red; land on an island and the dead rise twice as fast.
Stay in open sea until the blood moon: going near an island ends whatever sea event is on." ;;
  beedle-bosses) ON=(BEEDLE_SHIP MOLGERA PHANTOM_GANON); EVENT="molgera@+40;phantom@+45"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1; NOTE="$BEEDLE
Molgera and Titanic Phantom Ganon together from about 40 s: fight them from its deck or from the boat's cannon." ;;
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
  duo) ON=(BIG_SHIP MOLGERA PHANTOM_GANON); EVENT="molgera@+30;phantom@+35"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1; NOTE="$SHIP
Molgera and Titanic Phantom Ganon together, from about 30 s: get onto the deck first. Molgera 80 health (20 cannon
hits; 300 rupees and a blue potion), Phantom 18 cannon hits drive him off. Your Bomb Bag is filled up as each comes." ;;
  titanic-deck) ON=(BIG_SHIP PHANTOM_GANON); EVENT="phantom@+30"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1; NOTE="$SHIP
Titanic (12x) Phantom Ganon comes at about 30 s: get onto the deck first, then fight him from it (cannons, bow,
sword). His dark balls, their bursts and his hit sparks are his size." ;;
  molgera-chase) ON=(MOLGERA); EVENT="molgera@+20"; NOTE="Molgera comes at about 20 s, far off, and hunts you.
Set sail (A) and sail full speed to get away; it circles you like a Gyorg, charges in to bite, then circles again. Or turn and fight it." ;;
  phantom-titanic) ON=(PHANTOM_GANON); EVENT="phantom@+20"; export WWHD_MOD_PHANTOM_GANON_CHOICE=1
  NOTE="Titanic (12x) Phantom Ganon at about 20 s: his dark balls, their bursts and the sparks of hits are his size now." ;;
  anchor-island) ON=(BIG_SHIP); NOTE="$SHIP
Sail toward Outset (south-west): the ship drops anchor by itself before the island; take the boat in, then come back." ;;
  cannon-boat) EXTRA="WWHD_EXTRAS_SPAWN=B3:00FFFF00:DAC:0:FF"; NOTE="A cannon boat appears about 3500 away 4 s after the file loads and bombards you. Its aim is the game's
own: it lands short at first and closes in as you fire the boat's cannon back (or hit it)." ;;
  cannon-boat-deck) ON=(BIG_SHIP); EXTRA="WWHD_EXTRAS_SPAWN=B3:00FFFF00:DAC:0:FF WWHD_BIGSHIP_OFF=1100"; NOTE="$SHIP
A cannon boat appears about 3500 away 4 s after the file loads; the Big ship lies right beside you (B to leap on).
Fight it from the deck: its aim lands short at first and closes in as you fire the deck cannons back, as from the boat."  ;;
  seahats-swim) ON=(HAUNTED_SEA); EVENT="sea@+20"; NOTE="Haunted sea at about 20 s: get out of the boat (B) and swim.
The Seahats should now swoop at you while you swim instead of drifting off." ;;
  all) ON=(BIG_SHIP HAUNTED_SEA GIANT_SEAHATS LEVIATHAN MOLGERA PHANTOM_GANON HORROR_AUDIO); EVENT="leviathan@+60;molgera@+260;phantom@+460;sea@+640"
  export WWHD_MOD_PHANTOM_GANON_CHOICE=1  # Titanic
  NOTE="$SHIP
Everything at once. The first minute is yours: get on the deck, try the helm and a cannon. Then, one after another:
  at 1 min the Leviathan (48 health), at about 4.5 min Molgera (80 health), at about 8 min Titanic Phantom Ganon, at about
  11 min Haunted sea. Each ends when you kill it, outrun it or after its time. Kills give rupees and a blue potion." ;;
  soak) ON=(NIGHT_DEAD HAUNTED_SEA STORM FOG GIANT_SEAHATS PHANTOM_GANON HORROR_AUDIO DEEP LEVIATHAN MOLGERA PHANTOM_STALKER BIG_SHIP)
  OFTEN=2; NOTE="$SHIP
The long soak, played: every spooky mod on, How often at Often, nothing forced. Sail out into open sea (away from
islands) and keep going; the events come by chance, a roll a minute (none within reach of an island). Play as you like." ;;
  *) echo "Usage: tools/try.sh <tour|beedle|beedle-guns|gunner|crew|boarding|drowned|calm|photo|blood|cyclone|maelstrom|armada|siren|lion|wrecks|soak|plain|beedle-bosses|all|bosses|duo|titanic-deck|deck|cannon|leviathan-deck|molgera-deck|phantom-deck|molgera-chase|phantom-titanic|anchor-island|seahats-swim|cannon-boat|cannon-boat-deck>"; exit 2 ;;
esac
for m in "${SPOOKY[@]}"; do export WWHD_MOD_$m=0; done
for m in ${ON[@]+"${ON[@]}"}; do export WWHD_MOD_$m=1; [ -n "${OFTEN:-}" ] && export WWHD_MOD_${m}_OFTEN=$OFTEN; done
[ -n "${AT_NIGHT:-}" ] && export WWHD_MOD_TIME=1 WWHD_MOD_TIME_CHOICE=4  # the clock held at 23:00
[ -n "$EVENT" ] && export WWHD_SPOOKY="$EVENT"
echo "$NOTE"
echo
TRACE="$ROOT/build/try/${1}-$(date +%H%M%S).txt"; mkdir -p "$ROOT/build/try"
echo "Pick Quest Log 1. Trace: $TRACE"
cd "$ROOT" && env $EXTRA WWHD_PORTABLE_LOAD="$ROOT/save/haunted.wwstate" WWHD_MODS_TRACE="$TRACE" ./build/cmake/wwhd

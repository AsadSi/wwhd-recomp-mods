#!/bin/bash
# Usage: tools/try.sh <feature>   Plays one of the new mods from the haunted sea state (save/haunted.wwstate, on the
# boat NE of Outset): pick Quest Log 1, and the feature's event (if any) starts about 20 s after the file loads.
# Only that feature's mods are on; the spooky events come only when forced. Prints the controls first.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
pgrep -x wwhd >/dev/null && { echo "The game is already running; close it first."; exit 1; }
[ -f "$ROOT/save/haunted.wwstate" ] || { echo "save/haunted.wwstate is missing."; exit 1; }
SPOOKY=(NIGHT_DEAD HAUNTED_SEA STORM FOG GIANT_SEAHATS PHANTOM_GANON HORROR_AUDIO DEEP LEVIATHAN MOLGERA PHANTOM_STALKER BIG_SHIP BEEDLE_SHIP
        DEAD_CALM PHOTO_MODE SECOND_GUNNER BLOOD_MOON CYCLONES MAELSTROM ARMADA BIG_OCTO WRONG_LION DROWNED BOARDING TIME
        RAFT FAIR_WIND SCARE_PACING SHIP_UPGRADES WARP HELMAROC)
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
  (or your Bomb Bag's button) to man it: stick aims, ZL locks on the nearest monster (again: the next one), A fires (a bomb a shot), B leaves."
case "${1:-}" in
  long) ON=(NIGHT_DEAD HAUNTED_SEA STORM FOG GIANT_SEAHATS PHANTOM_GANON HORROR_AUDIO DEEP LEVIATHAN MOLGERA PHANTOM_STALKER RAFT FAIR_WIND
           DEAD_CALM BLOOD_MOON CYCLONES MAELSTROM ARMADA BIG_OCTO WRONG_LION DROWNED BOARDING); export WWHD_TEST_CREW=1
  OFTEN=2; export WWHD_MOD_DAMAGE=1 WWHD_MOD_DAMAGE_CHOICE=4; NOTE="Claude's long run: every spooky mod at Often, nothing forced, no damage; Claude sails it through the live pad." ;;
  plain) NOTE="Every new mod off: plain sailing, to compare (does Link and the boat lag here too?)." ;;
  beedle) ON=(BEEDLE_SHIP); NOTE="$BEEDLE" ;;
  beedle-guns) ON=(BEEDLE_SHIP HAUNTED_SEA); EVENT="sea@+30"; NOTE="$BEEDLE
Haunted sea at about 30 s: get onto its deck and try both guns." ;;
  gunner) ON=(BEEDLE_SHIP SECOND_GUNNER HAUNTED_SEA); EVENT="sea@+30"; NOTE="$BEEDLE
Connect TWO controllers. Player 2 (the second controller) has the roof gun from the start: left stick aims, ZL locks
on, A or ZR fires. Player 1 sails and plays as ever on the first controller. Haunted sea at about 30 s." ;;
  crew) ON=(BEEDLE_SHIP ARMADA); export WWHD_TEST_CREW=1; EVENT="armada@+30"; NOTE="$BEEDLE
The ship's crew fires both guns by itself at the nearest monster. A pirate armada comes at about 30 s: stop and watch,
or man one gun yourself (the crew keeps the other)." ;;
  boarding) ON=(BEEDLE_SHIP BOARDING ARMADA); EVENT="armada@+30"; NOTE="$BEEDLE
Get onto the deck before 30 s; then a pirate armada comes. Stay on the deck: when a cannon boat comes within 4500,
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
  octo) ON=(BIG_OCTO); EVENT="octo@+20"; AT_NIGHT=1; NOTE="At night. At about 20 s the sea goes still; at 50 s Big Octo rises in a whirlpool by the boat, Gyorgs around." ;;
  lion) ON=(WRONG_LION); EVENT="lion@+20;lion@+40;lion@+60"; AT_NIGHT=1; NOTE="It is night. Set sail (A). At about 20, 40 and 60 s the King of Red Lions laughs and swerves off by himself
for a few seconds." ;;
  raft) ON=(RAFT FAIR_WIND); NOTE="The King of Red Lions tows a big flagged raft (Fair wind on: the sail is always full). Stop and press B to
leap onto the raft; walk it, man its guns (A by one); A at its front edge leaps back into the boat. Fall in: A by its side." ;;
  full) ON=(NIGHT_DEAD HAUNTED_SEA GIANT_SEAHATS LEVIATHAN HORROR_AUDIO RAFT FAIR_WIND BOARDING DEAD_CALM BLOOD_MOON
            CYCLONES MAELSTROM ARMADA BIG_OCTO WRONG_LION DROWNED SCARE_PACING SHIP_UPGRADES); export WWHD_TEST_CREW=1
  EVENT="drowned@+180;armada@+330;lion@+570;calm@+630;octo@+780;maelstrom@+1080;sea@+1200;leviathan@+1380;cyclone@+1560;blood@+1710"
  AT_NIGHT=1; RECORD=1; export WWHD_MOD_DAMAGE=1 WWHD_MOD_DAMAGE_CHOICE=4 WWHD_TEST_SHOP=1
  NOTE="The full test, at night, no damage: the big raft in tow, Fair wind, the ship's crew, and every event with its warning signs
(30 s before each, sail away to escape it): the drowned 3:00, armada + boarding 5:30, the Lion 9:30, dead calm 10:30,
Big Octo 13:00, maelstrom 18:00, Haunted sea 20:00, Leviathan 23:00, cyclone 26:00, blood moon 28:30." ;;
  shop) ON=(RAFT FAIR_WIND SHIP_UPGRADES HAUNTED_SEA); EVENT="sea@+60"; export WWHD_TEST_SHOP=1 WWHD_TEST_CREW=1; NOTE="Zunari's ship fittings open 5 s in, and again on F8 (in play: after talking to Zunari on Windfall). Stick/d-pad (W/S) to choose, A (K) to buy,
B (J) to leave. You have 5000 rupees: buy some, then sail (faster?), get on the raft (B) and fire its guns (quicker,
bigger shots?). Haunted sea at about 1:30 for something to shoot." ;;
  raft-bosses) ON=(RAFT FAIR_WIND SHIP_UPGRADES SECOND_GUNNER MOLGERA PHANTOM_GANON HORROR_AUDIO); EVENT="molgera@+60;phantom@+70"; export WWHD_TEST_CREW=1
  export WWHD_MOD_PHANTOM_GANON_CHOICE=1 WWHD_TEST_SHOP=1; NOTE="The big raft against Molgera and Titanic Phantom Ganon (about 1 min in). The shop opens 5 s in (F8 opens it again): buy
upgrades first (5000 rupees). Get onto the raft (B) before they come and fight from its two guns (A by one; a second controller
mans the other). Molgera 80 health; Phantom 18 shots drive him off." ;;
  chaos) ON=(NIGHT_DEAD HAUNTED_SEA GIANT_SEAHATS LEVIATHAN MOLGERA PHANTOM_GANON HORROR_AUDIO RAFT FAIR_WIND BOARDING DEAD_CALM
             BLOOD_MOON CYCLONES MAELSTROM ARMADA BIG_OCTO WRONG_LION DROWNED SHIP_UPGRADES SECOND_GUNNER)
  EVENT="blood@+20;armada@+40;drowned@+45;sea@+50;calm@+60;octo@+70;leviathan@+80;maelstrom@+90;molgera@+100;phantom@+110;lion@+130;cyclone@+420"
  AT_NIGHT=1; RECORD=1; export WWHD_MOD_DAMAGE=1 WWHD_MOD_DAMAGE_CHOICE=4 WWHD_TEST_UPGRADES=3F WWHD_TEST_CREW=1 WWHD_MOD_PHANTOM_GANON_CHOICE=1
  NOTE="Everything at once, at night, no damage: blood moon, armada, drowned, Haunted sea, calm, Big Octo, Leviathan,
maelstrom, Molgera, Titanic Phantom and the Lion all within the first ~2 minutes; the cyclone at 7 min. Upgrades:
sailing 3, guns 3, shots 3 (no shop). Claude drives;
take the controller any time." ;;
  seamless) ON=(NIGHT_DEAD HAUNTED_SEA STORM FOG GIANT_SEAHATS PHANTOM_GANON HORROR_AUDIO DEEP LEVIATHAN MOLGERA PHANTOM_STALKER
                DEAD_CALM BLOOD_MOON CYCLONES MAELSTROM ARMADA BIG_OCTO WRONG_LION DROWNED BOARDING RAFT FAIR_WIND SHIP_UPGRADES WARP)
  RECORD=1; export WWHD_MOD_DAMAGE=1 WWHD_MOD_DAMAGE_CHOICE=3 WWHD_TEST_UPGRADES=15
  NOTE="The long seamless run: every spooky mod at its normal How often and Scare pacing, nothing forced, the clock
running, half damage, the raft in tow, upgrades at level 1. Claude sails it (build/try/pilot.py); take over any time." ;;
  buy-ship) ON=(BIG_SHIP WARP FAIR_WIND); export WWHD_TEST_STAGE="Obombh 0 0"; NOTE="Goes straight into Windfall's Bomb Shop (5000 rupees). Talk to the shopkeeper: as
the talk ends a panel offers Tetra's ship for 2500 rupees (A buy, B leave). Then sail out to open sea: she should
wait at anchor off your left. Get on (B beside her), take the wheel (A on the stern castle) and sail her: the wind
now sets her speed (try running before it and into it), she speeds up and slows down gently, and turns with a rudder." ;;
  cabin) ON=(BIG_SHIP); NOTE="$SHIP
The cabin: on her deck, walk through the door under the stern castle. Below, a panel opens (F10 opens it again): sleep
until morning or nightfall, fill empty bottles with soup (once a visit), stow your bombs and take them back. Walk out
the way you came: you should come up in the boat beside her, and she should lie where you left her." ;;
  beedle-shop) ON=(SHIP_UPGRADES); export WWHD_TEST_STAGE="${SHOP:-Obshop 4 4}"; NOTE="Straight into a Beedle shop (SHOP=\"Obshop 4 4\": his shop 5, Faster sailing in the Red Potion's place;
SHOP=\"Obshop 2 2\": shop 3, a cannon fitting where 30 Bombs stand once you have the Bomb Bag). Talk to Beedle, look at
the fitting (its name, price and words), buy it: the level goes up, the rupees go down, Link holds up nothing odd." ;;
  deep) ON=(DEEP); EVENT="deep@+15"; NOTE="Fog and the Deep about 15 s in: the sea and sky darken, a haze closes in far from land." ;;
  dead-windfall) ON=(NIGHT_DEAD HORROR_AUDIO); EVENT="dead@+25"; AT_NIGHT=1; export WWHD_TEST_STAGE="sea 11 0"; NOTE="Night on Windfall: set down at its first spawn point
on foot, and at about 25 s the dead rise, closing in from behind Link." ;;
  stalker) ON=(PHANTOM_STALKER HORROR_AUDIO); EVENT="stalker@+25"; AT_NIGHT=1; RECORD=1; export WWHD_TEST_STAGE="${STAGE:-sea 11 0}"; NOTE="Night on Windfall: set down at its first spawn point
on foot, and at about 25 s fog rolls in and the Shadow Beast drops from the sky behind you (WWHD_MOD_PHANTOM_STALKER_CHOICE=1:
Phantom Ganon). It hunts you; fight it (about a dozen sword hits) or outlast it (2 minutes)." ;;
  helm) ON=(BIG_SHIP); NOTE="$SHIP
New: the wind sets her speed (before it the boat's speed, a third of it sailing into it), she gathers speed and loses it
slowly, and the wheel turns a rudder over (turning is gentle from a standstill)." ;;
  helmaroc) ON=(HELMAROC FAIR_WIND); EVENT="helmaroc@+20"; RECORD=1
  NOTE="Set sail (A). About 20 s in the wind dies; 30 s later the Helmaroc King comes down and circles, diving at you every
8-12 s. Shoot him (arrows, bombs, Boomerang): 60 health. Don't beat him and after ~100 s his next dive snatches the boat
and drops you on another square of the sea (open water, or ashore on its island)." ;;
  windfall) ON=(SHIP_UPGRADES WARP FAIR_WIND); export WWHD_TEST_WARP="Zunari"; NOTE="Warps to Windfall 5 s in, then on foot beside Zunari's stall: talk to him and his ship fittings open after.
F9 opens the warp list any time (Warp on)." ;;
  octo-phantom) ON=(BIG_OCTO PHANTOM_GANON HORROR_AUDIO RAFT FAIR_WIND SHIP_UPGRADES SECOND_GUNNER); EVENT="octo@+30;phantom@+240"
  AT_NIGHT=1; RECORD=1; export WWHD_TEST_UPGRADES=3F WWHD_MOD_PHANTOM_GANON_CHOICE=1
  NOTE="At night, the big raft in tow, upgrades maxed. About 30 s in, the sea goes still and dark; at 1 min a whirlpool opens
by the boat and Big Octo rises, five Gyorgs ramming. Shoot out its eyes (raft guns: B onto the raft, A by a gun, ZL to
lock on, again for the next target) or break free and sail off. At about 4 min Titanic Phantom Ganon." ;;
  tour) ON=(BEEDLE_SHIP SECOND_GUNNER BOARDING PHOTO_MODE DROWNED ARMADA WRONG_LION DEAD_CALM BIG_OCTO MAELSTROM CYCLONES BLOOD_MOON NIGHT_DEAD HORROR_AUDIO); export WWHD_TEST_CREW=1
  EVENT="drowned@+60;armada@+190;lion@+440;lion@+455;lion@+470;calm@+490;octo@+640;maelstrom@+910;cyclone@+1010;blood@+1170"; AT_NIGHT=1
  NOTE="$BEEDLE
Every new mod in one run, at night (about 20 minutes). Times are from when the file loads:
  0:00  Yours: the tow, get on the deck and back, both guns. F7 = free camera (F7 again to stop). A second controller
        is player 2 on the roof gun; the ship's crew fires whichever gun nobody mans.
  1:00  The drowned: be on the deck. ReDeads climb aboard for 2 minutes.
  3:10  Pirate armada: four cannon boats. On the deck, Bokoblins board from any that come close; sink all four for 200.
  7:20  Something in the Lion, three times: be in the boat, sailing. He laughs and swerves off by himself.
  8:10  Dead calm: the wind dies and a still fog settles.
 10:40  Big Octo: a whirlpool opens and Big Octo rises, Gyorgs around.
 15:10  Maelstrom: wrench the stick left-right to break free (or it takes two hearts after 45 s).
 16:50  Wandering cyclone: steer round it, then sail into it to be thrown across the sea.
 19:30  Blood moon: the world turns red; land on an island and the dead rise twice as fast.
Stay in open sea until the blood moon: going near an island ends whatever sea event is on." ;;
  tour-rest) ON=(BEEDLE_SHIP SECOND_GUNNER BOARDING PHOTO_MODE DROWNED ARMADA WRONG_LION DEAD_CALM BIG_OCTO MAELSTROM CYCLONES BLOOD_MOON NIGHT_DEAD HORROR_AUDIO); export WWHD_TEST_CREW=1
  EVENT="armada@+30;calm@+180;octo@+300;maelstrom@+600;cyclone@+700;blood@+870"; AT_NIGHT=1
  NOTE="$BEEDLE
The tour's second half, at night: armada (with boarding) at 0:30, calm 3:00, Big Octo 5:00, maelstrom 10:00,
cyclone 11:40, blood moon 14:30." ;;
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
  *) echo "Usage: tools/try.sh <seamless|windfall|stalker|buy-ship|helm|cabin|beedle-shop|helmaroc|octo-phantom|chaos|full|raft|shop|raft-bosses|tour|tour-rest|long|beedle|beedle-guns|gunner|crew|boarding|drowned|calm|photo|blood|cyclone|maelstrom|armada|octo|lion|soak|plain|beedle-bosses|all|bosses|duo|titanic-deck|deck|cannon|leviathan-deck|molgera-deck|phantom-deck|molgera-chase|phantom-titanic|anchor-island|seahats-swim|cannon-boat|cannon-boat-deck>"; exit 2 ;;
esac
for m in "${SPOOKY[@]}"; do export WWHD_MOD_$m=0; done
export WWHD_MOD_TURBO=1  # hold T to fast-forward (Claude: live pad bit 28)
for m in ${ON[@]+"${ON[@]}"}; do export WWHD_MOD_$m=1; [ -n "${OFTEN:-}" ] && export WWHD_MOD_${m}_OFTEN=$OFTEN; done
# the Big ship sails only once bought (Bomb Shop): the test runs own her, but for buy-ship
[[ " ${ON[*]-} " == *" BIG_SHIP "* && "${1:-}" != buy-ship ]] && export WWHD_TEST_SHIP_OWNED=1
[ -n "${AT_NIGHT:-}" ] && export WWHD_MOD_TIME=1 WWHD_MOD_TIME_CHOICE=4  # the clock held at 23:00
[ -n "$EVENT" ] && export WWHD_SPOOKY="$EVENT${EXTRA_EVENT:+;$EXTRA_EVENT}"  # (EXTRA_EVENT: more, e.g. "end-armada@+60")
echo "$NOTE"
echo
TRACE="$ROOT/build/try/${1}-$(date +%H%M%S).txt"; mkdir -p "$ROOT/build/try"
echo "It goes straight to the state by itself (no need to press anything at the title). Trace: $TRACE"
: > "$ROOT/build/try/pad.txt"  # Claude can drive the game live through it: "<buttons hex> <lx> <ly> [<rx> <ry>]"
# straight into the state: A pressed through the title and onto Quest Log 1 (as the audit routes do; the state loads
# once a Quest Log is played)
SKIP="WWHD_TEST_ORIGIN=1 WWHD_TEST_PRESS=2-2.15:8000,2.5-2.65:8000,3-3.15:8000,3.5-3.65:8000,4-4.15:8000,4.5-4.65:8000,5-5.15:8000,5.5-5.65:8000,6-6.15:8000,6.5-6.65:8000,7-7.15:8000,7.5-7.65:8000,8-8.15:8000,8.5-8.65:8000,9-9.15:8000,9.5-9.65:8000,10-10.15:8000,10.5-10.65:8000,11-11.15:8000,11.5-11.65:8000,12-12.15:8000"  # (every 0.5 s: each screen moves on as soon as it takes A)
mkdir -p "$ROOT/build/try/shots"
# RECORD=1 (chaos, full; or set it yourself): a screenshot every 3 s, shrunk into build/try/rec-<run>/ (tools/rec.sh)
if [ -n "${RECORD:-}" ]; then
  export WWHD_TEST_SHOT_EVERY=3; rm -f "$ROOT"/build/try/shots/*.png
  (sleep 5; "$ROOT/tools/rec.sh" "$ROOT/build/try/shots" "${TRACE%.txt}-rec") &
  echo "Recording: ${TRACE%.txt}-rec/"
fi
cd "$ROOT" && env $EXTRA $SKIP WWHD_SCREENSHOT_DIR="$ROOT/build/try/shots" WWHD_LIVE_PAD="$ROOT/build/try/pad.txt" WWHD_PORTABLE_LOAD="$ROOT/save/haunted.wwstate" WWHD_MODS_TRACE="$TRACE" ./build/cmake/wwhd

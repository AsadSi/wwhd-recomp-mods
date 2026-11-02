# Online co-op (experimental)

One player hosts their own Quest Log; up to three friends join it and play in the host's world. Everyone runs
their own copy of this port, built from the same commit (a different one is refused), with their own copy of
the game. It is work in progress: expect rough edges.

## How to play
- **Host:** Mods > Online co-op > Host (port 7777), switch it on, load your Quest Log.
- **Guest:** Mods > Online co-op > Join, enter the host's address (an IP, or a Tailscale name), switch it on,
  and load any Quest Log: the host's world comes in, and you are set down beside the host.
- **Play as** (Mods > Online co-op): Link, Tetra, Medli or Princess Zelda, for you and as the others see you.
- **Go to a player** (Mods > Online co-op, a button per player): you are taken beside them.
- The host's UDP port must be reachable from the guests (on a VPN such as Tailscale nothing else is needed).
- Two to four copies on one computer, for testing: `tools/duo.sh` (`PLAYERS=3 tools/duo.sh`; `tools/duo.sh stop`).

## What is shared
- **The other players** as Link in their own tunic (green, red, blue, purple), or as the character they play
  (Tetra, Medli or Princess Zelda), with their animations, sword and items, shown in the same room as you, and you
  hear their sword, steps and voices where they are (Tetra, Medli and Zelda in their own voices). Players push each
  other, and a player who is hit flashes red on everyone's screen. Doors they open, open for you too.
- **Their items:** you see what they hold, throw and fire (bombs, the boomerang, arrows, the hookshot's chain, the
  grappling hook's rope). Their bombs warn before they go off, then go off in your game too: they hurt you if you're
  close, and break what they reach.
- **Medli flies:** played as Medli, A in the air takes off and flaps; the others see her wings.
- **The story and items:** story events, chests, switches, pickups, small keys and dungeon items, sunken
  treasure, items, songs, pearls, the Triforce, charts and maps, heart containers and bag sizes. Your own
  health, magic, ammunition, bags and bottle contents stay yours.
- **Rupees:** what any player picks up, everyone gets. Each player still has their own wallet, and what you spend
  comes out of yours alone.
- **Tetra's ship:** the host sails, guests walk her deck and fire her cannon.
- **Boats and the raft:** everyone sails their own King of Red Lions and sees the others' boats; with the raft mod
  on, the host's boat tows the raft and everyone can stand on it.
- **Sea events and monsters:** the host's sea events, and every placed monster, the same on every screen. Anyone's
  hits count, and a monster killed stays dead for whoever comes later (until everyone has left that place).

## Limits
- A guest's saves are off while joined (the host saves). When the host leaves or the link drops, a guest goes
  back to the title screen.
- What the other player opens, unlocks or breaks (chests, locked doors, bomb walls, boulders, ladders, hammer pegs)
  changes for you too, even in the room you are in.
- The tunic colours are for the hero's clothes; in Link's casual clothes all players look alike.
- If a guest falls, they come back beside the host. If the host falls, everyone joins the host where it continues.
- A cutscene on the host's game plays on the guests in the same room too. Guests who are elsewhere are brought
  beside the host after it.
- Going into a house, cave or dungeon the host isn't in asks the host first (A lets you in, B sends you back beside
  them; no answer in 30 s counts as no).
- What you see of the others is about 60 ms behind them, plus the network's own delay.
- Friendly fire is on: a sword hit or an arrow on another player costs them half a heart (a heart for spin and jump attacks)
  and knocks them back, but never takes their last quarter heart.
- **Voice chat** (Mods > Online co-op > Voice chat): the others hear you when you're near them, louder the closer
  you are. Off until you switch it on; the first time, the system asks for the microphone.
- A tester has run an earlier version of this on Windows, on one computer and between two over a VPN: joining, the other players, chests, switches and the story worked, but both players being in the same interior crashed the game (not fixed yet). A Mac host and a Linux guest have joined in a test on one Mac. Everything new in this version has only been tested on one Mac.

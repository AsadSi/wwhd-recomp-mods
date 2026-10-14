# Online co-op (experimental)

One player hosts their own Quest Log; up to three friends join it and play in the host's world. Everyone runs
their own copy of this port, built from the same source (a different build is refused), with their own copy of
the game. It is work in progress: expect rough edges.

## How to play
- **Host:** Mods > Online co-op > Host (port 7777), switch it on, load your Quest Log.
- **Guest:** Mods > Online co-op > Join, enter the host's address (an IP, or a Tailscale name), switch it on,
  and load any Quest Log: the host's world comes in, and you are set down beside the host.
- The host's UDP port must be reachable from the guests (on a VPN such as Tailscale nothing else is needed).
- Two to four copies on one computer, for testing: `tools/duo.sh` (`PLAYERS=3 tools/duo.sh`; `tools/duo.sh stop`).

## What is shared
- **The other players** as Link in their own tunic (green, red, blue, purple), with their animations, sword and
  items, shown in the same room as you. Players push each other.
- **The story and items:** story events, chests, switches, pickups, small keys and dungeon items, sunken
  treasure, items, songs, pearls, the Triforce, charts and maps, heart containers and bag sizes. Your own
  health, rupees, magic, ammunition, bags and bottle contents stay yours.
- **Tetra's ship:** the host sails, guests walk her deck and fire her cannon.
- **Sea events and their monsters:** the host's events, with the host's monsters on everyone's screen.

## Limits
- A guest's saves are off while joined (the host saves). When the host leaves or the link drops, a guest goes
  back to the title screen.
- A chest the other player opens opens for you too, and a boulder they blow up is gone for you, even in the room
  you are in. Other things (a door they unlocked, a wall they bombed) change for you only once you enter that room
  again.
- The tunic colours are for the hero's clothes; in Link's casual clothes all players look alike.
- Not yet: cutscenes shared, doors that need the host's say, voice, characters other than Link, hits on
  players, monsters outside sea events (each player has their own).

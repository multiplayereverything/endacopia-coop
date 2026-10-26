# Endacopia Multiplayer (online co-op)

Two players share one Endacopia game over the network. You both see the same rooms,
dialogue, inventory and puzzle state, and either of you can click, talk, use items and open
menus. The other player's cursor shows up tinted, with a P1 or P2 tag. Co-op works from the
title screen to the ending.

## How it works

Endacopia runs on Adventure Game Studio 3.6.0.55. This mod is a multiplayer build of that
open-source engine (`bin\EndacopiaMP.exe`) and runs the game's own data from
`Windows\Endacopia.exe`. It changes no game files.

The host plays normally. Every frame, the host sends the guest a small record with both
players' input and every value the engine read from outside the game (audio playback
position, the clock, the save list). The guest runs the same game and replays those records,
so the two games stay identical frame by frame. Both sides compare a checksum of the game state
every frame. When a guest joins, or if the checksums ever differ, the host takes a savegame
snapshot and both players load it.

The guest gets a copy of the host's saves in `%LOCALAPPDATA%\EndacopiaMP\guest-saves`. The
guest's own saves stay untouched.

## Requirements

Both players need Endacopia on Steam (same version) and this `MultiplayerMod` folder inside
their Endacopia install folder.

The easiest way to connect is through Steam: you need to be Steam friends and have Steam
running, and nobody has to touch their router. Joining by IP also works: on a LAN nothing else
is needed; over the internet the host forwards TCP port 27960 on their router, or you both join
the same Tailscale or ZeroTier network and use that IP. The first time you host, Windows may ask
whether to let the program through the firewall; allow it.

## Playing

1. Double-click `Endacopia Multiplayer.bat`.
2. The host clicks **Host on Steam** (or **Host game** to use an IP address). The game starts
   as usual, so continue from a save or start a new one.
3. The friend clicks **Join Steam friend**. It finds a Steam friend who is hosting; if several
   are, type the host's Steam name in the box first. To join by IP instead, type the host's IP
   and click **Join game**.
4. The friend's screen shows "Connecting...", then "Downloading game state from host...", and
   then they're in the host's game. The first time takes longest (about 2 MB); after that only
   saves that changed are sent.
5. Play. Hover text follows whoever moved their mouse last, and a click always lands where the
   clicking player's cursor is.

Saving and loading use the game's own menus, and saves go to the host's save folder. When
either player loads a save, both load the host's copy.

Whoever clicks Quit or closes their window leaves, and the other keeps playing. A host whose
guest left can be rejoined at any time; a guest whose host left continues solo.

Alt+Enter toggles fullscreen for your own window only. Options changed in the game's own menu
(volume, fullscreen) apply to both players.

The guest's input reaches the game one network round trip late. On a LAN you won't notice;
online it equals your ping, which makes timing minigames a little harder for the guest.

## Troubleshooting

Logs are in `%LOCALAPPDATA%\EndacopiaMP\logs\last-session.log`; multiplayer lines start with `[MP]`.

"Version mismatch" or "Game data mismatch" means the two players have different builds of the
mod or different versions of the game.

If the guest sits on "Connecting..." or "Looking for a Steam friend...", check that the host is
already in the game and, for IP, the address, the port forward and the firewall. After 60
seconds the guest gives up and plays solo. If the connection dies mid-game, the guest notices
within 15 seconds and keeps playing solo.

This version (1.1) can't play with version 1.0; both players need the same zip.

## For developers

Source, build steps and the test harness: https://github.com/multiplayereverything/endacopia-coop

Command-line options: `--mp-host [port]`, `--mp-join host[:port]`, `--mp-name <name>`,
`--mp-guest-dir <dir>`, `--mp-control <port>`.

## Credits

Endacopia is by AndyL4nd. Adventure Game Studio is by Chris Jones and the AGS contributors
(Artistic License 2.0). The multiplayer changes were written with Claude, an AI assistant,
following the universal-modder workflow.

# endacopia-coop

Online co-op for [Endacopia](https://store.steampowered.com/app/2684630/) (Steam, AGS 3.6.0.55).
Two players share one game from the title screen to the ending: same rooms, dialogue,
inventory and puzzle state, and either player can act. Friends join through Steam (no port
forwarding) or by IP.

The mod is a modified Adventure Game Studio runtime that runs the game's own, unchanged data.
No game files are included here or edited on disk.

To play, download the zip from Releases and follow [launcher/README.md](launcher/README.md).

## How it works

Host-authoritative lockstep. Every frame the host sends a record with both players' input and
every value the engine read from outside the game (audio play state and position, the clock,
the save list). Guests replay the records and compare a state hash every frame. On join, or if
the hashes ever differ, the host takes a savegame snapshot, restores it itself, and guests load
the same bytes. The full list of non-deterministic reads and the bugs the tests found is in the
[coop-anything write-up](https://github.com/cranebeaker/coop-anything/tree/main/games/endacopia).

## Layout

| Path | What |
|---|---|
| `engine/` | AGS v3.6.0.55 (first commit is vanilla upstream) plus the netplay layer in `engine/Engine/net/` |
| `launcher/` | The launcher window (`Endacopia Multiplayer.bat` / `.ps1`) and the player README |
| `tools/mptest.js` | Test harness: runs a host and up to 3 guests on one PC, hidden and muted, driven by scenario files |
| `tools/scenarios/` | Scenarios: smoke, probe events, joins, Steam, 14 save files, five bad-network profiles |
| `tools/steam-mock/` | A fake Steam (broker plus `steam_api.dll` stand-in), because two Steam accounts can't run on one PC |
| `tools/twins.json` | Mutation twins: each one undoes a fix and names the scenario that must then fail |

## Build

Visual Studio 2022 (MSVC 17.14), CMake 4, Win32:

```bash
cmake -S engine -B build -A Win32 -DAGS_BUILD_TOOLS=OFF -DAGS_TESTS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

```bash
cmake --build build --config Release --target ags
```

Copy `build/Release/ags.exe` to `<game dir>/MultiplayerMod/bin/EndacopiaMP.exe` next to the
launcher files. Configure a second folder, `build-mut`, with `-DAGS_MP_MUTANTS=ON` for the twin
build.

## Test

The harness needs a test copy of the install and never writes to it. Hardlink one into
`game-test/` (`Windows/` and `Data/` from `<game dir>`), or point `MP_GAME_DIR` at a copy's
`Windows` folder. The save scenarios load `%LOCALAPPDATA%/EndacopiaMP/lab-host/<save>`; copy
your own saves there.

```bash
node tools/mptest.js tools/scenarios/smoke.txt
```

It prints `RESULT: PASS` or `RESULT: FAIL`. `node tools/mptest.js --selftest` feeds the scorer
known-bad input and must fail. `bash tools/run_suite.sh` runs everything; the bad-network
profiles and twins also need a clone of
[coop-anything](https://github.com/cranebeaker/coop-anything) in `coop-anything/` and Python
(set `MP_PYTHON` if it isn't on `PATH`). The Steam scenario needs `tools/steam-mock` built with
CMake.

Last full run on the release build: 23 of 23 scenarios pass; 8 of 11 twins are caught.

## License

The engine is Adventure Game Studio by Chris Jones and the AGS contributors, under the Artistic
License 2.0 (`engine/License.txt`). This is a modified version; every change is in the `engine/`
history. Endacopia is by AndyL4nd and is not included. The netplay code and tools were written
with Claude, an AI assistant.

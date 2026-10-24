//=============================================================================
//
// Endacopia Multiplayer (AGS-MP): host-authoritative deterministic netplay.
//
// The host runs the game normally. Each engine frame (WaitForNextFrame) it
// builds a TickRecord with the merged input of both players plus every
// non-deterministic value the engine read during that frame ("oracles").
// The guest runs the same game data and replays the records exactly, so both
// simulations stay identical. A per-frame state hash detects drift; on drift
// (and on join) the host takes a savegame snapshot, restores it itself and
// sends it to the guest, who restores the same bytes.
//
//=============================================================================
#ifndef __AGS_EE_NET__NETPLAY_H
#define __AGS_EE_NET__NETPLAY_H

#include <stdint.h>
#include <vector>
#include "util/string.h"

union SDL_Event;

namespace NetPlay
{

enum Role
{
    kRoleNone,
    kRoleHost,
    kRoleGuest
};

// Oracle kinds: identifies a non-deterministic read; guest validates the
// kind sequence to detect desyncs early.
enum OracleKind : uint8_t
{
    kOr_AudioState = 1,
    kOr_AudioPos,
    kOr_AudioPlay,
    kOr_AudioPause,
    kOr_Time,
    kOr_IgnoreInput,
    kOr_MusicUpdate,
    kOr_SysWindowed,
    kOr_SysVsync,
    kOr_SysGamma,
    kOr_SaveList,
    kOr_SaveFile,
    kOr_SysVolume,
    kOr_Misc
};

// Parses --mp-* command line options; returns true if the arg was consumed.
// 'ee' is advanced past consumed parameters.
bool ParseCmdArg(int argc, char *argv[], int &ee);
// Guest: separate user data dir that receives the host's mirrored saves
AGS::Common::String GetGuestDataDir();
// Called once the engine is initialized, before the game starts.
void Startup();
void Shutdown();

Role GetRole();
// Netplay drives input and oracles (both peers are in lockstep)
bool IsSynced();
// Input must not be applied from local SDL events directly
bool IsInputRouted();
// Guest: true if the oracle stream must be consumed from the host
bool IsReplaying();
// Host: true if oracles are being recorded
bool IsRecording();

// --- Input ---
// Called by sys_evt_process_pending for every SDL event; returns true if the
// event was captured by netplay and must NOT be passed to the engine.
bool CaptureEvent(const SDL_Event &evt);
// Logical (synced) mouse position, in native game coords
void GetLogicalMouse(int &x, int &y);
void SetLogicalMouse(int x, int y);
// Synced key state for IsKeyPressed
bool IsScancodeDown(int scancode);

// --- Frame boundary ---
// Called at the end of WaitForNextFrame()
void OnFrameBoundary();
// Guest: tells WaitForNextFrame to not sleep (catching up on backlog)
bool ShouldSkipFrameWait();
// Called from the outermost game loop, when no script is blocking
void OnIdlePoint();
// Guest: called before the game starts; blocks until a join snapshot is
// received. Returns the path to the savegame to load (empty if no netplay).
AGS::Common::String GuestWaitForJoin(int &slot);
// Called after a join/resync snapshot was restored (both peers)
void OnSnapshotRestored();

// --- Oracles ---
int  OracleInt(OracleKind kind, int local_value);
// Host: records 'data'; guest: replaces 'data' with host's value
void OracleBlob(OracleKind kind, std::vector<uint8_t> &data);

// --- Rendering ---
// Local player's cursor position (game coords); returns false if netplay
// should not change cursor drawing.
bool GetLocalCursor(int &x, int &y);
// Remote player's cursor; returns false if not available
bool GetRemoteCursor(int &x, int &y);
// Short status line for HUD (empty if nothing to show)
AGS::Common::String GetHudText();

// Desync diagnostics
void ComputeStateHash(uint32_t out[6]);

} // namespace NetPlay

#endif // __AGS_EE_NET__NETPLAY_H

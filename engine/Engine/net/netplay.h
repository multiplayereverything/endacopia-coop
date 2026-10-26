// Endacopia Multiplayer: the host streams each frame's input plus every non-deterministic read ("oracles"),
// the guest replays them; on join or a state-hash mismatch both peers restore the same savegame snapshot.
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

// The guest checks the kind sequence, so a diverged code path is caught on the first mismatched read.
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

// Returns true if argv[ee] was an --mp-* option; advances 'ee' past its parameters.
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

// Returns true if netplay captured the event and the engine must not process it.
bool CaptureEvent(const SDL_Event &evt);
// Logical (synced) mouse position, in native game coords
void GetLogicalMouse(int &x, int &y);
void SetLogicalMouse(int x, int y);
// Synced key state for IsKeyPressed
bool IsScancodeDown(int scancode);
// True if the most recent click/key in the synced stream came from this peer
bool LocalPlayerActedLast();

// Called at the end of WaitForNextFrame()
void OnFrameBoundary();
// Guest: tells WaitForNextFrame to not sleep (catching up on backlog)
bool ShouldSkipFrameWait();
// Called from the outermost game loop, when no script is blocking
void OnIdlePoint();
// Guest: blocks until the join snapshot arrives; returns its savegame path, or empty to play solo.
AGS::Common::String GuestWaitForJoin(int &slot);
// Guest: restores the join snapshot exactly the way the host restored it.
void RestoreJoinSnapshot(const AGS::Common::String &path, int slot);
// Called after a join/resync snapshot was restored (both peers)
void OnSnapshotRestored();

int  OracleInt(OracleKind kind, int local_value);
// Host: records 'data'; guest: replaces 'data' with host's value
void OracleBlob(OracleKind kind, std::vector<uint8_t> &data);

// Local player's real cursor in game coords; false outside co-op.
bool GetLocalCursor(int &x, int &y);
// Remote player's cursor; returns false if not available
bool GetRemoteCursor(int &x, int &y);
// Short status line for HUD (empty if nothing to show)
AGS::Common::String GetHudText();

void ComputeStateHash(uint32_t out[6]);

} // namespace NetPlay

#endif // __AGS_EE_NET__NETPLAY_H

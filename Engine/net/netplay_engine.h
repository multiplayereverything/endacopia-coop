// Engine-side helpers for the netplay core; the only netplay unit that touches engine internals.
#ifndef __AGS_EE_NET__NETPLAY_ENGINE_H
#define __AGS_EE_NET__NETPLAY_ENGINE_H

#include <stdint.h>
#include <vector>
#include "util/string.h"

union SDL_Event;

namespace NetPlayEngine
{
    using AGS::Common::String;

    // Input application (deterministic, called on both peers)
    void PushKeyEvent(const SDL_Event &evt);
    void SetMouseButtons(int state, int accum);
    void AddMouseWheel(int dz);
    void ResetInputState();
    // Converts window coords to native game coords (clamped to game viewport)
    void WindowToGame(int &x, int &y);
    // Pushes a synthetic event into the SDL queue (vanilla mode automation)
    void InjectVanillaMouse(int gx, int gy, int button, bool down, bool move_only);
    void InjectVanillaKey(int scancode, int sym, int mod, bool down);
    void InjectVanillaText(const char *text);
    // Local-only service actions
    void ToggleFullscreen();
    // Draws only the engine overlay (status HUD), used while the guest waits to join.
    void RenderWaitScreen();

    String GetGameGuid();
    String GetTranslationName();
    void   ChangeTranslation(const String &name);
    String GetUserDataDir();
    String GetAppDataDir();
    bool   IsIdle();         // outermost loop, no script running, room loaded
    bool   IsGameLoaded();   // a room is displayed
    void   CancelScripts();
    String DescribeState();  // for automation "state"
    void   SaveScreenshot(const String &path);
    int    GetCurrentFrameRate();
    String DescribeMisc();   // misc hash fields, for desync diagnostics

    bool MakeSnapshot(const String &tmp_path, std::vector<uint8_t> &bytes);
    void RestoreSnapshot(const String &path, int slot);

    // Six component hashes so a desync report says which part of the state diverged.
    void ComputeStateHash(uint32_t out[6]);
}

#endif // __AGS_EE_NET__NETPLAY_ENGINE_H

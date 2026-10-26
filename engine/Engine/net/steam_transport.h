// Netplay over Steam: friends-only lobbies to find each other, Steam P2P (relayed by Valve) to talk.
// Uses the steam_api.dll Endacopia ships (flat API, SDK ~1.42), loaded at run time, so the game's own
// Steam app id is used and nothing extra is downloaded. Call results are polled; no callbacks.
#ifndef __AGS_EE_NET__STEAM_TRANSPORT_H
#define __AGS_EE_NET__STEAM_TRANSPORT_H

#include <stdint.h>
#include <vector>
#include "util/string.h"

namespace SteamNet
{
    using AGS::Common::String;

    // Loads the API from the already-loaded steam_api.dll, or from dll_path (tests use a fake Steam).
    bool Init(const String &dll_path);
    bool IsReady();
    String LastError();
    uint64_t MyId();
    String MyName();
    String NameOf(uint64_t user);

    // Lobbies. Create/Join are asynchronous: call Poll() and check LobbyId().
    void CreateLobby(int max_members);
    void JoinLobby(uint64_t lobby);
    void LeaveLobby();
    uint64_t LobbyId();          // 0 until created/joined
    bool LobbyFailed();          // the last create/join failed
    uint64_t LobbyOwner();
    std::vector<uint64_t> LobbyMembers();

    struct FriendLobby
    {
        uint64_t user = 0;
        uint64_t lobby = 0;
        String name;
    };
    // Friends who are running this game inside a lobby.
    std::vector<FriendLobby> FindFriendLobbies();

    // Reliable, ordered P2P byte stream per peer (channel 0).
    void Accept(uint64_t peer);
    bool Send(uint64_t peer, const uint8_t *data, size_t len);
    bool Receive(std::vector<uint8_t> &data, uint64_t &from);
    void Close(uint64_t peer);

    void Poll();
}

#endif // __AGS_EE_NET__STEAM_TRANSPORT_H

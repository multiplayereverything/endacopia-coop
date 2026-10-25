#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include "net/steam_transport.h"
#include "debug/out.h"

using namespace AGS::Common;

namespace SteamNet
{

// Flat API of the Steamworks SDK the game ships (1.4x): every interface call takes the interface
// pointer first, CSteamID travels as uint64, everything is __cdecl.
typedef intptr_t Iface;
typedef uint64_t SteamAPICall;

// Interface versions of that SDK. steamclient.dll serves every historical version, so these keep working.
static const char *V_CLIENT = "SteamClient017";
static const char *V_USER = "SteamUser019";
static const char *V_FRIENDS = "SteamFriends015";
static const char *V_UTILS = "SteamUtils009";
static const char *V_MATCHMAKING = "SteamMatchMaking009";
static const char *V_NETWORKING = "SteamNetworking005";

enum { kLobbyFriendsOnly = 1, kP2PReliable = 2, kFriendFlagImmediate = 4 };
enum { kCb_LobbyEnter = 504, kCb_LobbyCreated = 513 };
static const uint32_t MAX_PACKET = 256 * 1024;

// Callback payloads as Steam lays them out on Windows (8-byte packing).
#pragma pack(push, 8)
struct LobbyCreated { int32_t result; uint64_t lobby; };
struct LobbyEnter { uint64_t lobby; uint32_t chat_permissions; bool locked; uint32_t response; };
struct FriendGameInfo { uint64_t game_id; uint32_t ip; uint16_t port, query_port; uint64_t lobby; };
#pragma pack(pop)

struct Api
{
    HMODULE dll = nullptr;
    Iface (__cdecl *SteamClient)() = nullptr;
    int32_t (__cdecl *GetHSteamUser)() = nullptr;
    int32_t (__cdecl *GetHSteamPipe)() = nullptr;
    void (__cdecl *RunCallbacks)() = nullptr;
    Iface (__cdecl *GetUser)(Iface, int32_t, int32_t, const char*) = nullptr;
    Iface (__cdecl *GetFriends)(Iface, int32_t, int32_t, const char*) = nullptr;
    Iface (__cdecl *GetUtils)(Iface, int32_t, const char*) = nullptr;
    Iface (__cdecl *GetMatchmaking)(Iface, int32_t, int32_t, const char*) = nullptr;
    Iface (__cdecl *GetNetworking)(Iface, int32_t, int32_t, const char*) = nullptr;
    uint64_t (__cdecl *GetSteamID)(Iface) = nullptr;
    const char *(__cdecl *GetPersonaName)(Iface) = nullptr;
    const char *(__cdecl *GetFriendPersonaName)(Iface, uint64_t) = nullptr;
    int (__cdecl *GetFriendCount)(Iface, int) = nullptr;
    uint64_t (__cdecl *GetFriendByIndex)(Iface, int, int) = nullptr;
    bool (__cdecl *GetFriendGamePlayed)(Iface, uint64_t, FriendGameInfo*) = nullptr;
    bool (__cdecl *SetRichPresence)(Iface, const char*, const char*) = nullptr;
    uint32_t (__cdecl *GetAppID)(Iface) = nullptr;
    bool (__cdecl *IsAPICallCompleted)(Iface, SteamAPICall, bool*) = nullptr;
    bool (__cdecl *GetAPICallResult)(Iface, SteamAPICall, void*, int, int, bool*) = nullptr;
    SteamAPICall (__cdecl *CreateLobby)(Iface, int, int) = nullptr;
    SteamAPICall (__cdecl *JoinLobby)(Iface, uint64_t) = nullptr;
    void (__cdecl *LeaveLobby)(Iface, uint64_t) = nullptr;
    bool (__cdecl *SetLobbyData)(Iface, uint64_t, const char*, const char*) = nullptr;
    bool (__cdecl *SetLobbyJoinable)(Iface, uint64_t, bool) = nullptr;
    int (__cdecl *GetNumLobbyMembers)(Iface, uint64_t) = nullptr;
    uint64_t (__cdecl *GetLobbyMemberByIndex)(Iface, uint64_t, int) = nullptr;
    uint64_t (__cdecl *GetLobbyOwner)(Iface, uint64_t) = nullptr;
    bool (__cdecl *SendP2PPacket)(Iface, uint64_t, const void*, uint32_t, int, int) = nullptr;
    bool (__cdecl *IsP2PPacketAvailable)(Iface, uint32_t*, int) = nullptr;
    bool (__cdecl *ReadP2PPacket)(Iface, void*, uint32_t, uint32_t*, uint64_t*, int) = nullptr;
    bool (__cdecl *AcceptP2PSessionWithUser)(Iface, uint64_t) = nullptr;
    bool (__cdecl *CloseP2PSessionWithUser)(Iface, uint64_t) = nullptr;
    bool (__cdecl *AllowP2PPacketRelay)(Iface, bool) = nullptr;

    Iface user = 0, friends = 0, utils = 0, mm = 0, net = 0;
} static api;

static String last_error;
static bool ready = false;
static uint64_t lobby = 0;
static bool lobby_failed = false;
static SteamAPICall pending = 0;
static int pending_kind = 0;

template <typename T>
static bool Load(T &fn, const char *name)
{
    fn = reinterpret_cast<T>(GetProcAddress(api.dll, name));
    if (!fn) last_error.Format("steam_api is missing %s", name);
    return fn != nullptr;
}

bool Init(const String &dll_path)
{
    if (ready) return true;
    api.dll = dll_path.IsEmpty() ? GetModuleHandleA("steam_api.dll") : LoadLibraryA(dll_path.GetCStr());
    if (!api.dll && dll_path.IsEmpty())
        api.dll = LoadLibraryA("steam_api.dll");
    if (!api.dll) { last_error = "steam_api.dll not found"; return false; }
    bool ok = Load(api.SteamClient, "SteamClient") && Load(api.GetHSteamUser, "SteamAPI_GetHSteamUser") &&
        Load(api.GetHSteamPipe, "SteamAPI_GetHSteamPipe") && Load(api.RunCallbacks, "SteamAPI_RunCallbacks") &&
        Load(api.GetUser, "SteamAPI_ISteamClient_GetISteamUser") && Load(api.GetFriends, "SteamAPI_ISteamClient_GetISteamFriends") &&
        Load(api.GetUtils, "SteamAPI_ISteamClient_GetISteamUtils") && Load(api.GetMatchmaking, "SteamAPI_ISteamClient_GetISteamMatchmaking") &&
        Load(api.GetNetworking, "SteamAPI_ISteamClient_GetISteamNetworking") && Load(api.GetSteamID, "SteamAPI_ISteamUser_GetSteamID") &&
        Load(api.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName") && Load(api.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName") &&
        Load(api.GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount") && Load(api.GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex") &&
        Load(api.GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed") && Load(api.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence") &&
        Load(api.GetAppID, "SteamAPI_ISteamUtils_GetAppID") && Load(api.IsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted") &&
        Load(api.GetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult") && Load(api.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby") &&
        Load(api.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby") && Load(api.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby") &&
        Load(api.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData") && Load(api.SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable") &&
        Load(api.GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers") &&
        Load(api.GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex") &&
        Load(api.GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner") && Load(api.SendP2PPacket, "SteamAPI_ISteamNetworking_SendP2PPacket") &&
        Load(api.IsP2PPacketAvailable, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable") && Load(api.ReadP2PPacket, "SteamAPI_ISteamNetworking_ReadP2PPacket") &&
        Load(api.AcceptP2PSessionWithUser, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser") &&
        Load(api.CloseP2PSessionWithUser, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser") &&
        Load(api.AllowP2PPacketRelay, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
    if (!ok) return false;

    // The agsteam plugin normally initialised the API in this process already; we only borrow its pipe.
    if (!api.GetHSteamPipe())
    {
        bool (__cdecl *init)() = reinterpret_cast<bool (__cdecl *)()>(GetProcAddress(api.dll, "SteamAPI_Init"));
        if (!init || !init()) { last_error = "Steam is not running or not logged in"; return false; }
    }
    const Iface client = api.SteamClient();
    const int32_t user = api.GetHSteamUser(), pipe = api.GetHSteamPipe();
    if (!client || !pipe) { last_error = "Steam is not running or not logged in"; return false; }
    api.user = api.GetUser(client, user, pipe, V_USER);
    api.friends = api.GetFriends(client, user, pipe, V_FRIENDS);
    api.utils = api.GetUtils(client, pipe, V_UTILS);
    api.mm = api.GetMatchmaking(client, user, pipe, V_MATCHMAKING);
    api.net = api.GetNetworking(client, user, pipe, V_NETWORKING);
    if (!api.user || !api.friends || !api.utils || !api.mm || !api.net)
    {
        last_error = "Steam did not provide its interfaces";
        return false;
    }
    (void)V_CLIENT;
    api.AllowP2PPacketRelay(api.net, true);
    ready = true;
    Debug::Printf(kDbgMsg_Info, "[MP] Steam ready: %s (app %u)", MyName().GetCStr(), api.GetAppID(api.utils));
    return true;
}

bool IsReady() { return ready; }
String LastError() { return last_error; }
uint64_t MyId() { return ready ? api.GetSteamID(api.user) : 0; }
String MyName() { return ready ? String(api.GetPersonaName(api.friends)) : String(); }
String NameOf(uint64_t u) { return ready ? String(api.GetFriendPersonaName(api.friends, u)) : String(); }

void CreateLobby(int max_members)
{
    lobby = 0;
    lobby_failed = false;
    pending = api.CreateLobby(api.mm, kLobbyFriendsOnly, max_members);
    pending_kind = kCb_LobbyCreated;
}

void JoinLobby(uint64_t id)
{
    lobby = 0;
    lobby_failed = false;
    pending = api.JoinLobby(api.mm, id);
    pending_kind = kCb_LobbyEnter;
}

void LeaveLobby()
{
    if (lobby) api.LeaveLobby(api.mm, lobby);
    if (ready) api.SetRichPresence(api.friends, "connect", "");
    lobby = 0;
}

uint64_t LobbyId() { return lobby; }
bool LobbyFailed() { return lobby_failed; }
uint64_t LobbyOwner() { return lobby ? api.GetLobbyOwner(api.mm, lobby) : 0; }

std::vector<uint64_t> LobbyMembers()
{
    std::vector<uint64_t> out;
    if (!lobby) return out;
    const int n = api.GetNumLobbyMembers(api.mm, lobby);
    for (int i = 0; i < n; ++i)
        out.push_back(api.GetLobbyMemberByIndex(api.mm, lobby, i));
    return out;
}

std::vector<FriendLobby> FindFriendLobbies()
{
    std::vector<FriendLobby> out;
    if (!ready) return out;
    const uint32_t app = api.GetAppID(api.utils);
    const int n = api.GetFriendCount(api.friends, kFriendFlagImmediate);
    for (int i = 0; i < n; ++i)
    {
        const uint64_t f = api.GetFriendByIndex(api.friends, i, kFriendFlagImmediate);
        FriendGameInfo info = {};
        if (!api.GetFriendGamePlayed(api.friends, f, &info)) continue;
        if ((info.game_id & 0xFFFFFF) != app || info.lobby == 0) continue;  // low 24 bits of a game id are the app id
        FriendLobby fl;
        fl.user = f;
        fl.lobby = info.lobby;
        fl.name = NameOf(f);
        out.push_back(fl);
    }
    return out;
}

void Accept(uint64_t peer) { if (ready) api.AcceptP2PSessionWithUser(api.net, peer); }
void Close(uint64_t peer) { if (ready) api.CloseP2PSessionWithUser(api.net, peer); }

bool Send(uint64_t peer, const uint8_t *data, size_t len)
{
    for (size_t at = 0; at < len; at += MAX_PACKET)
    {
        const uint32_t n = (uint32_t)std::min<size_t>(MAX_PACKET, len - at);
        if (!api.SendP2PPacket(api.net, peer, data + at, n, kP2PReliable, 0))
            return false;
    }
    return true;
}

bool Receive(std::vector<uint8_t> &data, uint64_t &from)
{
    uint32_t size = 0;
    if (!ready || !api.IsP2PPacketAvailable(api.net, &size, 0)) return false;
    data.resize(size);
    uint32_t got = 0;
    if (!api.ReadP2PPacket(api.net, data.data(), size, &got, &from, 0)) return false;
    data.resize(got);
    return true;
}

void Poll()
{
    if (!ready) return;
    api.RunCallbacks();
    if (!pending) return;
    bool failed = false;
    if (!api.IsAPICallCompleted(api.utils, pending, &failed)) return;
    if (pending_kind == kCb_LobbyCreated)
    {
        LobbyCreated r = {};
        if (!failed && api.GetAPICallResult(api.utils, pending, &r, sizeof(r), kCb_LobbyCreated, &failed) && !failed && r.result == 1)
        {
            lobby = r.lobby;
            api.SetLobbyData(api.mm, lobby, "endacopia_mp", "1");
            api.SetLobbyJoinable(api.mm, lobby, true);
            // Lets friends use Steam's own "Join Game" while the mod is running.
            api.SetRichPresence(api.friends, "connect", String::FromFormat("+connect_lobby %llu", (unsigned long long)lobby).GetCStr());
            Debug::Printf(kDbgMsg_Info, "[MP] Steam lobby %llu created", (unsigned long long)lobby);
        }
        else
        {
            lobby_failed = true;
            last_error.Format("could not create a Steam lobby (result %d)", r.result);
        }
    }
    else if (pending_kind == kCb_LobbyEnter)
    {
        LobbyEnter r = {};
        if (!failed && api.GetAPICallResult(api.utils, pending, &r, sizeof(r), kCb_LobbyEnter, &failed) && !failed && r.response == 1)
        {
            lobby = r.lobby;
            Debug::Printf(kDbgMsg_Info, "[MP] joined Steam lobby %llu", (unsigned long long)lobby);
        }
        else
        {
            lobby_failed = true;
            last_error.Format("could not join the Steam lobby (response %u)", r.response);
        }
    }
    pending = 0;
}

} // namespace SteamNet

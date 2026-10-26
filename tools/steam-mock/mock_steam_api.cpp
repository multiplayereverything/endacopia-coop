// Fake steam_api.dll for one-PC tests of the Steam transport (net/steam_transport.cpp).
// Exports the flat-API functions the transport uses and talks to tools/steam-mock/broker.py.
// Identity comes from MOCK_STEAM_ID / MOCK_STEAM_NAME, the broker port from MOCK_STEAM_PORT (default 27990).
// Like real Steam, packets from a user we have not accepted (or sent to) are held until AcceptP2PSessionWithUser.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define API extern "C" __declspec(dllexport)
typedef intptr_t Iface;
typedef uint64_t SteamAPICall;

enum { HELLO = 1, USERS, CREATE_LOBBY, JOIN_LOBBY, MEMBERS, LEAVE, SEND, PACKET = 100 };
static const uint32_t APP_ID = 2684630;

#pragma pack(push, 8)
struct LobbyCreated { int32_t result; uint64_t lobby; };
struct LobbyEnter { uint64_t lobby; uint32_t chat_permissions; bool locked; uint32_t response; };
struct FriendGameInfo { uint64_t game_id; uint32_t ip; uint16_t port, query_port; uint64_t lobby; };
#pragma pack(pop)

struct User { uint64_t id; uint64_t lobby; std::string name; };

static SOCKET sock = INVALID_SOCKET;
static std::mutex req_mx, mx;
static std::condition_variable cv;
static std::map<int, std::vector<uint8_t>> replies;
static std::deque<std::pair<uint64_t, std::vector<uint8_t>>> rx;
static std::map<uint64_t, std::deque<std::vector<uint8_t>>> held;
static std::set<uint64_t> accepted;
static uint64_t my_id = 0;
static std::string my_name;
static std::vector<User> users;
static DWORD users_at = 0;
static std::map<SteamAPICall, std::vector<uint8_t>> call_results;
static SteamAPICall next_call = 1;
static std::string name_buf;

static bool read_all(void *p, int n)
{
    char *c = (char*)p;
    while (n > 0) { int g = recv(sock, c, n, 0); if (g <= 0) return false; c += g; n -= g; }
    return true;
}

static void reader()
{
    for (;;)
    {
        uint8_t hdr[5];
        if (!read_all(hdr, 5)) return;
        uint32_t len; memcpy(&len, hdr, 4);
        std::vector<uint8_t> body(len - 1);
        if (len > 1 && !read_all(body.data(), (int)body.size())) return;
        std::lock_guard<std::mutex> lk(mx);
        if (hdr[4] == PACKET)
        {
            uint64_t from; memcpy(&from, body.data(), 8);
            std::vector<uint8_t> data(body.begin() + 8, body.end());
            if (accepted.count(from)) rx.emplace_back(from, std::move(data));
            else held[from].push_back(std::move(data));
        }
        else
        {
            replies[hdr[4]] = std::move(body);
            cv.notify_all();
        }
    }
}

static void send_frame(int op, const std::vector<uint8_t> &payload)
{
    uint32_t len = (uint32_t)payload.size() + 1;
    std::vector<uint8_t> f(5);
    memcpy(f.data(), &len, 4);
    f[4] = (uint8_t)op;
    f.insert(f.end(), payload.begin(), payload.end());
    send(sock, (const char*)f.data(), (int)f.size(), 0);
}

static std::vector<uint8_t> request(int op, const std::vector<uint8_t> &payload)
{
    std::lock_guard<std::mutex> rl(req_mx);
    send_frame(op, payload);
    std::unique_lock<std::mutex> lk(mx);
    cv.wait_for(lk, std::chrono::seconds(10), [&] { return replies.count(op) != 0; });
    auto it = replies.find(op);
    if (it == replies.end()) return {};
    std::vector<uint8_t> r = std::move(it->second);
    replies.erase(it);
    return r;
}

template <typename T> static void put(std::vector<uint8_t> &v, T x) { uint8_t b[sizeof(T)]; memcpy(b, &x, sizeof(T)); v.insert(v.end(), b, b + sizeof(T)); }
template <typename T> static T get(const std::vector<uint8_t> &v, size_t &at) { T x{}; if (at + sizeof(T) <= v.size()) memcpy(&x, &v[at], sizeof(T)); at += sizeof(T); return x; }

static bool connect_broker()
{
    if (sock != INVALID_SOCKET) return true;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    const char *id = getenv("MOCK_STEAM_ID"), *name = getenv("MOCK_STEAM_NAME"), *port = getenv("MOCK_STEAM_PORT");
    my_id = id ? _strtoui64(id, nullptr, 10) : 76561190000000001ull;
    my_name = name ? name : "mock";
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)(port ? atoi(port) : 27990));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(sock, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(sock); sock = INVALID_SOCKET; return false; }
    std::thread(reader).detach();
    std::vector<uint8_t> p; put(p, my_id); put(p, (uint32_t)my_name.size());
    p.insert(p.end(), my_name.begin(), my_name.end());
    request(HELLO, p);
    return true;
}

static void refresh_users()
{
    if (GetTickCount() - users_at < 200 && users_at) return;
    users_at = GetTickCount();
    std::vector<uint8_t> r = request(USERS, {});
    size_t at = 0;
    uint32_t n = get<uint32_t>(r, at);
    users.clear();
    for (uint32_t i = 0; i < n; ++i)
    {
        User u; u.id = get<uint64_t>(r, at); u.lobby = get<uint64_t>(r, at);
        uint32_t ln = get<uint32_t>(r, at);
        if (at + ln <= r.size()) u.name.assign((const char*)&r[at], ln);
        at += ln;
        users.push_back(u);
    }
}

static SteamAPICall store_result(const void *p, size_t n)
{
    SteamAPICall c = next_call++;
    call_results[c] = std::vector<uint8_t>((const uint8_t*)p, (const uint8_t*)p + n);
    return c;
}

API Iface __cdecl SteamClient() { return connect_broker() ? 1 : 0; }
API bool __cdecl SteamAPI_Init() { return connect_broker(); }
API int32_t __cdecl SteamAPI_GetHSteamUser() { return 1; }
API int32_t __cdecl SteamAPI_GetHSteamPipe() { return sock != INVALID_SOCKET ? 1 : 0; }
API void __cdecl SteamAPI_RunCallbacks() {}
API Iface __cdecl SteamAPI_ISteamClient_GetISteamUser(Iface, int32_t, int32_t, const char*) { return 2; }
API Iface __cdecl SteamAPI_ISteamClient_GetISteamFriends(Iface, int32_t, int32_t, const char*) { return 3; }
API Iface __cdecl SteamAPI_ISteamClient_GetISteamUtils(Iface, int32_t, const char*) { return 4; }
API Iface __cdecl SteamAPI_ISteamClient_GetISteamMatchmaking(Iface, int32_t, int32_t, const char*) { return 5; }
API Iface __cdecl SteamAPI_ISteamClient_GetISteamNetworking(Iface, int32_t, int32_t, const char*) { return 6; }
API uint64_t __cdecl SteamAPI_ISteamUser_GetSteamID(Iface) { return my_id; }
API const char *__cdecl SteamAPI_ISteamFriends_GetPersonaName(Iface) { return my_name.c_str(); }
API const char *__cdecl SteamAPI_ISteamFriends_GetFriendPersonaName(Iface, uint64_t f)
{
    refresh_users();
    name_buf = "[unknown]";
    for (auto &u : users) if (u.id == f) name_buf = u.name;
    return name_buf.c_str();
}
API int __cdecl SteamAPI_ISteamFriends_GetFriendCount(Iface, int) { refresh_users(); return (int)users.size(); }
API uint64_t __cdecl SteamAPI_ISteamFriends_GetFriendByIndex(Iface, int i, int) { refresh_users(); return i >= 0 && i < (int)users.size() ? users[i].id : 0; }
API bool __cdecl SteamAPI_ISteamFriends_GetFriendGamePlayed(Iface, uint64_t f, FriendGameInfo *info)
{
    refresh_users();
    for (auto &u : users)
        if (u.id == f) { *info = FriendGameInfo(); info->game_id = APP_ID; info->lobby = u.lobby; return true; }
    return false;
}
API bool __cdecl SteamAPI_ISteamFriends_SetRichPresence(Iface, const char*, const char*) { return true; }
API uint32_t __cdecl SteamAPI_ISteamUtils_GetAppID(Iface) { return APP_ID; }
API bool __cdecl SteamAPI_ISteamUtils_IsAPICallCompleted(Iface, SteamAPICall c, bool *failed) { if (failed) *failed = false; return call_results.count(c) != 0; }
API bool __cdecl SteamAPI_ISteamUtils_GetAPICallResult(Iface, SteamAPICall c, void *out, int size, int, bool *failed)
{
    auto it = call_results.find(c);
    if (it == call_results.end() || (int)it->second.size() > size) { if (failed) *failed = true; return false; }
    memcpy(out, it->second.data(), it->second.size());
    call_results.erase(it);
    if (failed) *failed = false;
    return true;
}
API SteamAPICall __cdecl SteamAPI_ISteamMatchmaking_CreateLobby(Iface, int, int max_members)
{
    std::vector<uint8_t> p; put(p, (uint32_t)max_members);
    std::vector<uint8_t> r = request(CREATE_LOBBY, p);
    size_t at = 0;
    LobbyCreated lc = {}; lc.lobby = get<uint64_t>(r, at); lc.result = lc.lobby ? 1 : 2;
    users_at = 0;
    return store_result(&lc, sizeof(lc));
}
API SteamAPICall __cdecl SteamAPI_ISteamMatchmaking_JoinLobby(Iface, uint64_t lobby)
{
    std::vector<uint8_t> p; put(p, lobby);
    std::vector<uint8_t> r = request(JOIN_LOBBY, p);
    size_t at = 0;
    LobbyEnter le = {}; le.lobby = lobby; le.response = get<uint8_t>(r, at) ? 1 : 2;
    users_at = 0;
    return store_result(&le, sizeof(le));
}
API void __cdecl SteamAPI_ISteamMatchmaking_LeaveLobby(Iface, uint64_t lobby) { std::vector<uint8_t> p; put(p, lobby); request(LEAVE, p); }
API bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyData(Iface, uint64_t, const char*, const char*) { return true; }
API bool __cdecl SteamAPI_ISteamMatchmaking_SetLobbyJoinable(Iface, uint64_t, bool) { return true; }
static std::vector<uint64_t> members(uint64_t lobby)
{
    std::vector<uint8_t> p; put(p, lobby);
    std::vector<uint8_t> r = request(MEMBERS, p);
    size_t at = 0;
    uint32_t n = get<uint32_t>(r, at);
    std::vector<uint64_t> out;
    for (uint32_t i = 0; i < n; ++i) out.push_back(get<uint64_t>(r, at));
    return out;
}
API int __cdecl SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(Iface, uint64_t lobby) { return (int)members(lobby).size(); }
API uint64_t __cdecl SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(Iface, uint64_t lobby, int i) { auto m = members(lobby); return i >= 0 && i < (int)m.size() ? m[i] : 0; }
API uint64_t __cdecl SteamAPI_ISteamMatchmaking_GetLobbyOwner(Iface, uint64_t lobby) { auto m = members(lobby); return m.empty() ? 0 : m[0]; }
API bool __cdecl SteamAPI_ISteamNetworking_AllowP2PPacketRelay(Iface, bool) { return true; }
API bool __cdecl SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(Iface, uint64_t peer)
{
    std::lock_guard<std::mutex> lk(mx);
    accepted.insert(peer);
    for (auto &d : held[peer]) rx.emplace_back(peer, std::move(d));
    held.erase(peer);
    return true;
}
API bool __cdecl SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(Iface, uint64_t peer) { std::lock_guard<std::mutex> lk(mx); accepted.erase(peer); return true; }
API bool __cdecl SteamAPI_ISteamNetworking_SendP2PPacket(Iface, uint64_t to, const void *data, uint32_t n, int, int)
{
    {
        std::lock_guard<std::mutex> lk(mx);
        if (!accepted.count(to))  // sending to someone accepts their replies, as on real Steam
        {
            accepted.insert(to);
            for (auto &d : held[to]) rx.emplace_back(to, std::move(d));
            held.erase(to);
        }
    }
    std::vector<uint8_t> p; put(p, to);
    p.insert(p.end(), (const uint8_t*)data, (const uint8_t*)data + n);
    std::lock_guard<std::mutex> rl(req_mx);
    send_frame(SEND, p);
    return true;
}
API bool __cdecl SteamAPI_ISteamNetworking_IsP2PPacketAvailable(Iface, uint32_t *size, int)
{
    std::lock_guard<std::mutex> lk(mx);
    if (rx.empty()) return false;
    *size = (uint32_t)rx.front().second.size();
    return true;
}
API bool __cdecl SteamAPI_ISteamNetworking_ReadP2PPacket(Iface, void *dest, uint32_t cap, uint32_t *got, uint64_t *from, int)
{
    std::lock_guard<std::mutex> lk(mx);
    if (rx.empty()) return false;
    auto &f = rx.front();
    const uint32_t n = (uint32_t)std::min<size_t>(cap, f.second.size());
    memcpy(dest, f.second.data(), n);
    *got = n;
    *from = f.first;
    rx.pop_front();
    return true;
}

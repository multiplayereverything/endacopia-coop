// Netplay core: sockets, tick records, oracles, join/resync. Design notes are in netplay.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef DeleteFile
#undef CreateFile
#undef CreateDirectory
#include <algorithm>
#include <deque>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <SDL.h>
#include "net/netplay.h"
#include "net/netplay_engine.h"
#include "debug/out.h"
#include "util/file.h"
#include "util/directory.h"
#include "util/path.h"
#include "util/stream.h"

using namespace AGS::Common;

void sys_evt_process_one(const SDL_Event &event);

namespace NetPlay
{

static const uint32_t MP_MAGIC    = 0x31504D45; // 'EMP1'
static const uint32_t MP_PROTOCOL = 3;
static const int      DEFAULT_PORT = 27960;
static const uint32_t OWNER_IDLE_TICKS = 16;   // cursor ownership hand-over delay
static const size_t   MAX_MSG_SIZE = 256u * 1024u * 1024u;

enum MsgType : uint8_t
{
    kMsg_Hello = 1,     // guest -> host
    kMsg_Welcome,       // host -> guest
    kMsg_Reject,        // host -> guest
    kMsg_JoinReq,       // guest -> host (join or resync request)
    kMsg_Snapshot,      // host -> guest
    kMsg_Tick,          // host -> guest
    kMsg_Input,         // guest -> host
    kMsg_Desync,        // guest -> host (diagnostics)
    kMsg_Ping,
    kMsg_Pong,
    kMsg_Bye
};

enum EvType : uint8_t
{
    kEv_Move = 1,
    kEv_Button,
    kEv_Wheel,
    kEv_Key,
    kEv_Text
};

struct Writer
{
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void bytes(const void *p, size_t n) { const uint8_t *c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }
    void str(const String &s) { u32((uint32_t)s.GetLength()); bytes(s.GetCStr(), s.GetLength()); }
    void blob(const std::vector<uint8_t> &v) { u32((uint32_t)v.size()); if (!v.empty()) bytes(v.data(), v.size()); }
};

struct Reader
{
    const uint8_t *p; size_t n; size_t at = 0; bool bad = false;
    Reader(const uint8_t *data, size_t len) : p(data), n(len) {}
    bool need(size_t k) { if (at + k > n) { bad = true; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return p[at++]; }
    uint16_t u16() { uint16_t lo = u8(); return lo | (uint16_t)(u8() << 8); }
    uint32_t u32() { uint32_t lo = u16(); return lo | ((uint32_t)u16() << 16); }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    String str() { uint32_t len = u32(); if (!need(len)) return ""; String s((const char*)p + at, len); at += len; return s; }
    std::vector<uint8_t> blob() { uint32_t len = u32(); std::vector<uint8_t> v; if (!need(len)) return v; v.assign(p + at, p + at + len); at += len; return v; }
};

struct NetEvent
{
    uint8_t type = 0;
    uint8_t player = 0;
    int16_t x = 0, y = 0;
    uint8_t mask = 0, press = 0, eff = 0; // buttons
    int8_t  dz = 0;                       // wheel
    uint8_t down = 0, repeat = 0;         // keys
    uint16_t scancode = 0, mod = 0;
    int32_t sym = 0;
    String  text;
};

static void WriteEvent(Writer &w, const NetEvent &e)
{
    w.u8(e.type);
    w.u8(e.player);
    switch (e.type)
    {
    case kEv_Move: w.i16(e.x); w.i16(e.y); break;
    case kEv_Button: w.i16(e.x); w.i16(e.y); w.u8(e.mask); w.u8(e.press); w.u8(e.eff); break;
    case kEv_Wheel: w.u8((uint8_t)e.dz); break;
    case kEv_Key: w.u8(e.down); w.u16(e.scancode); w.i32(e.sym); w.u16(e.mod); w.u8(e.repeat); break;
    case kEv_Text: w.str(e.text); break;
    }
}

static NetEvent ReadEvent(Reader &r)
{
    NetEvent e;
    e.type = r.u8();
    e.player = r.u8();
    switch (e.type)
    {
    case kEv_Move: e.x = r.i16(); e.y = r.i16(); break;
    case kEv_Button: e.x = r.i16(); e.y = r.i16(); e.mask = r.u8(); e.press = r.u8(); e.eff = r.u8(); break;
    case kEv_Wheel: e.dz = (int8_t)r.u8(); break;
    case kEv_Key: e.down = r.u8(); e.scancode = r.u16(); e.sym = r.i32(); e.mod = r.u16(); e.repeat = r.u8(); break;
    case kEv_Text: e.text = r.str(); break;
    default: r.bad = true; break;
    }
    return e;
}

struct OracleEntry
{
    uint8_t kind = 0;
    bool is_blob = false;
    int32_t value = 0;
    std::vector<uint8_t> data;
};

struct TickRecord
{
    uint32_t tick = 0;
    uint32_t hash[6] = {};
    std::vector<NetEvent> events;
    std::vector<OracleEntry> oracles;
    int16_t cursor[2][2] = {};
    std::vector<uint32_t> gchunks; // only with --mp-debug
    int32_t eip = 0;               // only with --mp-debug: engine location at the boundary
};

static void WriteRecord(Writer &w, const TickRecord &rec)
{
    w.u32(rec.tick);
    for (int i = 0; i < 6; ++i) w.u32(rec.hash[i]);
    w.u16((uint16_t)rec.events.size());
    for (const auto &e : rec.events) WriteEvent(w, e);
    w.u32((uint32_t)rec.oracles.size());
    for (const auto &o : rec.oracles)
    {
        w.u8(o.kind);
        w.u8(o.is_blob ? 1 : 0);
        if (o.is_blob) w.blob(o.data); else w.i32(o.value);
    }
    for (int p = 0; p < 2; ++p) { w.i16(rec.cursor[p][0]); w.i16(rec.cursor[p][1]); }
    w.u32((uint32_t)rec.gchunks.size());
    for (uint32_t c : rec.gchunks) w.u32(c);
    w.i32(rec.eip);
}

static bool ReadRecord(Reader &r, TickRecord &rec)
{
    rec.tick = r.u32();
    for (int i = 0; i < 6; ++i) rec.hash[i] = r.u32();
    uint16_t nev = r.u16();
    for (uint16_t i = 0; i < nev && !r.bad; ++i) rec.events.push_back(ReadEvent(r));
    uint32_t nor = r.u32();
    for (uint32_t i = 0; i < nor && !r.bad; ++i)
    {
        OracleEntry o;
        o.kind = r.u8();
        o.is_blob = r.u8() != 0;
        if (o.is_blob) o.data = r.blob(); else o.value = r.i32();
        rec.oracles.push_back(std::move(o));
    }
    for (int p = 0; p < 2; ++p) { rec.cursor[p][0] = r.i16(); rec.cursor[p][1] = r.i16(); }
    uint32_t nc = r.u32();
    for (uint32_t i = 0; i < nc && !r.bad; ++i) rec.gchunks.push_back(r.u32());
    rec.eip = r.i32();
    return !r.bad;
}

struct Connection
{
    SOCKET sock = INVALID_SOCKET;
    std::vector<uint8_t> inbuf;
    std::vector<uint8_t> outbuf;
    size_t out_at = 0;
    size_t max_msg = MAX_MSG_SIZE; // host limits what a guest may send

    bool IsOpen() const { return sock != INVALID_SOCKET; }

    void Close()
    {
        if (sock != INVALID_SOCKET)
        {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        inbuf.clear();
        outbuf.clear();
        out_at = 0;
    }

    void Attach(SOCKET s)
    {
        Close();
        sock = s;
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        int bufsz = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsz, sizeof(bufsz));
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsz, sizeof(bufsz));
    }

    void Send(uint8_t type, const std::vector<uint8_t> &payload)
    {
        if (!IsOpen()) return;
        uint32_t len = (uint32_t)payload.size() + 1;
        uint8_t hdr[5] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
            (uint8_t)((len >> 16) & 0xFF), (uint8_t)(len >> 24), type };
        outbuf.insert(outbuf.end(), hdr, hdr + 5);
        outbuf.insert(outbuf.end(), payload.begin(), payload.end());
        Flush();
    }

    // Returns false if the connection broke
    bool Flush()
    {
        while (IsOpen() && out_at < outbuf.size())
        {
            int chunk = (int)std::min<size_t>(outbuf.size() - out_at, 1 << 20);
            int sent = send(sock, (const char*)outbuf.data() + out_at, chunk, 0);
            if (sent == SOCKET_ERROR)
            {
                if (WSAGetLastError() == WSAEWOULDBLOCK)
                    break;
                Close();
                return false;
            }
            out_at += sent;
        }
        if (out_at > 0 && out_at == outbuf.size())
        {
            outbuf.clear();
            out_at = 0;
        }
        else if (out_at > (8u << 20))
        {
            outbuf.erase(outbuf.begin(), outbuf.begin() + out_at);
            out_at = 0;
        }
        return IsOpen();
    }

    // Reads available data; returns false if the connection broke
    bool Pump()
    {
        if (!IsOpen()) return false;
        uint8_t tmp[65536];
        for (;;)
        {
            int got = recv(sock, (char*)tmp, sizeof(tmp), 0);
            if (got > 0)
            {
                inbuf.insert(inbuf.end(), tmp, tmp + got);
                continue;
            }
            if (got == 0) { Close(); return false; }
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            Close();
            return false;
        }
        return Flush();
    }

    // Extracts one complete message
    bool Next(uint8_t &type, std::vector<uint8_t> &payload)
    {
        if (inbuf.size() < 5) return false;
        uint32_t len = inbuf[0] | (inbuf[1] << 8) | (inbuf[2] << 16) | ((uint32_t)inbuf[3] << 24);
        if (len == 0 || len > max_msg) { Close(); return false; }
        if (inbuf.size() < 4 + (size_t)len) return false;
        type = inbuf[4];
        payload.assign(inbuf.begin() + 5, inbuf.begin() + 4 + len);
        inbuf.erase(inbuf.begin(), inbuf.begin() + 4 + len);
        return true;
    }
};

enum GuestPhase
{
    kGuest_Off,
    kGuest_Connecting,
    kGuest_Synced,
    kGuest_Resyncing,
    kGuest_Solo        // host left; playing locally
};

struct Config
{
    Role role = kRoleNone;
    int port = DEFAULT_PORT;
    String host_addr;
    String name = "Player";
    int control_port = 0;
    String guest_data_dir;
    bool debug = false;
} static cfg;

static bool wsa_ready = false;
static SOCKET listen_sock = INVALID_SOCKET;
static Connection peer;          // host: the guest; guest: the host
static Connection ctl;           // automation control client
static SOCKET ctl_listen = INVALID_SOCKET;
static std::string ctl_line;

// Shared lockstep state
static uint32_t tick_no = 0;
static bool synced = false;
static int  logical_x = 160, logical_y = 100;
static int  btn_state = 0, btn_accum = 0;
static bool keys_down[512] = {};
static int  local_x = 0, local_y = 0;
static bool local_seen = false;
static int  remote_x = -1000, remote_y = -1000;
static bool remote_seen = false;
static int  last_actor = 0;       // player of the latest click/key

// Host state
static bool guest_hello = false;
static bool guest_wants_snapshot = false;
static bool guest_needs_files = true;
static TickRecord cur_rec;           // record in progress (host)
static std::vector<NetEvent> local_q;  // local events awaiting next boundary
static std::vector<NetEvent> remote_q; // guest events awaiting next boundary
static int  owner = 0;
static uint32_t last_active[2] = {};
static int  player_btns[2] = {};
static int  cursors[2][2] = {{-1000,-1000},{-1000,-1000}};
static uint32_t rtt_ms = 0;
static auto last_ping = std::chrono::steady_clock::now();
static String host_status;

// Guest state
static GuestPhase gphase = kGuest_Off;
static std::deque<TickRecord> records;
static TickRecord play_rec;          // record being replayed
static size_t oracle_at = 0;
static bool oracle_mismatch = false;
static bool snapshot_ready = false;
static uint32_t snapshot_tick = 0;
static uint32_t snapshot_seed = 0;
static int snapshot_mx = 0, snapshot_my = 0, snapshot_owner = 0;
static String snapshot_path;
static std::vector<NetEvent> guest_out;  // guest input to send
static uint32_t desync_count = 0;
// Host, --mp-debug: script globals of recent ticks, to report host values at a guest's desync tick.
static std::deque<std::pair<uint32_t, std::vector<uint8_t>>> globals_history;

static String hud_text;
static auto hud_until = std::chrono::steady_clock::now();
static bool hud_sticky = false;

static void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Debug::Printf(kDbgMsg_Info, "[MP] %s", buf);
}

static void SetHud(const String &text, bool sticky = false, int seconds = 6)
{
    if (text != hud_text)
        Log("status: %s", text.GetCStr());
    hud_text = text;
    hud_sticky = sticky;
    hud_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

bool ParseCmdArg(int argc, char *argv[], int &ee)
{
    const char *arg = argv[ee];
    if (_stricmp(arg, "--mp-host") == 0)
    {
        cfg.role = kRoleHost;
        if (ee + 1 < argc && argv[ee + 1][0] != '-')
            cfg.port = atoi(argv[++ee]);
        return true;
    }
    if (_stricmp(arg, "--mp-join") == 0 && ee + 1 < argc)
    {
        cfg.role = kRoleGuest;
        String addr = argv[++ee];
        size_t colon = addr.FindCharReverse(':');
        if (colon != String::NoIndex)
        {
            cfg.port = atoi(addr.GetCStr() + colon + 1);
            addr.ClipRight(addr.GetLength() - colon);
        }
        cfg.host_addr = addr;
        return true;
    }
    if (_stricmp(arg, "--mp-name") == 0 && ee + 1 < argc)
    {
        cfg.name = argv[++ee];
        return true;
    }
    if (_stricmp(arg, "--mp-control") == 0 && ee + 1 < argc)
    {
        cfg.control_port = atoi(argv[++ee]);
        return true;
    }
    if (_stricmp(arg, "--mp-watch") == 0 && ee + 1 < argc)
    {
        String spec = argv[++ee];
        size_t colon = spec.FindChar(':');
        if (colon != String::NoIndex)
            NetPlayEngine::SetWatch(atoi(spec.GetCStr()), atoi(spec.GetCStr() + colon + 1));
        return true;
    }
    if (_stricmp(arg, "--mp-debug") == 0)
    {
        cfg.debug = true;
        return true;
    }
    if (_stricmp(arg, "--mp-guest-dir") == 0 && ee + 1 < argc)
    {
        cfg.guest_data_dir = argv[++ee];
        return true;
    }
    return false;
}

// AGS refuses to create a missing parent dir, so build the whole chain here.
static void EnsureDir(const String &path)
{
    String p = path;
    p.Replace('/', '\\');
    for (size_t i = 3; i <= p.GetLength(); ++i)
    {
        if (i == p.GetLength() || p.GetAt(i) == '\\')
            CreateDirectoryA(p.Left(i).GetCStr(), nullptr);
    }
}

String GetGuestDataDir()
{
    String dir = cfg.guest_data_dir;
    if (dir.IsEmpty())
    {
        char buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
        String base = (n > 0 && n < MAX_PATH) ? String(buf) : String(".");
        dir = Path::ConcatPaths(base, "EndacopiaMP/guest-saves");
    }
    EnsureDir(Path::ConcatPaths(dir, "shared"));
    return dir;
}

static SOCKET OpenListen(int port, bool loopback_only)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR || listen(s, 2) == SOCKET_ERROR)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

static SOCKET AcceptOne(SOCKET ls)
{
    if (ls == INVALID_SOCKET) return INVALID_SOCKET;
    return accept(ls, nullptr, nullptr);
}

void Startup()
{
    if (cfg.role == kRoleNone && cfg.control_port == 0)
        return;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Log("WSAStartup failed");
        cfg.role = kRoleNone;
        return;
    }
    wsa_ready = true;

    if (cfg.control_port > 0)
    {
        ctl_listen = OpenListen(cfg.control_port, true);
        Log("automation control on 127.0.0.1:%d %s", cfg.control_port, ctl_listen != INVALID_SOCKET ? "ok" : "FAILED");
    }

    if (cfg.role == kRoleHost)
    {
        listen_sock = OpenListen(cfg.port, false);
        if (listen_sock == INVALID_SOCKET)
        {
            Log("cannot listen on port %d (error %d)", cfg.port, WSAGetLastError());
            SetHud(String::FromFormat("MP: cannot open port %d", cfg.port), false, 15);
            cfg.role = kRoleNone;
            return;
        }
        Log("hosting on port %d", cfg.port);
        SetHud(String::FromFormat("HOST on port %d - waiting for player 2", cfg.port), false, 10);
    }
}

void Shutdown()
{
    if (peer.IsOpen())
    {
        peer.Send(kMsg_Bye, {});
        peer.Flush();
    }
    peer.Close();
    ctl.Close();
    if (listen_sock != INVALID_SOCKET) closesocket(listen_sock);
    if (ctl_listen != INVALID_SOCKET) closesocket(ctl_listen);
    listen_sock = ctl_listen = INVALID_SOCKET;
    if (wsa_ready) WSACleanup();
    wsa_ready = false;
}

Role GetRole() { return cfg.role; }
bool IsSynced() { return synced; }
bool IsInputRouted()
{
    return synced || (cfg.role == kRoleGuest && (gphase == kGuest_Connecting || gphase == kGuest_Resyncing));
}
bool IsReplaying() { return cfg.role == kRoleGuest && synced && gphase == kGuest_Synced; }
bool IsRecording() { return cfg.role == kRoleHost && synced; }

static int SdlButtonMask(int button)
{
    switch (button)
    {
    case SDL_BUTTON_LEFT: return 0x01;
    case SDL_BUTTON_RIGHT: return 0x02;
    case SDL_BUTTON_MIDDLE: return 0x04;
    default: return 0;
    }
}

static void QueueLocal(const NetEvent &e)
{
    if (cfg.role == kRoleHost)
        local_q.push_back(e);
    else if (cfg.role == kRoleGuest)
        guest_out.push_back(e);
}

static void NoteLocalPos(int x, int y)
{
    local_x = x;
    local_y = y;
    local_seen = true;
}

bool CaptureEvent(const SDL_Event &evt)
{
    if (!IsInputRouted())
    {
        // Track local cursor for HUD purposes even in vanilla mode
        if (evt.type == SDL_MOUSEMOTION)
        {
            int x = evt.motion.x, y = evt.motion.y;
            NetPlayEngine::WindowToGame(x, y);
            NoteLocalPos(x, y);
        }
        return false;
    }

    NetEvent e;
    e.player = (cfg.role == kRoleGuest) ? 1 : 0;
    switch (evt.type)
    {
    case SDL_MOUSEMOTION:
    {
        int x = evt.motion.x, y = evt.motion.y;
        NetPlayEngine::WindowToGame(x, y);
        NoteLocalPos(x, y);
        e.type = kEv_Move; e.x = (int16_t)x; e.y = (int16_t)y;
        QueueLocal(e);
        return true;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
        int mask = SdlButtonMask(evt.button.button);
        if (mask == 0) return true;
        int x = evt.button.x, y = evt.button.y;
        NetPlayEngine::WindowToGame(x, y);
        NoteLocalPos(x, y);
        e.type = kEv_Button; e.x = (int16_t)x; e.y = (int16_t)y;
        e.mask = (uint8_t)mask; e.press = evt.type == SDL_MOUSEBUTTONDOWN ? 1 : 0;
        QueueLocal(e);
        return true;
    }
    case SDL_MOUSEWHEEL:
        e.type = kEv_Wheel; e.dz = (int8_t)std::max(-100, std::min(100, (int)evt.wheel.y));
        QueueLocal(e);
        return true;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
        // Alt+Enter: local fullscreen toggle, never synced
        if (evt.type == SDL_KEYDOWN && (evt.key.keysym.mod & KMOD_ALT) &&
            (evt.key.keysym.scancode == SDL_SCANCODE_RETURN || evt.key.keysym.scancode == SDL_SCANCODE_KP_ENTER))
        {
            NetPlayEngine::ToggleFullscreen();
            return true;
        }
        e.type = kEv_Key;
        e.down = evt.type == SDL_KEYDOWN ? 1 : 0;
        e.scancode = (uint16_t)evt.key.keysym.scancode;
        e.sym = evt.key.keysym.sym;
        e.mod = evt.key.keysym.mod;
        e.repeat = evt.key.repeat;
        QueueLocal(e);
        return true;
    }
    case SDL_TEXTINPUT:
        e.type = kEv_Text;
        e.text = evt.text.text;
        QueueLocal(e);
        return true;
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        return true; // touch is translated into mouse events by the engine itself
    default:
        return false;
    }
}

void GetLogicalMouse(int &x, int &y) { x = logical_x; y = logical_y; }
void SetLogicalMouse(int x, int y) { logical_x = x; logical_y = y; }
bool IsScancodeDown(int scancode) { return scancode >= 0 && scancode < 512 && keys_down[scancode]; }

bool LocalPlayerActedLast()
{
    return last_actor == (cfg.role == kRoleGuest ? 1 : 0);
}

// Applies one synced event to the engine input state; identical on both peers
static void ApplyEvent(const NetEvent &e)
{
    if (e.type == kEv_Button || e.type == kEv_Key || e.type == kEv_Text)
        last_actor = e.player & 1;
    switch (e.type)
    {
    case kEv_Move:
        logical_x = e.x; logical_y = e.y;
        break;
    case kEv_Button:
        logical_x = e.x; logical_y = e.y;
        if (e.press) btn_accum |= e.mask;
        btn_state = e.eff;
        break;
    case kEv_Wheel:
        NetPlayEngine::AddMouseWheel(e.dz);
        break;
    case kEv_Key:
    {
        if (e.scancode < 512) keys_down[e.scancode] = e.down != 0;
        SDL_Event s = {};
        s.type = e.down ? SDL_KEYDOWN : SDL_KEYUP;
        s.key.state = e.down ? SDL_PRESSED : SDL_RELEASED;
        s.key.repeat = e.repeat;
        s.key.keysym.scancode = (SDL_Scancode)e.scancode;
        s.key.keysym.sym = e.sym;
        s.key.keysym.mod = e.mod;
        NetPlayEngine::PushKeyEvent(s);
        break;
    }
    case kEv_Text:
    {
        SDL_Event s = {};
        s.type = SDL_TEXTINPUT;
        snprintf(s.text.text, sizeof(s.text.text), "%s", e.text.GetCStr());
        NetPlayEngine::PushKeyEvent(s);
        break;
    }
    }
}

static void ApplyRecordInput(const TickRecord &rec)
{
    btn_accum = 0;
    for (const auto &e : rec.events)
        ApplyEvent(e);
    NetPlayEngine::SetMouseButtons(btn_state, btn_accum);
}

static void ResetNetInput()
{
    btn_state = btn_accum = 0;
    player_btns[0] = player_btns[1] = 0;
    memset(keys_down, 0, sizeof(keys_down));
    local_q.clear();
    remote_q.clear();
    NetPlayEngine::ResetInputState();
}

// Host: merges both players' raw events into the authoritative event list
static void HostMergeInput(std::vector<NetEvent> &out)
{
    auto handle = [&](NetEvent e)
    {
        const int p = e.player & 1;
        switch (e.type)
        {
        case kEv_Move:
            cursors[p][0] = e.x; cursors[p][1] = e.y;
            if (owner != p && (tick_no - last_active[owner]) < OWNER_IDLE_TICKS)
            {
                last_active[p] = tick_no;
                return; // other player is actively moving; keep ownership
            }
            owner = p;
            last_active[p] = tick_no;
            out.push_back(e);
            break;
        case kEv_Button:
        {
            cursors[p][0] = e.x; cursors[p][1] = e.y;
            owner = p;
            last_active[p] = tick_no;
            if (e.press) player_btns[p] |= e.mask; else player_btns[p] &= ~e.mask;
            e.eff = (uint8_t)(player_btns[0] | player_btns[1]);
            out.push_back(e);
            break;
        }
        default:
            out.push_back(e);
            break;
        }
    };
    for (auto &e : local_q) handle(e);
    for (auto &e : remote_q) handle(e);
    local_q.clear();
    remote_q.clear();
}

static void FlagDesync(const char *why);

int OracleInt(OracleKind kind, int local_value)
{
    if (IsRecording())
    {
        OracleEntry o; o.kind = kind; o.value = local_value;
        cur_rec.oracles.push_back(std::move(o));
        return local_value;
    }
    if (IsReplaying())
    {
        if (oracle_at >= play_rec.oracles.size() || play_rec.oracles[oracle_at].kind != kind ||
            play_rec.oracles[oracle_at].is_blob)
        {
            FlagDesync(String::FromFormat("oracle mismatch (int kind %d at #%u)", kind, (unsigned)oracle_at).GetCStr());
            return local_value;
        }
        return play_rec.oracles[oracle_at++].value;
    }
    return local_value;
}

void OracleBlob(OracleKind kind, std::vector<uint8_t> &data)
{
    if (IsRecording())
    {
        OracleEntry o; o.kind = kind; o.is_blob = true; o.data = data;
        cur_rec.oracles.push_back(std::move(o));
        return;
    }
    if (IsReplaying())
    {
        if (oracle_at >= play_rec.oracles.size() || play_rec.oracles[oracle_at].kind != kind ||
            !play_rec.oracles[oracle_at].is_blob)
        {
            FlagDesync(String::FromFormat("oracle mismatch (blob kind %d at #%u)", kind, (unsigned)oracle_at).GetCStr());
            return;
        }
        data = play_rec.oracles[oracle_at++].data;
    }
}

// Join: the host sends its save folder so file reads match on both peers.
static void CollectDir(Writer &w, uint8_t dir_id, const String &dir)
{
    std::vector<std::pair<String, std::vector<uint8_t>>> files;
    if (!dir.IsEmpty() && File::IsDirectory(dir))
    {
        for (FindFile ff = FindFile::OpenFiles(dir, "*"); !ff.AtEnd(); ff.Next())
        {
            String name = ff.Current();
            String full = Path::ConcatPaths(dir, name);
            std::unique_ptr<Stream> in(File::OpenFileRead(full));
            if (!in) continue;
            soff_t len = in->GetLength();
            if (len < 0 || len > (64 << 20)) continue;
            std::vector<uint8_t> bytes((size_t)len);
            if (len > 0) in->Read(bytes.data(), (size_t)len);
            files.emplace_back(name, std::move(bytes));
        }
    }
    w.u8(dir_id);
    w.u32((uint32_t)files.size());
    for (auto &f : files) { w.str(f.first); w.blob(f.second); }
}

static void WriteDirFiles(Reader &r, const String &user_dir, const String &app_dir)
{
    for (int d = 0; d < 2 && !r.bad; ++d)
    {
        uint8_t dir_id = r.u8();
        uint32_t count = r.u32();
        String dir = dir_id == 0 ? user_dir : app_dir;
        // A relative dir means the engine fell back to the game folder; never mirror there.
        bool writable = !dir.IsEmpty() && !dir.StartsWith(".") && File::IsDirectory(dir);
        if (writable && dir_id == 0)
        {
            // drop stale saves so the mirror matches host exactly
            std::vector<String> old;
            for (FindFile ff = FindFile::OpenFiles(dir, "agssave.*"); !ff.AtEnd(); ff.Next())
                old.push_back(Path::ConcatPaths(dir, ff.Current()));
            for (auto &o : old) File::DeleteFile(o);
        }
        for (uint32_t i = 0; i < count && !r.bad; ++i)
        {
            String name = r.str();
            std::vector<uint8_t> bytes = r.blob();
            if (!writable || name.FindChar('/') != String::NoIndex || name.FindChar('\\') != String::NoIndex ||
                name.FindString("..") != String::NoIndex)
                continue;
            std::unique_ptr<Stream> out(File::CreateFile(Path::ConcatPaths(dir, name)));
            if (out && !bytes.empty()) out->Write(bytes.data(), bytes.size());
        }
        Log("mirrored %u files into %s", count, dir.GetCStr());
    }
}

static void HostDropGuest(const char *why)
{
    Log("guest dropped: %s", why);
    peer.Close();
    guest_hello = false;
    guest_wants_snapshot = false;
    guest_needs_files = true;
    if (synced)
    {
        synced = false;
        ResetNetInput();
    }
    remote_seen = false;
    SetHud(String::FromFormat("Player 2 left (%s) - waiting on port %d", why, cfg.port), false, 8);
}

static void HostHandleMessage(uint8_t type, const std::vector<uint8_t> &payload)
{
    Reader r(payload.data(), payload.size());
    switch (type)
    {
    case kMsg_Hello:
    {
        uint32_t magic = r.u32(), proto = r.u32();
        String guid = r.str();
        String name = r.str();
        Writer w;
        if (magic != MP_MAGIC || proto != MP_PROTOCOL)
        {
            w.str("Version mismatch: both players need the same Endacopia Multiplayer build.");
            peer.Send(kMsg_Reject, w.b);
            peer.Flush();
            HostDropGuest("version mismatch");
            return;
        }
        if (guid.CompareNoCase(NetPlayEngine::GetGameGuid()) != 0)
        {
            w.str("Game data mismatch: both players need the same version of Endacopia.");
            peer.Send(kMsg_Reject, w.b);
            peer.Flush();
            HostDropGuest("game mismatch");
            return;
        }
        guest_hello = true;
        w.u32(MP_PROTOCOL);
        w.str(NetPlayEngine::GetTranslationName());
        w.i32(NetPlayEngine::GetCurrentFrameRate());
        peer.Send(kMsg_Welcome, w.b);
        Log("guest '%s' says hello", name.GetCStr());
        SetHud(String::FromFormat("%s is joining...", name.GetCStr()), true);
        break;
    }
    case kMsg_JoinReq:
    {
        uint8_t reason = r.u8();
        guest_wants_snapshot = true;
        if (reason == 1)
        {
            Log("guest requested resync");
            SetHud("Resyncing player 2...", true);
        }
        break;
    }
    case kMsg_Input:
    {
        if (!synced) break;
        int16_t cx = r.i16(), cy = r.i16();
        cursors[1][0] = cx; cursors[1][1] = cy;
        remote_x = cx; remote_y = cy; remote_seen = true;
        uint16_t n = r.u16();
        for (uint16_t i = 0; i < n && !r.bad; ++i)
        {
            NetEvent e = ReadEvent(r);
            e.player = 1;
            remote_q.push_back(e);
        }
        break;
    }
    case kMsg_Desync:
    {
        uint32_t t = r.u32();
        uint32_t mask = r.u32();
        String why = r.str();
        Log("guest reports desync at tick %u (components 0x%x): %s", t, mask, why.GetCStr());
        if (cfg.debug)
            Log("  host now: %s", NetPlayEngine::DescribeDynamicSprites().GetCStr());
        // Host values are from a later tick, but usually show which variable it is.
        for (size_t at = why.FindString("| chunk "); at != String::NoIndex; at = why.FindString("| chunk ", at + 1))
        {
            const size_t chunk = (size_t)atoi(why.GetCStr() + at + 8);
            for (const auto &h : globals_history)
            {
                if (h.first != t || (chunk + 1) * 64 > h.second.size()) continue;
                String bytes;
                for (size_t b = chunk * 64; b < chunk * 64 + 64; b += 4)
                    bytes.AppendFmt("%08x ", *(const uint32_t*)&h.second[b]);
                Log("  host at tick %u, chunk %u: %s", t, (unsigned)chunk, bytes.GetCStr());
            }
        }
        break;
    }
    case kMsg_Pong:
    {
        uint32_t sent_ms = r.u32();
        uint32_t now_ms = (uint32_t)GetTickCount();
        rtt_ms = now_ms - sent_ms;
        break;
    }
    case kMsg_Bye:
        HostDropGuest("quit");
        break;
    }
}

static void HostPoll()
{
    if (!peer.IsOpen())
    {
        SOCKET s = AcceptOne(listen_sock);
        if (s != INVALID_SOCKET)
        {
            peer.Attach(s);
            peer.max_msg = 1u << 20;
            Log("guest connected");
            SetHud("Player 2 connecting...", true);
        }
        return;
    }
    if (!peer.Pump())
    {
        HostDropGuest("connection lost");
        return;
    }
    uint8_t type;
    std::vector<uint8_t> payload;
    while (peer.IsOpen() && peer.Next(type, payload))
        HostHandleMessage(type, payload);
    if (!peer.IsOpen() && (guest_hello || synced))
        HostDropGuest("connection lost");

    auto now = std::chrono::steady_clock::now();
    if (peer.IsOpen() && now - last_ping > std::chrono::seconds(1))
    {
        Writer w; w.u32((uint32_t)GetTickCount());
        peer.Send(kMsg_Ping, w.b);
        last_ping = now;
    }
}

// Host: snapshot + self-restore at an idle point, then send to guest
static void HostDoSnapshot()
{
    String tmp = Path::ConcatPaths(NetPlayEngine::GetUserDataDir(), "mp_snapshot.tmp");
    std::vector<uint8_t> save_bytes;
    // Not recording while we restore; the guest restores the same bytes the same way.
    synced = false;
    if (!NetPlayEngine::MakeSnapshot(tmp, save_bytes))
    {
        Log("snapshot failed");
        return;
    }
    const uint32_t seed = (uint32_t)GetTickCount() ^ (uint32_t)(tick_no * 2654435761u);
    // files go first so the guest's save dir matches before restoring
    Writer files;
    files.u8(guest_needs_files ? 1 : 0);
    if (guest_needs_files)
    {
        CollectDir(files, 0, NetPlayEngine::GetUserDataDir());
        CollectDir(files, 1, NetPlayEngine::GetAppDataDir());
    }

    // Restoring runs fade frames that advance tick_no, so the shared timeline starts after the restore.
    NetPlayEngine::RestoreSnapshot(tmp, 1000);
    File::DeleteFile(tmp);
    srand(seed);
    ResetNetInput();
    Writer w;
    w.u32(tick_no);
    w.u32(seed);
    w.bytes(files.b.data(), files.b.size());
    // the logical mouse after restore is part of the snapshot message
    w.i32(logical_x);
    w.i32(logical_y);
    w.i32(owner);
    w.blob(save_bytes);
    peer.Send(kMsg_Snapshot, w.b);
    guest_needs_files = false;
    guest_wants_snapshot = false;

    synced = true;
    cur_rec = TickRecord();
    cur_rec.tick = tick_no;
    NetPlayEngine::ComputeStateHash(cur_rec.hash);
    last_active[0] = last_active[1] = tick_no;
    Log("snapshot sent at tick %u (%u bytes save) misc: %s", tick_no, (unsigned)save_bytes.size(), NetPlayEngine::DescribeMisc().GetCStr());
    SetHud("Player 2 joined - co-op active", false, 6);
}

static void GuestGoSolo(const char *why)
{
    Log("host connection ended: %s", why);
    peer.Close();
    gphase = kGuest_Solo;
    synced = false;
    records.clear();
    ResetNetInput();
    SetHud(String::FromFormat("Host left (%s) - continuing solo", why), false, 10);
}

static void GuestSendInput()
{
    if (!peer.IsOpen() || (gphase != kGuest_Synced && gphase != kGuest_Resyncing))
    {
        guest_out.clear();
        return;
    }
    if (guest_out.empty() && !local_seen)
        return;
    Writer w;
    w.i16((int16_t)local_x);
    w.i16((int16_t)local_y);
    size_t n = std::min<size_t>(guest_out.size(), 2000);
    w.u16((uint16_t)n);
    for (size_t i = 0; i < n; ++i) WriteEvent(w, guest_out[i]);
    guest_out.erase(guest_out.begin(), guest_out.begin() + n);
    if (n > 0 || local_seen)
        peer.Send(kMsg_Input, w.b);
}

static void GuestHandleMessage(uint8_t type, const std::vector<uint8_t> &payload)
{
    Reader r(payload.data(), payload.size());
    switch (type)
    {
    case kMsg_Welcome:
    {
        uint32_t proto = r.u32();
        String tra = r.str();
        r.i32(); // host frame rate (informational)
        if (proto != MP_PROTOCOL) { GuestGoSolo("protocol mismatch"); return; }
        if (tra.CompareNoCase(NetPlayEngine::GetTranslationName()) != 0)
        {
            Log("switching translation to host's '%s'", tra.GetCStr());
            NetPlayEngine::ChangeTranslation(tra);
        }
        Writer w; w.u8(0);
        peer.Send(kMsg_JoinReq, w.b);
        SetHud("Downloading game state from host...", true);
        break;
    }
    case kMsg_Reject:
    {
        String why = r.str();
        Log("host rejected: %s", why.GetCStr());
        SetHud(why, false, 20);
        GuestGoSolo("rejected");
        break;
    }
    case kMsg_Snapshot:
    {
        snapshot_tick = r.u32();
        snapshot_seed = r.u32();
        if (r.u8() != 0)
            WriteDirFiles(r, NetPlayEngine::GetUserDataDir(), NetPlayEngine::GetAppDataDir());
        snapshot_mx = r.i32();
        snapshot_my = r.i32();
        snapshot_owner = r.i32();
        std::vector<uint8_t> save = r.blob();
        if (r.bad) { GuestGoSolo("bad snapshot"); return; }
        snapshot_path = Path::ConcatPaths(NetPlayEngine::GetUserDataDir(), "mp_snapshot.tmp");
        std::unique_ptr<Stream> out(File::CreateFile(snapshot_path));
        if (!out) { GuestGoSolo("cannot write snapshot"); return; }
        out->Write(save.data(), save.size());
        out.reset();
        records.clear(); // anything older than the snapshot is obsolete
        snapshot_ready = true;
        Log("snapshot received for tick %u (%u bytes)", snapshot_tick, (unsigned)save.size());
        break;
    }
    case kMsg_Tick:
    {
        TickRecord rec;
        if (!ReadRecord(r, rec)) { GuestGoSolo("bad record"); return; }
        records.push_back(std::move(rec));
        break;
    }
    case kMsg_Ping:
    {
        Writer w; w.u32(r.u32());
        peer.Send(kMsg_Pong, w.b);
        break;
    }
    case kMsg_Bye:
        GuestGoSolo("host quit");
        break;
    }
}

static void GuestPoll()
{
    if (!peer.IsOpen()) return;
    GuestSendInput();
    if (!peer.Pump())
    {
        GuestGoSolo("connection lost");
        return;
    }
    uint8_t type;
    std::vector<uint8_t> payload;
    while (peer.IsOpen() && peer.Next(type, payload))
        GuestHandleMessage(type, payload);
    if (!peer.IsOpen() && gphase != kGuest_Solo)
        GuestGoSolo("connection lost");
}

static void FlagDesync(const char *why)
{
    if (cfg.role != kRoleGuest || gphase != kGuest_Synced)
        return;
    oracle_mismatch = true;
    desync_count++;
    Log("DESYNC at tick %u: %s", tick_no, why);
    Writer w;
    w.u32(tick_no);
    w.u32(0);
    w.str(why);
    peer.Send(kMsg_Desync, w.b);
    // Request a fresh snapshot; keep running locally until it arrives
    gphase = kGuest_Resyncing;
    synced = false;
    snapshot_ready = false;
    Writer jr; jr.u8(1);
    peer.Send(kMsg_JoinReq, jr.b);
    NetPlayEngine::CancelScripts();
    SetHud("Out of sync - resyncing with host...", true);
}

static bool PumpSystemWhileWaiting()
{
    // before the game is loaded nothing else draws: show the HUD (status)
    static DWORD last_draw = 0;
    if (!NetPlayEngine::IsGameLoaded() && GetTickCount() - last_draw > 200)
    {
        NetPlayEngine::RenderWaitScreen();
        last_draw = GetTickCount();
    }
    // keep the window responsive; input is captured (routed) by netplay
    SDL_Event evt;
    while (SDL_PollEvent(&evt))
    {
        if (evt.type == SDL_QUIT)
        {
            return false;
        }
        if (!CaptureEvent(evt))
        {
            sys_evt_process_one(evt);
        }
    }
    return true;
}

static void PollControl();

String GuestWaitForJoin(int &slot)
{
    slot = 1000;
    if (cfg.role != kRoleGuest)
        return "";
    gphase = kGuest_Connecting;
    SetHud(String::FromFormat("Connecting to %s:%d...", cfg.host_addr.GetCStr(), cfg.port), true);

    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    String port_s = String::FromFormat("%d", cfg.port);
    auto give_up_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!peer.IsOpen())
    {
        if (getaddrinfo(cfg.host_addr.GetCStr(), port_s.GetCStr(), &hints, &res) == 0 && res)
        {
            SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (s != INVALID_SOCKET && connect(s, res->ai_addr, (int)res->ai_addrlen) == 0)
                peer.Attach(s);
            else if (s != INVALID_SOCKET)
                closesocket(s);
            freeaddrinfo(res);
            res = nullptr;
        }
        if (peer.IsOpen()) break;
        if (std::chrono::steady_clock::now() > give_up_at || !PumpSystemWhileWaiting())
        {
            GuestGoSolo("could not connect");
            return "";
        }
        PollControl();
        Sleep(500);
    }
    Log("connected to host %s:%d", cfg.host_addr.GetCStr(), cfg.port);
    Writer w;
    w.u32(MP_MAGIC);
    w.u32(MP_PROTOCOL);
    w.str(NetPlayEngine::GetGameGuid());
    w.str(cfg.name);
    peer.Send(kMsg_Hello, w.b);

    give_up_at = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    while (!snapshot_ready)
    {
        GuestPoll();
        PollControl();
        if (gphase == kGuest_Solo || !PumpSystemWhileWaiting() || std::chrono::steady_clock::now() > give_up_at)
        {
            if (gphase != kGuest_Solo) GuestGoSolo("gave up waiting");
            return "";
        }
        Sleep(5);
    }
    return snapshot_path;
}

// Guest: blocks until the record for 'tick' arrives; false if we stopped being a synced guest.
static bool GuestFetchRecord(uint32_t tick, TickRecord &out)
{
    for (;;)
    {
        GuestPoll();
        PollControl();
        if (gphase != kGuest_Synced)
            return false;
        while (!records.empty() && records.front().tick < tick)
            records.pop_front();
        if (!records.empty())
        {
            if (records.front().tick != tick)
            {
                FlagDesync(String::FromFormat("tick gap: wanted %u got %u", tick, records.front().tick).GetCStr());
                return false;
            }
            out = std::move(records.front());
            records.pop_front();
            return true;
        }
        if (!PumpSystemWhileWaiting())
        {
            GuestGoSolo("window closed");
            return false;
        }
        Sleep(1);
    }
}

static void GuestBeginRecord(uint32_t tick)
{
    TickRecord rec;
    if (!GuestFetchRecord(tick, rec))
        return;
    uint32_t mine[6];
    NetPlayEngine::ComputeStateHash(mine);
    uint32_t diff = 0;
    for (int i = 0; i < 6; ++i)
        if (mine[i] != rec.hash[i]) diff |= 1u << i;
    play_rec = std::move(rec);
    oracle_at = 0;
    if (diff)
    {
        static const char *names[6] = { "room/player", "characters", "room objects", "script globals", "gui/cursor", "misc" };
        String why = "state hash differs:";
        for (int i = 0; i < 6; ++i) if (diff & (1u << i)) { why.Append(" "); why.Append(names[i]); }
        why.AppendFmt(" | guest misc: %s", NetPlayEngine::DescribeMisc().GetCStr());
        if ((diff & (1u << 5)) && cfg.debug)
            Log("guest %s", NetPlayEngine::DescribeDynamicSprites().GetCStr());
        if ((diff & (1u << 3)) && !play_rec.gchunks.empty())
        {
            std::vector<uint32_t> mine_chunks;
            NetPlayEngine::ComputeGlobalChunks(mine_chunks);
            int shown = 0;
            for (size_t c = 0; c < mine_chunks.size() && c < play_rec.gchunks.size() && shown < 5; ++c)
            {
                if (mine_chunks[c] == play_rec.gchunks[c]) continue;
                Log("globals differ: chunk %u, guest %s", (unsigned)c, NetPlayEngine::DescribeGlobalChunk(c).GetCStr());
                why.AppendFmt(" | chunk %u", (unsigned)c);
                shown++;
            }
        }
        Writer w; w.u32(tick); w.u32(diff); w.str(why);
        peer.Send(kMsg_Desync, w.b);
        FlagDesync(why.GetCStr());
        return;
    }
    if (cfg.debug && play_rec.eip != NetPlayEngine::GetEip())
        Log("eip differs at tick %u: host %d guest %d", tick, play_rec.eip, NetPlayEngine::GetEip());
    remote_x = play_rec.cursor[0][0];
    remote_y = play_rec.cursor[0][1];
    remote_seen = remote_x > -1000;
    ApplyRecordInput(play_rec);
}

void OnFrameBoundary()
{
    NetPlayEngine::CheckWatchPerFrame(tick_no);
    PollControl();
    if (cfg.role == kRoleHost)
    {
        HostPoll();
        if (synced)
        {
            // finalize the record of the frame that just ended
            cur_rec.cursor[0][0] = (int16_t)cursors[0][0];
            cur_rec.cursor[0][1] = (int16_t)cursors[0][1];
            cur_rec.cursor[1][0] = (int16_t)cursors[1][0];
            cur_rec.cursor[1][1] = (int16_t)cursors[1][1];
            Writer w;
            WriteRecord(w, cur_rec);
            peer.Send(kMsg_Tick, w.b);
        }
        tick_no++;
        if (synced)
        {
            cursors[0][0] = local_x; cursors[0][1] = local_y;
            cur_rec = TickRecord();
            cur_rec.tick = tick_no;
            NetPlayEngine::ComputeStateHash(cur_rec.hash);
            if (cfg.debug)
            {
                NetPlayEngine::ComputeGlobalChunks(cur_rec.gchunks);
                cur_rec.eip = NetPlayEngine::GetEip();
                globals_history.emplace_back(tick_no, std::vector<uint8_t>());
                NetPlayEngine::CopyGlobals(globals_history.back().second);
                if (globals_history.size() > 200) globals_history.pop_front();
            }
            HostMergeInput(cur_rec.events);
            ApplyRecordInput(cur_rec);
        }
        return;
    }
    if (cfg.role == kRoleGuest)
    {
        if (gphase == kGuest_Synced)
        {
            if (oracle_at != play_rec.oracles.size())
            {
                String kinds;
                for (const auto &ev : play_rec.events)
                    kinds.AppendFmt("ev%d/p%d/%d ", ev.type, ev.player, ev.type == kEv_Key ? ev.scancode : ev.mask);
                for (size_t i = 0; i < play_rec.oracles.size() && i < 24; ++i)
                    kinds.AppendFmt("%d%s", play_rec.oracles[i].kind, i + 1 == oracle_at ? "*" : ",");
                FlagDesync(String::FromFormat("oracle count mismatch at tick %u (%u of %u used; host kinds %s; eip host %d guest %d; %s)",
                    tick_no, (unsigned)oracle_at, (unsigned)play_rec.oracles.size(), kinds.GetCStr(), play_rec.eip, NetPlayEngine::GetEip(),
                    NetPlayEngine::DescribeState().GetCStr()).GetCStr());
            }
        }
        tick_no++;
        if (gphase == kGuest_Synced)
            GuestBeginRecord(tick_no);
        else
            GuestPoll();
    }
}

bool ShouldSkipFrameWait()
{
    return cfg.role == kRoleGuest && gphase == kGuest_Synced && records.size() > 2;
}

void OnIdlePoint()
{
    if (cfg.role == kRoleHost)
    {
        if (peer.IsOpen() && guest_hello && guest_wants_snapshot && NetPlayEngine::IsIdle())
            HostDoSnapshot();
        return;
    }
    if (cfg.role == kRoleGuest && gphase == kGuest_Resyncing)
    {
        GuestPoll();
        if (snapshot_ready && NetPlayEngine::IsIdle())
        {
            Log("restoring resync snapshot");
            NetPlayEngine::RestoreSnapshot(snapshot_path, 1000);
            File::DeleteFile(snapshot_path);
            OnSnapshotRestored();
        }
    }
}

void RestoreJoinSnapshot(const String &path, int slot)
{
    NetPlayEngine::RestoreSnapshot(path, slot);
    File::DeleteFile(path);
}

void OnSnapshotRestored()
{
    if (cfg.role != kRoleGuest || !snapshot_ready)
        return;
    snapshot_ready = false;
    srand(snapshot_seed);
    ResetNetInput();
    logical_x = snapshot_mx;
    logical_y = snapshot_my;
    owner = snapshot_owner;
    tick_no = snapshot_tick;
    gphase = kGuest_Synced;
    synced = true;
    oracle_mismatch = false;
    play_rec = TickRecord();
    oracle_at = 0;
    SetHud(desync_count ? "Resynced with host" : "Joined host - co-op active", false, 6);
    GuestBeginRecord(tick_no);
}

bool GetLocalCursor(int &x, int &y)
{
    if (!synced || !local_seen) return false;
    x = local_x; y = local_y;
    return true;
}

bool GetRemoteCursor(int &x, int &y)
{
    if (!synced) return false;
    if (cfg.role == kRoleHost)
    {
        if (!remote_seen) return false;
        x = remote_x; y = remote_y;
        return true;
    }
    if (!remote_seen) return false;
    x = remote_x; y = remote_y;
    return true;
}

String GetHudText()
{
    if (cfg.role == kRoleNone)
        return "";
    if (!hud_sticky && std::chrono::steady_clock::now() > hud_until)
        return "";
    String t = hud_text;
    if (cfg.role == kRoleHost && synced && rtt_ms > 0 && !hud_sticky)
        t.AppendFmt("  (ping %u ms)", rtt_ms);
    return t;
}

void ComputeStateHash(uint32_t out[6]) { NetPlayEngine::ComputeStateHash(out); }

// Automation port (localhost only) for unattended playtests.
static void CtlReply(const String &s)
{
    if (!ctl.IsOpen()) return;
    std::vector<uint8_t> b(s.GetCStr(), s.GetCStr() + s.GetLength());
    b.push_back('\n');
    ctl.outbuf.insert(ctl.outbuf.end(), b.begin(), b.end());
    ctl.Flush();
}

static void InjectMouse(int x, int y, int button, bool down, bool move_only)
{
    if (IsInputRouted())
    {
        NetEvent e;
        e.player = cfg.role == kRoleGuest ? 1 : 0;
        NoteLocalPos(x, y);
        if (move_only) { e.type = kEv_Move; e.x = (int16_t)x; e.y = (int16_t)y; }
        else { e.type = kEv_Button; e.x = (int16_t)x; e.y = (int16_t)y; e.mask = (uint8_t)button; e.press = down ? 1 : 0; }
        QueueLocal(e);
    }
    else
    {
        NetPlayEngine::InjectVanillaMouse(x, y, button, down, move_only);
    }
}

static void InjectKey(int scancode, int sym, int mod, bool down)
{
    if (IsInputRouted())
    {
        NetEvent e;
        e.player = cfg.role == kRoleGuest ? 1 : 0;
        e.type = kEv_Key; e.down = down ? 1 : 0; e.scancode = (uint16_t)scancode; e.sym = sym; e.mod = (uint16_t)mod;
        QueueLocal(e);
    }
    else
    {
        NetPlayEngine::InjectVanillaKey(scancode, sym, mod, down);
    }
}

static void HandleControlLine(const String &line)
{
    std::vector<String> tok = line.Split(' ');
    if (tok.empty()) return;
    const String cmd = tok[0];
    auto num = [&](size_t i, int def) { return i < tok.size() ? atoi(tok[i].GetCStr()) : def; };
    if (cmd == "move")
    {
        InjectMouse(num(1, 0), num(2, 0), 0, false, true);
        CtlReply("OK");
    }
    else if (cmd == "down" || cmd == "up")
    {
        int b = num(3, 1);
        InjectMouse(num(1, 0), num(2, 0), b == 2 ? 0x02 : 0x01, cmd == "down", false);
        CtlReply("OK");
    }
    else if (cmd == "click")
    {
        int b = num(3, 1);
        int mask = b == 2 ? 0x02 : (b == 3 ? 0x04 : 0x01);
        InjectMouse(num(1, 0), num(2, 0), 0, false, true);
        InjectMouse(num(1, 0), num(2, 0), mask, true, false);
        InjectMouse(num(1, 0), num(2, 0), mask, false, false);
        CtlReply("OK");
    }
    else if (cmd == "key")
    {
        // key <scancode> [sym] [mod]
        int sc = num(1, 0);
        int sym = tok.size() > 2 ? num(2, 0) : (int)SDL_GetKeyFromScancode((SDL_Scancode)sc);
        int mod = num(3, 0);
        InjectKey(sc, sym, mod, true);
        InjectKey(sc, sym, mod, false);
        CtlReply("OK");
    }
    else if (cmd == "keydown" || cmd == "keyup")
    {
        int sc = num(1, 0);
        InjectKey(sc, (int)SDL_GetKeyFromScancode((SDL_Scancode)sc), num(2, 0), cmd == "keydown");
        CtlReply("OK");
    }
    else if (cmd == "text")
    {
        String txt = line.Mid(5);
        if (IsInputRouted())
        {
            NetEvent e; e.player = cfg.role == kRoleGuest ? 1 : 0; e.type = kEv_Text; e.text = txt;
            QueueLocal(e);
        }
        else NetPlayEngine::InjectVanillaText(txt.GetCStr());
        CtlReply("OK");
    }
    else if (cmd == "state")
    {
        uint32_t h[6];
        NetPlayEngine::ComputeStateHash(h);
        String s = String::FromFormat("OK tick=%u role=%d synced=%d phase=%d records=%u desyncs=%u hash=%08x%08x%08x%08x%08x%08x mouse=%d,%d %s",
            tick_no, (int)cfg.role, synced ? 1 : 0, (int)gphase, (unsigned)records.size(), desync_count,
            h[0], h[1], h[2], h[3], h[4], h[5], logical_x, logical_y, NetPlayEngine::DescribeState().GetCStr());
        CtlReply(s);
    }
    else if (cmd == "shot")
    {
        NetPlayEngine::SaveScreenshot(line.Mid(5));
        CtlReply("OK");
    }
    else if (cmd == "resync")
    {
        if (cfg.role == kRoleGuest && gphase == kGuest_Synced) FlagDesync("requested by automation");
        CtlReply("OK");
    }
    else if (cmd == "quit")
    {
        CtlReply("OK");
        Shutdown();
        ExitProcess(0);
    }
    else
    {
        CtlReply("ERR unknown command");
    }
}

static void PollControl()
{
    if (ctl_listen == INVALID_SOCKET) return;
    if (!ctl.IsOpen())
    {
        SOCKET s = AcceptOne(ctl_listen);
        if (s == INVALID_SOCKET) return;
        ctl.Attach(s);
        ctl_line.clear();
    }
    if (!ctl.Pump())
        return;
    for (uint8_t c : ctl.inbuf)
    {
        if (c == '\n')
        {
            if (!ctl_line.empty() && ctl_line.back() == '\r') ctl_line.pop_back();
            HandleControlLine(String(ctl_line.c_str()));
            ctl_line.clear();
        }
        else ctl_line.push_back((char)c);
    }
    ctl.inbuf.clear();
}

} // namespace NetPlay

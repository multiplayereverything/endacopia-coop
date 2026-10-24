#include "net/netplay_engine.h"
#include <memory>
#include <SDL.h>
#include "ac/common.h"
#include "ac/characterinfo.h"
#include "ac/draw.h"
#include "ac/event.h"
#include "ac/game.h"
#include "ac/gamesetup.h"
#include "ac/gamesetupstruct.h"
#include "ac/gamestate.h"
#include "ac/global_game.h"
#include "ac/mouse.h"
#include "ac/path_helper.h"
#include "ac/roomstatus.h"
#include "ac/screen.h"
#include "ac/sys_events.h"
#include "ac/translation.h"
#include "debug/out.h"
#include "device/mousew32.h"
#include "game/savegame.h"
#include "gfx/bitmap.h"
#include "gfx/graphicsdriver.h"
#include "gui/guimain.h"
#include "main/engine.h"
#include "main/graphics_mode.h"
#include "media/audio/audio.h"
#include "media/audio/soundclip.h"
#include "script/cc_instance.h"
#include "script/cc_internal.h"
#include "script/script.h"
#include "script/systemimports.h"
#include <deque>
#include "util/file.h"
#include "util/stream.h"

using namespace AGS::Common;
using namespace AGS::Engine;

extern GameSetupStruct game;
extern GameState play;
extern int displayed_room;
extern RoomStatus *croom;
extern CharacterInfo *playerchar;
extern std::vector<GUIMain> guis;
extern int cur_mode, cur_cursor;
extern int in_new_room;
extern int game_paused;
extern RGB palette[256];
extern int frames_per_second;
extern int ifacepopped;
extern unsigned int loopcounter;
extern int our_eip;
extern AGS::Common::Bitmap *saved_viewport_bitmap;
extern IGraphicsDriver *gfxDriver;
void render_to_screen();

// sys_events.cpp netplay hooks
void ags_net_push_key_event(const SDL_Event &evt);
void ags_net_set_mouse_buttons(int state, int accum);
void ags_net_add_wheel(int dz);
void ags_net_reset_input_history();
void reset_gui_mouse_hold();

bool netplay_watch_enabled = false;
static int watch_script = -1, watch_offset = 0;
static std::deque<std::pair<RuntimeScriptValue, int32_t>> recent_calls;

void netplay_note_extcall(const RuntimeScriptValue &fn, int32_t ret)
{
    recent_calls.emplace_back(fn, ret);
    if (recent_calls.size() > 40) recent_calls.pop_front();
}

namespace NetPlayEngine { static void ScriptInstances(std::vector<ccInstance*> &v); }

void netplay_watch_check(const ccInstance *inst, const char *addr, int32_t value, int line)
{
    std::vector<ccInstance*> insts;
    NetPlayEngine::ScriptInstances(insts);
    if (watch_script < 0 || watch_script >= (int)insts.size() || !insts[watch_script] || !insts[watch_script]->globaldata)
        return;
    // Watches the whole 64-byte chunk containing the offset.
    const char *base = insts[watch_script]->globaldata + (watch_offset & ~63);
    if (addr < base || addr >= base + 64)
        return;
    String calls;
    for (const auto &c : recent_calls)
        calls.AppendFmt("%s=%d ", simp.findName(c.first).GetCStr(), c.second);
    Debug::Printf(kDbgMsg_Info, "[MP] watch: +%d = %d (0x%x) at line %d, loop %u; recent API calls: %s",
        (int)(addr - insts[watch_script]->globaldata), value, value, line, loopcounter, calls.GetCStr());
}

namespace NetPlayEngine
{

void CheckWatchPerFrame(uint32_t tick)
{
    if (!netplay_watch_enabled) return;
    static std::vector<uint8_t> last;
    std::vector<ccInstance*> insts;
    ScriptInstances(insts);
    if (watch_script >= (int)insts.size() || !insts[watch_script] || !insts[watch_script]->globaldata) return;
    const uint8_t *p = (const uint8_t*)insts[watch_script]->globaldata + (watch_offset & ~63);
    std::vector<uint8_t> now(p, p + 64);
    if (now == last) return;
    String vals, calls;
    for (int b = 0; b < 64; b += 4) vals.AppendFmt("%08x ", *(const uint32_t*)&now[b]);
    for (const auto &c : recent_calls)
        calls.AppendFmt("%s=%d ", simp.findName(c.first).GetCStr(), c.second);
    Debug::Printf(kDbgMsg_Info, "[MP] watch tick %u room %d: %s| calls: %s", tick, displayed_room, vals.GetCStr(), calls.GetCStr());
    last = now;
}

void SetWatch(int script, int offset)
{
    watch_script = script;
    watch_offset = offset;
    netplay_watch_enabled = script >= 0;
    Debug::Printf(kDbgMsg_Info, "[MP] watching script %d offset %d", script, offset);
}

void PushKeyEvent(const SDL_Event &evt) { ags_net_push_key_event(evt); }
void SetMouseButtons(int state, int accum) { ags_net_set_mouse_buttons(state, accum); }
void AddMouseWheel(int dz) { ags_net_add_wheel(dz); }

void ResetInputState()
{
    ags_clear_input_state();
    ags_net_set_mouse_buttons(0, 0);
    ags_net_reset_input_history();
    reset_gui_mouse_hold();
}

void WindowToGame(int &x, int &y)
{
    Mouse::WindowToGameClamped(x, y);
}

static void GameToWindow(int gx, int gy, int &wx, int &wy)
{
    Mouse::GameToWindow(gx, gy, wx, wy);
}

void InjectVanillaMouse(int gx, int gy, int button, bool down, bool move_only)
{
    int wx, wy;
    GameToWindow(gx, gy, wx, wy);
    SDL_Event e = {};
    if (move_only)
    {
        e.type = SDL_MOUSEMOTION;
        e.motion.x = wx; e.motion.y = wy;
    }
    else
    {
        e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.button = button == 0x02 ? SDL_BUTTON_RIGHT : (button == 0x04 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT);
        e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.button.x = wx; e.button.y = wy;
    }
    SDL_PushEvent(&e);
}

void InjectVanillaKey(int scancode, int sym, int mod, bool down)
{
    SDL_Event e = {};
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    e.key.keysym.sym = sym;
    e.key.keysym.mod = (Uint16)mod;
    SDL_PushEvent(&e);
}

void InjectVanillaText(const char *text)
{
    SDL_Event e = {};
    e.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof(e.text.text), "%s", text);
    SDL_PushEvent(&e);
}

void ToggleFullscreen()
{
    engine_try_switch_windowed_gfxmode();
}

void RenderWaitScreen()
{
    gfxDriver->ClearDrawLists();
    render_to_screen();
}

String GetGameGuid() { return game.guid; }
String GetTranslationName() { return get_translation_name(); }
void ChangeTranslation(const String &name) { Game_ChangeTranslation(name.GetCStr()); }
String GetUserDataDir() { return get_save_game_directory(); }
String GetAppDataDir() { return GetGameAppDataDir().FullDir; }

bool IsIdle()
{
    return displayed_room >= 0 && inside_script == 0 && num_scripts == 0 &&
        play.fast_forward == 0 && in_new_room == 0;
}

bool IsGameLoaded() { return displayed_room >= 0; }

void CancelScripts()
{
    cancel_all_scripts();
}

String DescribeState()
{
    String s = String::FromFormat("room=%d px=%d py=%d mode=%d cursor=%d inscript=%d iface=%d paused=%d cutscene=%d overlay=%d",
        displayed_room, playerchar ? playerchar->x : -1, playerchar ? playerchar->y : -1,
        cur_mode, cur_cursor, inside_script, play.disabled_user_interface, game_paused,
        play.in_cutscene, play.text_overlay_on);
    s.Append(" guis=");
    for (size_t i = 0; i < guis.size(); ++i)
        if (guis[i].IsDisplayed())
            s.AppendFmt("%s,", guis[i].Name.GetCStr());
    return s;
}

void SaveScreenshot(const String &path)
{
    std::unique_ptr<Bitmap> buffer(CopyScreenIntoBitmap(play.GetMainViewport().GetWidth(), play.GetMainViewport().GetHeight()));
    if (buffer)
        buffer->SaveToFile(path, palette);
}

int GetCurrentFrameRate() { return frames_per_second; }

int GetEip() { return our_eip; }

String DescribeDynamicSprites()
{
    String s;
    int n = 0;
    for (size_t i = 1; i < game.SpriteInfos.size(); ++i)
    {
        if (!(game.SpriteInfos[i].Flags & SPF_DYNAMICALLOC)) continue;
        if (n++ < 40) s.AppendFmt("%u:%dx%d ", (unsigned)i, game.SpriteInfos[i].Width, game.SpriteInfos[i].Height);
    }
    return String::FromFormat("%d dynamic sprites: %s", n, s.GetCStr());
}

String DescribeMisc()
{
    return String::FromFormat("score=%d cutscene=%d ff=%d paused=%d roomchanges=%d fps=%d wait=%d",
        play.score, play.in_cutscene, play.fast_forward, game_paused, play.room_changes, frames_per_second, play.wait_counter);
}

bool MakeSnapshot(const String &tmp_path, std::vector<uint8_t> &bytes)
{
    {
        std::unique_ptr<Stream> out(StartSavegame(tmp_path, "MP snapshot", nullptr));
        if (!out)
            return false;
        SaveGameState(out.get());
    }
    std::unique_ptr<Stream> in(File::OpenFileRead(tmp_path));
    if (!in)
        return false;
    bytes.resize((size_t)in->GetLength());
    if (!bytes.empty())
        in->Read(bytes.data(), bytes.size());
    return true;
}

void RestoreSnapshot(const String &path, int slot)
{
    // Start from a black screen on both peers: a fade-out from whatever each one shows takes different frame counts.
    play.screen_is_faded_out = 1;
    // Pending engine events aren't saved; a fresh guest and a running host would replay different ones.
    Debug::Printf(kDbgMsg_Info, "[MP] restore: dropping %u pending events", (unsigned)events.size());
    events.clear();
    try_restore_save(path, slot);
    // Crossfade/dissolve need the previous screen (not saved); whether it exists changes the fade length,
    // so both peers get the same black one.
    delete saved_viewport_bitmap;
    const Rect &vp = play.GetMainViewport();
    saved_viewport_bitmap = BitmapHelper::CreateBitmap(vp.GetWidth(), vp.GetHeight(), game.GetColorDepth());
    saved_viewport_bitmap->Clear();
}

struct Fnv
{
    uint32_t h = 2166136261u;
    void bytes(const void *p, size_t n)
    {
        const uint8_t *c = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 16777619u; }
    }
    void i(int v) { bytes(&v, sizeof(v)); }
};

static void HashScript(Fnv &f, ccInstance *inst)
{
    if (inst && inst->globaldata && inst->globaldatasize > 0)
        f.bytes(inst->globaldata, inst->globaldatasize);
}

static void ScriptInstances(std::vector<ccInstance*> &v)
{
    v.push_back(gameinst);
    for (auto *inst : moduleInst)
        v.push_back(inst);
    v.push_back(roominst);
}

static const size_t GLOBAL_CHUNK = 64;

void CopyGlobals(std::vector<uint8_t> &out)
{
    out.clear();
    std::vector<ccInstance*> insts;
    ScriptInstances(insts);
    for (auto *inst : insts)
        if (inst && inst->globaldata)
        {
            // Padded per script so offsets line up with ComputeGlobalChunks indices.
            out.insert(out.end(), inst->globaldata, inst->globaldata + inst->globaldatasize);
            out.resize((out.size() + GLOBAL_CHUNK - 1) / GLOBAL_CHUNK * GLOBAL_CHUNK, 0);
        }
}

void ComputeGlobalChunks(std::vector<uint32_t> &out)
{
    out.clear();
    std::vector<ccInstance*> insts;
    ScriptInstances(insts);
    for (auto *inst : insts)
    {
        if (!inst || !inst->globaldata) continue;
        for (int32_t at = 0; at < inst->globaldatasize; at += GLOBAL_CHUNK)
        {
            Fnv f;
            f.bytes(inst->globaldata + at, std::min<size_t>(GLOBAL_CHUNK, inst->globaldatasize - at));
            out.push_back(f.h);
        }
    }
}

String DescribeGlobalChunk(size_t chunk)
{
    std::vector<ccInstance*> insts;
    ScriptInstances(insts);
    for (size_t i = 0; i < insts.size(); ++i)
    {
        ccInstance *inst = insts[i];
        if (!inst || !inst->globaldata) continue;
        size_t n = (inst->globaldatasize + GLOBAL_CHUNK - 1) / GLOBAL_CHUNK;
        if (chunk >= n) { chunk -= n; continue; }
        const int32_t off = (int32_t)(chunk * GLOBAL_CHUNK);
        String sym = "?";
        int32_t best = -1;
        const ccScript *sc = inst->instanceof.get();
        for (int k = 0; sc && k < sc->numexports; ++k)
        {
            const int32_t addr = sc->export_addr[k];
            const int32_t eoff = addr & 0xFFFFFF;
            if ((addr >> 24) == EXPORT_DATA && eoff <= off + (int32_t)GLOBAL_CHUNK - 1 && eoff > best)
            {
                best = eoff;
                sym = sc->exports[k];
            }
        }
        String bytes;
        for (int32_t b = off; b < off + (int32_t)GLOBAL_CHUNK && b < inst->globaldatasize; b += 4)
            bytes.AppendFmt("%08x ", *(const uint32_t*)(inst->globaldata + b));
        return String::FromFormat("script %u offset %d (last export at/before: %s @%d): %s", (unsigned)i, off, sym.GetCStr(), best, bytes.GetCStr());
    }
    return "out of range";
}

void ComputeStateHash(uint32_t out[6])
{
    Fnv room, chars, objs, globals, gui, misc;

    room.i(displayed_room);
    room.i(game.playercharacter);
    if (playerchar) { room.i(playerchar->x); room.i(playerchar->y); room.i(playerchar->room); }

    for (int i = 0; i < game.numcharacters; ++i)
    {
        const CharacterInfo &c = game.chars[i];
        chars.i(c.x); chars.i(c.y); chars.i(c.z); chars.i(c.room);
        chars.i(c.loop); chars.i(c.frame); chars.i(c.view);
        chars.i(c.walking); chars.i(c.animating); chars.i(c.flags); chars.i(c.on);
        chars.i(c.activeinv);
        chars.bytes(c.inv, sizeof(c.inv));
    }

    if (croom && displayed_room >= 0)
    {
        for (uint32_t i = 0; i < croom->numobj && i < croom->obj.size(); ++i)
        {
            const RoomObject &o = croom->obj[i];
            objs.i(o.x); objs.i(o.y); objs.i(o.num); objs.i(o.on);
            objs.i(o.cycling); objs.i(o.moving); objs.i(o.view); objs.i(o.loop); objs.i(o.frame);
            objs.i(o.flags); objs.i(o.transparent); objs.i(o.baseline);
        }
    }

    HashScript(globals, gameinst);
    for (auto *inst : moduleInst)
        HashScript(globals, inst);
    HashScript(globals, roominst);
    globals.bytes(play.globalvars, sizeof(play.globalvars));

    for (const auto &g : guis)
    {
        gui.i(g.IsDisplayed() ? 1 : 0);
        gui.i(g.X); gui.i(g.Y);
        gui.i(g.Transparency);
    }
    gui.i(cur_mode);
    gui.i(cur_cursor);
    gui.i(play.disabled_user_interface);
    gui.i(ifacepopped);

    misc.i(play.score);
    misc.i(play.in_cutscene);
    misc.i(play.fast_forward);
    misc.i(game_paused);
    // play.room_changes is per-session and only compared relatively, so it is not hashed.
    misc.i(frames_per_second);
    misc.i(play.wait_counter);
    misc.i(play.screen_is_faded_out);
    for (size_t i = 1; i < game.SpriteInfos.size(); ++i)
        if (game.SpriteInfos[i].Flags & SPF_DYNAMICALLOC)
            misc.i((int)i);
    for (int ch = 0; ch < TOTAL_AUDIO_CHANNELS; ++ch)
    {
        SOUNDCLIP *clip = AudioChans::GetChannel(ch);
        misc.i(clip ? clip->sourceClipID : -1);
    }

    out[0] = room.h; out[1] = chars.h; out[2] = objs.h;
    out[3] = globals.h; out[4] = gui.h; out[5] = misc.h;
}

} // namespace NetPlayEngine

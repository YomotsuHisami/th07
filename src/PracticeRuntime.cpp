#include "PracticeRuntime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL_iostream.h>

#include "AnmManager.hpp"
#include "AsciiManager.hpp"
#include "Controller.hpp"
#include "EnemyManager.hpp"
#include "FileSystem.hpp"
#include "GameManager.hpp"
#include "ScreenEffect.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include "utils.hpp"
#if defined(THPRAC_PORTABLE_ENABLED)
#include "section_catalog.hpp"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace PracticeRuntime
{
static Config g_Config;
static MenuResult g_MenuResult = MenuResult::Waiting;
static bool g_MenuOpen = false;
static i32 g_MenuCursor = 0;
static i32 g_MenuDifficulty = 0;
static i32 g_MenuShotType = 0;
static i32 g_MenuSectionIndex = 0;
static constexpr i32 kMenuItemCount = 20;
static bool g_PauseWasOpen = false;
static bool g_PauseSettings = false;
static i32 g_PauseCursor = 0;
static bool g_PreserveConfigOnRestart = false;
#if !defined(__EMSCRIPTEN__)
static bool LoadDesktopSession();
#endif

template <typename T> static T Clamp(T value, T minimum, T maximum)
{
    return std::max(minimum, std::min(maximum, value));
}

#ifdef __EMSCRIPTEN__
static double HostNumber(const char *key, double fallback)
{
    return EM_ASM_DOUBLE({
        const session = Module.eaglerOptions?.thpracSession;
        const value = session?.params?.[UTF8ToString($0)];
        return Number.isFinite(Number(value)) ? Number(value) : $1;
    }, key, fallback);
}

static bool HostBool(const char *key, bool fallback)
{
    return EM_ASM_INT({
        const session = Module.eaglerOptions?.thpracSession;
        const value = session?.params?.[UTF8ToString($0)];
        return typeof value === 'boolean' ? value : !!$1;
    }, key, fallback) != 0;
}

static void PublishConfigToHost()
{
    char json[2304];
    std::snprintf(json, sizeof(json),
        "{\"schema\":\"eagler-touhou/thprac-session/1\",\"game\":\"th07\",\"params\":{"
        "\"mode\":%d,\"stage\":%d,\"warp\":%d,\"section\":%d,\"phase\":%d,\"frame\":%d,"
        "\"dlg\":%s,\"score\":%lld,\"life\":%d,\"bomb\":%d,\"power\":%d,\"graze\":%d,"
        "\"point\":%d,\"point_total\":%d,\"point_stage\":%d,\"cherry\":%d,\"cherryMax\":%d,"
        "\"cherryPlus\":%d,\"spellBonus\":%d,\"rank\":%d,\"rankLock\":%s}}",
        g_Config.mode, g_Config.stage, g_Config.warp, g_Config.section, g_Config.phase, g_Config.frame,
        g_Config.dialogue ? "true" : "false", static_cast<long long>(g_Config.score), g_Config.life,
        g_Config.bomb, g_Config.power, g_Config.graze, g_Config.point, g_Config.pointTotal,
        g_Config.pointStage, g_Config.cherry, g_Config.cherryMax, g_Config.cherryPlus,
        g_Config.spellBonus, g_Config.rank, g_Config.rankLock ? "true" : "false");
    EM_ASM({
        Module.eaglerOptions = Module.eaglerOptions || {};
        Module.eaglerOptions.thpracSession = JSON.parse(UTF8ToString($0));
    }, json);
}
#endif

void SetConfig(const Config &config)
{
    g_Config = config;
    g_Config.mode = Clamp(g_Config.mode, 0, 1);
    g_Config.stage = Clamp(g_Config.stage, 0, 7);
    g_Config.warp = Clamp(g_Config.warp, 0, 8);
    g_Config.section = Clamp(g_Config.section, 0, 19999);
    g_Config.phase = Clamp(g_Config.phase, 0, 64);
    g_Config.frame = std::max(0, g_Config.frame);
    g_Config.score = Clamp<i64>(g_Config.score, 0, 9999999990LL);
    g_Config.life = Clamp(g_Config.life, 0, 8);
    g_Config.bomb = Clamp(g_Config.bomb, 0, 8);
    g_Config.power = Clamp(g_Config.power, 0, 128);
    g_Config.graze = Clamp(g_Config.graze, 0, 99999);
    g_Config.point = Clamp(g_Config.point, 0, 9999);
    g_Config.pointTotal = Clamp(g_Config.pointTotal, 0, 9999);
    g_Config.pointStage = Clamp(g_Config.pointStage, 0, 9999);
    g_Config.cherry = Clamp(g_Config.cherry, 0, 9999990);
    g_Config.cherryMax = Clamp(g_Config.cherryMax, 0, 9999990);
    g_Config.cherryPlus = Clamp(g_Config.cherryPlus, 0, 50000);
    g_Config.spellBonus = Clamp(g_Config.spellBonus, 0, 30);
    // Upstream permits 99 only while rank lock is enabled. Easy uses a
    // narrower unlocked 12..20 range; the final difficulty-aware clamp is
    // performed when the game state is available.
    g_Config.rank = Clamp(g_Config.rank, 10, 99);
}

void RefreshFromHost()
{
    if (g_PreserveConfigOnRestart)
    {
        g_PreserveConfigOnRestart = false;
        return;
    }
#ifdef __EMSCRIPTEN__
    const bool active = EM_ASM_INT({
        const session = Module.eaglerOptions?.thpracSession;
        return !!session && session.game === 'th07' &&
            (session.schema === 'eagler-touhou/thprac-session/1' ||
             session.schema === 'eagler-touhou/thprac-replay/1');
    }) != 0;
    if (!active)
    {
        g_Config = {};
        return;
    }
    Config config;
    config.active = true;
    config.mode = static_cast<i32>(HostNumber("mode", 1));
    config.stage = static_cast<i32>(HostNumber("stage", 0));
    config.warp = static_cast<i32>(HostNumber("warp", 0));
    config.section = static_cast<i32>(HostNumber("section", 0));
    config.phase = static_cast<i32>(HostNumber("phase", 0));
    config.frame = static_cast<i32>(HostNumber("frame", 0));
    config.dialogue = HostBool("dlg", false);
    config.score = static_cast<i64>(HostNumber("score", 0));
    config.life = static_cast<i32>(HostNumber("life", 8));
    config.bomb = static_cast<i32>(HostNumber("bomb", 8));
    config.power = static_cast<i32>(HostNumber("power", 128));
    config.graze = static_cast<i32>(HostNumber("graze", 0));
    config.point = static_cast<i32>(HostNumber("point", 0));
    config.pointTotal = static_cast<i32>(HostNumber("point_total", 0));
    config.pointStage = static_cast<i32>(HostNumber("point_stage", 0));
    config.cherry = static_cast<i32>(HostNumber("cherry", 0));
    config.cherryMax = static_cast<i32>(HostNumber("cherryMax", 200000));
    config.cherryPlus = static_cast<i32>(HostNumber("cherryPlus", 0));
    config.spellBonus = static_cast<i32>(HostNumber("spellBonus", 0));
    config.rank = static_cast<i32>(HostNumber("rank", 16));
    config.rankLock = HostBool("rankLock", false);
    SetConfig(config);
#else
    LoadDesktopSession();
#endif
}

const Config &GetConfig()
{
    return g_Config;
}

bool Active()
{
    return g_Config.active;
}

bool Enabled()
{
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({ return !!Module.eaglerOptions?.thpracEnabled; }) != 0;
#else
    SDL_IOStream *file = SDL_IOFromFile("thprac-session.json", "rb");
    if (!file)
        return false;
    SDL_CloseIO(file);
    return true;
#endif
}

void OpenPracticeMenu(i32 difficulty, i32 shotType)
{
#ifdef __EMSCRIPTEN__
    RefreshFromHost();
#else
    LoadDesktopSession();
#endif
    if (!g_Config.active)
    {
        g_Config = {};
        g_Config.mode = 1;
        g_Config.life = 8;
        g_Config.bomb = 8;
        g_Config.power = 128;
        g_Config.cherryMax = 200000;
        g_Config.rank = 16;
    }
    g_MenuDifficulty = Clamp(difficulty, 0, 5);
    g_MenuShotType = shotType;
    g_MenuCursor = g_MenuSectionIndex = 0;
    g_MenuOpen = true;
    g_MenuResult = MenuResult::Waiting;
}

static const thprac::portable::generated::SectionLabel *CurrentSection(i32 offset = 0)
{
#if defined(THPRAC_PORTABLE_ENABLED)
    using namespace thprac::portable::generated;
    const std::uint32_t difficultyBit = 1u << Clamp(g_MenuDifficulty, 0, 5);
    const SectionLabel *matches[180] = {};
    i32 count = 0;
    i32 lastId = -1;
    for (const SectionLabel &entry : th07SectionLabels)
    {
        if (entry.stage != g_Config.stage || !(entry.difficultyMask & difficultyBit) || entry.id == lastId)
            continue;
        if ((g_Config.warp == 2 && entry.bgm != 0) || (g_Config.warp == 3 && entry.bgm != 1) ||
            (g_Config.warp == 4 && entry.spell != 0) || (g_Config.warp == 5 && entry.spell != 1))
            continue;
        matches[count++] = &entry;
        lastId = entry.id;
    }
    if (!count) return nullptr;
    g_MenuSectionIndex = (g_MenuSectionIndex + offset + count) % count;
    const SectionLabel *selected = matches[g_MenuSectionIndex];
    g_Config.section = selected->id;
    g_Config.dialogue = selected->dialogue && g_Config.dialogue;
    return selected;
#else
    (void)offset; return nullptr;
#endif
}

static i32 VisibleMenuCount()
{
    return g_Config.mode == 0 ? 3 : kMenuItemCount;
}

static i32 VisibleMenuItem(i32 cursor)
{
    static constexpr i32 stagePracticeItems[] = {0, 1, kMenuItemCount - 1};
    return g_Config.mode == 0 ? stagePracticeItems[Clamp(cursor, 0, 2)] : cursor;
}

static void AdjustMenuValue(i32 item, i32 direction)
{
    switch (item)
    {
    case 0: g_Config.mode = Clamp(g_Config.mode + direction, 0, 1); break;
    case 1: g_Config.stage = Clamp(g_Config.stage + direction, 0, 7); g_MenuSectionIndex = 0; break;
    case 2: g_Config.warp = Clamp(g_Config.warp + direction, 0, 6); g_MenuSectionIndex = 0; break;
    case 3: CurrentSection(direction); break;
    case 4: g_Config.phase = Clamp(g_Config.phase + direction, 0, 64); break;
    case 5: g_Config.dialogue = !g_Config.dialogue; break;
    case 6: g_Config.life = Clamp(g_Config.life + direction, 0, 8); break;
    case 7: g_Config.bomb = Clamp(g_Config.bomb + direction, 0, 8); break;
    case 8: g_Config.power = Clamp(g_Config.power + direction * 8, 0, 128); break;
    case 9: g_Config.score = Clamp<i64>(g_Config.score + direction * 1000000LL, 0, 9999999990LL); break;
    case 10: g_Config.graze = Clamp(g_Config.graze + direction * 100, 0, 99999); break;
    case 11: g_Config.pointTotal = Clamp(g_Config.pointTotal + direction * 10, 0, 9999); break;
    case 12: g_Config.pointStage = Clamp(g_Config.pointStage + direction * 10, 0, 9999); break;
    case 13: g_Config.cherry = Clamp(g_Config.cherry + direction * 10000, 0, 9999990); break;
    case 14: g_Config.cherryMax = Clamp(g_Config.cherryMax + direction * 10000, 0, 9999990); break;
    case 15: g_Config.cherryPlus = Clamp(g_Config.cherryPlus + direction * 1000, 0, 50000); break;
    case 16: g_Config.spellBonus = Clamp(g_Config.spellBonus + direction, 0, 30); break;
    case 17: g_Config.rank = Clamp(g_Config.rank + direction, 10, 99); break;
    case 18: g_Config.rankLock = !g_Config.rankLock; break;
    }
    CurrentSection();
}

MenuResult PollPracticeMenu()
{
    if (g_MenuOpen)
    {
        if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_UP)) { const i32 count=VisibleMenuCount(); g_MenuCursor=(g_MenuCursor+count-1)%count; }
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_DOWN)) g_MenuCursor=(g_MenuCursor+1)%VisibleMenuCount();
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_LEFT)) { AdjustMenuValue(VisibleMenuItem(g_MenuCursor),-1); g_MenuCursor=Clamp(g_MenuCursor,0,VisibleMenuCount()-1); }
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_RIGHT)) { AdjustMenuValue(VisibleMenuItem(g_MenuCursor),1); g_MenuCursor=Clamp(g_MenuCursor,0,VisibleMenuCount()-1); }
        else if (WAS_PRESSED_RAW(TH_BUTTON_RETURNMENU)) { g_MenuOpen = false; return MenuResult::Cancelled; }
        else if (WAS_PRESSED_RAW(TH_BUTTON_SELECTMENU))
        {
            if (VisibleMenuItem(g_MenuCursor) == kMenuItemCount - 1)
            {
                g_Config.active = true; CurrentSection();
#ifdef __EMSCRIPTEN__
                PublishConfigToHost();
#endif
                g_MenuOpen = false; return MenuResult::Accepted;
            }
            AdjustMenuValue(VisibleMenuItem(g_MenuCursor), 1);
            g_MenuCursor = Clamp(g_MenuCursor, 0, VisibleMenuCount() - 1);
        }
        return MenuResult::Waiting;
    }
    if (g_MenuResult != MenuResult::Waiting)
    {
        const MenuResult result = g_MenuResult;
        g_MenuResult = MenuResult::Waiting;
        return result;
    }
    return MenuResult::Waiting;
}

void DrawPracticeMenu()
{
    if (!g_MenuOpen) return;
    ZunRect panel {160.0f, 32.0f, 632.0f, 448.0f};
    ScreenEffect::DrawSquare(&panel, 0xff181820);
    const auto *section = CurrentSection();
    const char *sectionName = section ? section->en : "Whole stage";
    const char *warpNames[] = {"Whole stage", "Chapter", "Midboss", "Boss", "Nonspell", "Spell", "Frame"};
    char lines[kMenuItemCount][64] = {};
    std::snprintf(lines[0],64,"Mode         %s",g_Config.mode?"Advanced practice":"Stage practice");
    std::snprintf(lines[1],64,"Stage        %d",g_Config.stage+1);
    std::snprintf(lines[2],64,"Warp         %s",warpNames[Clamp(g_Config.warp,0,6)]);
    std::snprintf(lines[3],64,"Section      %.44s",sectionName);
    std::snprintf(lines[4],64,"Phase        %d",g_Config.phase);
    std::snprintf(lines[5],64,"Dialogue     %s",g_Config.dialogue?"On":"Off");
    std::snprintf(lines[6],64,"Lives        %d",g_Config.life);
    std::snprintf(lines[7],64,"Bombs        %d",g_Config.bomb);
    std::snprintf(lines[8],64,"Power        %d",g_Config.power);
    std::snprintf(lines[9],64,"Score        %lld",static_cast<long long>(g_Config.score));
    std::snprintf(lines[10],64,"Graze        %d",g_Config.graze);
    std::snprintf(lines[11],64,"Point total  %d",g_Config.pointTotal);
    std::snprintf(lines[12],64,"Point stage  %d",g_Config.pointStage);
    std::snprintf(lines[13],64,"Cherry       %d",g_Config.cherry);
    std::snprintf(lines[14],64,"Cherry max   %d",g_Config.cherryMax);
    std::snprintf(lines[15],64,"Cherry plus  %d",g_Config.cherryPlus);
    std::snprintf(lines[16],64,"Spell bonus  %d",g_Config.spellBonus);
    std::snprintf(lines[17],64,"Rank         %d",g_Config.rank);
    std::snprintf(lines[18],64,"Rank lock    %s",g_Config.rankLock?"On":"Off");
    std::snprintf(lines[19],64,"Start advanced practice");
    const i32 visibleCount=VisibleMenuCount();
    const i32 shownRows=std::min(12,visibleCount);
    const i32 first=Clamp(g_MenuCursor-6,0,std::max(0,visibleCount-shownRows));
    ZunVec3 title(180.0f,52.0f,0.0f); g_AsciiManager.color=0xfff0c080;
    g_AsciiManager.AddString(&title,"thprac - Advanced Practice");
    static const char *diff[]={"Easy","Normal","Hard","Lunatic","Extra","Phantasm"};
    static const char *shots[]={"Reimu A","Reimu B","Marisa A","Marisa B","Sakuya A","Sakuya B"};
    ZunVec3 context(180.0f,76.0f,0.0f); g_AsciiManager.color=0xffc0c0c0;
    AsciiManager::AddFormatText(&g_AsciiManager,&context,"%s / %s",diff[Clamp(g_MenuDifficulty,0,5)],shots[Clamp(g_MenuShotType,0,5)]);
    for(i32 row=0;row<shownRows;row++) { const i32 visibleIndex=first+row; const i32 index=VisibleMenuItem(visibleIndex); ZunVec3 pos(180.0f,108.0f+row*24.0f,0.0f);
        g_AsciiManager.color=visibleIndex==g_MenuCursor?0xfff0f0c0:0xffa0c0c0;
        AsciiManager::AddFormatText(&g_AsciiManager,&pos,"%c %s",visibleIndex==g_MenuCursor?'>':' ',lines[index]); }
    g_AsciiManager.color=0xffffffff;
}

void DebugAcceptPracticeMenu()
{
    if (!g_MenuOpen)
        return;
    g_Config.active = true;
    CurrentSection();
    g_MenuOpen = false;
    g_MenuResult = MenuResult::Accepted;
}

bool UpdatePauseMenu()
{
    if (!Active() || !g_GameManager.isInPauseMenu || g_GameManager.replay)
    {
        g_PauseWasOpen = false;
        return false;
    }
    if (!g_PauseWasOpen)
    {
        g_PauseWasOpen = true;
        g_PauseSettings = false;
        g_PauseCursor = 0;
    }

    if (g_PauseSettings)
    {
        if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_UP))
            g_MenuCursor = (g_MenuCursor + 19) % 20;
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_DOWN))
            g_MenuCursor = (g_MenuCursor + 1) % 20;
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_LEFT))
            AdjustMenuValue(g_MenuCursor, -1);
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_RIGHT) || WAS_PRESSED_RAW(TH_BUTTON_SELECTMENU))
            AdjustMenuValue(g_MenuCursor, 1);
        else if (WAS_PRESSED_RAW(TH_BUTTON_RETURNMENU))
            g_PauseSettings = false;
    }
    else
    {
        if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_UP))
            g_PauseCursor = (g_PauseCursor + 3) % 4;
        else if (WAS_PRESSED_RAW_AND_IS_EIGHTH(TH_BUTTON_DOWN))
            g_PauseCursor = (g_PauseCursor + 1) % 4;
        else if (WAS_PRESSED_RAW(TH_BUTTON_RETURNMENU))
            g_PauseCursor = 0;
        else if (WAS_PRESSED_RAW(TH_BUTTON_SELECTMENU))
        {
            switch (g_PauseCursor)
            {
            case 0:
                g_GameManager.isInPauseMenu = 0;
                g_PauseWasOpen = false;
                break;
            case 1:
                // Enter the game-complete transition, not the vanilla pause
                // exit-to-title transition. ResultScreen state 18 then opens
                // the normal Practice replay-save editor.
                g_GameManager.isInPauseMenu = 0;
                g_Supervisor.curState = 6;
                g_PauseWasOpen = false;
                break;
            case 2:
                // TH07's normal retry state reconstructs GameManager. The
                // already-selected in-memory parameters remain authoritative
                // across the next RefreshFromHost call.
                g_GameManager.isInPauseMenu = 0;
#ifdef __EMSCRIPTEN__
                PublishConfigToHost();
#endif
                g_PreserveConfigOnRestart = true;
                g_Supervisor.curState = 10;
                g_PauseWasOpen = false;
                break;
            case 3:
                g_PauseSettings = true;
                g_MenuCursor = 0;
                break;
            }
        }
    }

    ZunVec3 title(96.0f, 112.0f, 0.0f);
    g_AsciiManager.color = 0xfff0c080;
    AsciiManager::AddFormatText(&g_AsciiManager, &title, "%s",
                                g_PauseSettings ? "thprac - Settings" : "thprac - Pause Menu");
    if (g_PauseSettings)
    {
        const char *labels[] = {"Mode", "Stage", "Warp", "Section", "Phase", "Dialogue", "Lives",
                                "Bombs", "Power", "Score", "Graze", "Point total", "Point stage",
                                "Cherry", "Cherry max", "Cherry plus", "Spell bonus", "Rank", "Rank lock"};
        const i32 first = Clamp(g_MenuCursor - 5, 0, 11);
        for (i32 row = 0; row < 8; row++)
        {
            const i32 index = first + row;
            ZunVec3 pos(96.0f, 144.0f + row * 28.0f, 0.0f);
            g_AsciiManager.color = index == g_MenuCursor ? 0xfff0f0c0 : 0xffa0c0c0;
            AsciiManager::AddFormatText(&g_AsciiManager, &pos, "%c %s", index == g_MenuCursor ? '>' : ' ', labels[index]);
        }
    }
    else
    {
        const char *items[] = {"Resume", "Exit", "Restart", "Settings"};
        for (i32 index = 0; index < 4; index++)
        {
            ZunVec3 pos(128.0f, 168.0f + index * 44.0f, 0.0f);
            g_AsciiManager.color = index == g_PauseCursor ? 0xfff0f0c0 : 0xffa0c0c0;
            AsciiManager::AddFormatText(&g_AsciiManager, &pos, "%c %s", index == g_PauseCursor ? '>' : ' ', items[index]);
        }
    }
    g_AsciiManager.color = 0xffffffff;
    return true;
}

void DrawPauseMenuPanel()
{
    if (!Active() || !g_GameManager.isInPauseMenu)
        return;
    ZunRect panel {56.0f, 80.0f, 392.0f, 400.0f};
    ScreenEffect::DrawSquare(&panel, 0xff181820);
}

void PrepareStart(GameManager &gameManager)
{
    if (!Active() || gameManager.replay)
        return;
    gameManager.currentStage = g_Config.stage;
    gameManager.practice = 1;
    if (g_Config.stage == 6)
        gameManager.difficulty = 4;
    else if (g_Config.stage == 7)
        gameManager.difficulty = 5;
}

i32 ResolveWarpFrame(i32 stage, i32 portion)
{
    static constexpr i32 frames[][8] = {
        {540, 1341, 3107, 0, 0, 0, 0, 0},
        {390, 3366, 0, 0, 0, 0, 0, 0},
        {390, 854, 1805, 0, 0, 0, 0, 0},
        {80, 1948, 3028, 4288, 7964, 10136, 11396, 13166},
        {440, 840, 2550, 4883, 0, 0, 0, 0},
        {660, 1180, 0, 0, 0, 0, 0, 0},
        {485, 1035, 1955, 2615, 3305, 4028, 5808, 7088},
        {485, 965, 1595, 2485, 3191, 3928, 5708, 6988}
    };
    if (stage < 0 || stage >= 8 || portion < 1 || portion > 8)
        return 0;
    return frames[stage][portion - 1];
}

static void SetPointItems(GameManager &gameManager)
{
    ZunGlobals &globals = *gameManager.globals;
    if (g_Config.point)
        globals.pointItemsCollectedThisStage = globals.pointItemsCollectedForExtend = g_Config.point;
    else
    {
        globals.pointItemsCollectedThisStage = g_Config.pointStage;
        globals.pointItemsCollectedForExtend = g_Config.pointTotal;
    }
    globals.extendsFromPointItems = 0;
    while (true)
    {
        if (gameManager.difficulty >= 4)
            globals.nextNeededPointItemsForExtend = globals.extendsFromPointItems == 0 ? 200 :
                globals.extendsFromPointItems == 1 ? 500 : 500 * (globals.extendsFromPointItems - 2) + 800;
        else if (globals.extendsFromPointItems >= 3)
            globals.nextNeededPointItemsForExtend = globals.extendsFromPointItems >= 5 ?
                200 * (globals.extendsFromPointItems - 5) + 800 : 150 * (globals.extendsFromPointItems - 3) + 300;
        else
            globals.nextNeededPointItemsForExtend = 75 * globals.extendsFromPointItems + 50;
        if (globals.pointItemsCollectedForExtend < globals.nextNeededPointItemsForExtend)
            break;
        globals.extendsFromPointItems++;
    }
}

void ApplyInitialState(GameManager &gameManager, bool applyStats)
{
    // Upstream mode 0 is an unmodified whole-stage Practice run. Only mode 1
    // applies thprac parameters and ECL/time warps.
    if (!Active() || g_Config.mode != 1)
        return;
    if (applyStats && gameManager.globals)
    {
        gameManager.SetLivesRemaining(g_Config.life);
        gameManager.SetBombsRemainingAndComputeCsum(g_Config.bomb);
        gameManager.SetCurrentPower(g_Config.power);
        const u32 score = static_cast<u32>(g_Config.score / 10);
        gameManager.globals->guiScore = gameManager.globals->score = score;
        gameManager.globals->grazeInStage = gameManager.globals->grazeInTotal = g_Config.graze;
        gameManager.globals->spellCardsCaptured = g_Config.spellBonus;
        SetPointItems(gameManager);
        gameManager.cherry = gameManager.globals->cherryStart + g_Config.cherry;
        gameManager.cherryMax = gameManager.globals->cherryStart + g_Config.cherryMax;
        gameManager.cherryPlus = gameManager.globals->cherryStart + g_Config.cherryPlus;
        const i32 rankMinimum = gameManager.difficulty == 0 ? 12 : 10;
        const i32 rankMaximum = g_Config.rankLock ? 99 : gameManager.difficulty == 0 ? 20 : 32;
        const i32 rank = Clamp(g_Config.rank, rankMinimum, rankMaximum);
        gameManager.rank.rank = rank;
        if (g_Config.rankLock)
            gameManager.rank.minRank = gameManager.rank.maxRank = rank;
        gameManager.RegenerateGameIntegrityCsum();
    }
    i32 frame = g_Config.frame;
    if (frame == 0 && g_Config.section >= 10000)
    {
        const i32 encoded = g_Config.section - 10000;
        frame = ResolveWarpFrame(encoded / 100 - 1, encoded % 100);
    }
    if (frame == 0)
        frame = ResolveWarpFrame(g_Config.stage, g_Config.warp);
    if (frame > 0)
    {
        // Match thprac's stage-3 background correction before jumping the ECL
        // timelines, otherwise the enemies and background disagree.
        if (g_Config.stage == 2 && g_AnmManager && g_AnmManager->anmFiles[5].raw)
        {
            u16 *raw = reinterpret_cast<u16 *>(g_AnmManager->anmFiles[5].raw);
            raw[0xa0] = raw[0xd2] = raw[0x104] = raw[0x136] = 2;
        }
        const i32 count = g_Config.stage == 1 ? 3 : 2;
        for (i32 index = 0; index < count; index++)
            g_EnemyManager.timelines[index].timelineTime.current = frame;
    }
}

static double JsonNumber(const std::string &json, const char *key, double fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t position = json.find(needle);
    if (position == std::string::npos || (position = json.find(':', position + needle.size())) == std::string::npos)
        return fallback;
    const char *begin = json.c_str() + position + 1;
    char *end = nullptr;
    const double value = std::strtod(begin, &end);
    return end == begin ? fallback : value;
}

static bool JsonBool(const std::string &json, const char *key, bool fallback)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t position = json.find(needle);
    if (position == std::string::npos || (position = json.find(':', position + needle.size())) == std::string::npos)
        return fallback;
    position = json.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos)
        return fallback;
    if (json.compare(position, 4, "true") == 0)
        return true;
    if (json.compare(position, 5, "false") == 0)
        return false;
    return fallback;
}

static bool LoadConfigJson(const std::string &json)
{
    if ((json.find("\"schema\":\"eagler-touhou/thprac-session/1\"") == std::string::npos &&
         json.find("\"schema\":\"eagler-touhou/thprac-replay/1\"") == std::string::npos &&
         json.find("\"schema\":\"thprac/portable-replay/1\"") == std::string::npos) ||
        json.find("\"game\":\"th07\"") == std::string::npos)
        return false;
    Config config;
    config.active = true;
    config.mode = static_cast<i32>(JsonNumber(json, "mode", 1));
    config.stage = static_cast<i32>(JsonNumber(json, "stage", 0));
    config.warp = static_cast<i32>(JsonNumber(json, "warp", 0));
    config.section = static_cast<i32>(JsonNumber(json, "section", 0));
    config.phase = static_cast<i32>(JsonNumber(json, "phase", 0));
    config.frame = static_cast<i32>(JsonNumber(json, "frame", 0));
    config.dialogue = JsonBool(json, "dlg", false);
    config.score = static_cast<i64>(JsonNumber(json, "score", 0));
    config.life = static_cast<i32>(JsonNumber(json, "life", 8));
    config.bomb = static_cast<i32>(JsonNumber(json, "bomb", 8));
    config.power = static_cast<i32>(JsonNumber(json, "power", 128));
    config.graze = static_cast<i32>(JsonNumber(json, "graze", 0));
    config.point = static_cast<i32>(JsonNumber(json, "point", 0));
    config.pointTotal = static_cast<i32>(JsonNumber(json, "point_total", 0));
    config.pointStage = static_cast<i32>(JsonNumber(json, "point_stage", 0));
    config.cherry = static_cast<i32>(JsonNumber(json, "cherry", 0));
    config.cherryMax = static_cast<i32>(JsonNumber(json, "cherryMax", 200000));
    config.cherryPlus = static_cast<i32>(JsonNumber(json, "cherryPlus", 0));
    config.spellBonus = static_cast<i32>(JsonNumber(json, "spellBonus", 0));
    config.rank = static_cast<i32>(JsonNumber(json, "rank", 16));
    config.rankLock = JsonBool(json, "rankLock", false);
    SetConfig(config);
    return true;
}

#if !defined(__EMSCRIPTEN__)
#if defined(THPRAC_PORTABLE_ENABLED)
extern "C" bool ThpracPortableTh07SetSessionJson(const char *json);
#endif
static bool LoadDesktopSession()
{
    u8 *bytes = FileSystem::OpenFile("thprac-session.json", 1);
    if (!bytes) { g_Config = {}; return false; }
    const std::string json(reinterpret_cast<char *>(bytes), g_LastFileSize);
    free(bytes);
    if (!LoadConfigJson(json)) { g_Config = {}; return false; }
#if defined(THPRAC_PORTABLE_ENABLED)
    if (!ThpracPortableTh07SetSessionJson(json.c_str())) { g_Config = {}; return false; }
#endif
    return true;
}
#endif

bool LoadReplayMetadata(const char *replayPath)
{
    g_Config = {};
#if !defined(__EMSCRIPTEN__) && defined(THPRAC_PORTABLE_ENABLED)
    ThpracPortableTh07SetSessionJson(nullptr);
#endif
    if (!replayPath || !*replayPath)
        return false;
    const std::string sidecar = std::string(replayPath) + ".thprac.json";
    u8 *bytes = FileSystem::OpenFile(sidecar.c_str(), 1);
    if (!bytes)
        return false;
    const std::string json(reinterpret_cast<char *>(bytes), g_LastFileSize);
    free(bytes);
    if (!LoadConfigJson(json))
        return false;
#ifdef __EMSCRIPTEN__
    PublishConfigToHost();
#endif
#if !defined(__EMSCRIPTEN__) && defined(THPRAC_PORTABLE_ENABLED)
    if (!ThpracPortableTh07SetSessionJson(json.c_str()))
    {
        g_Config = {};
        return false;
    }
#endif
    return true;
}

bool SaveReplayMetadata(const char *replayPath)
{
    if (!Active() || !replayPath || !*replayPath)
        return false;
    const std::string sidecar = std::string(replayPath) + ".thprac.json";
    char json[2304];
    const int length = std::snprintf(json, sizeof(json),
        "{\"schema\":\"eagler-touhou/thprac-replay/1\",\"game\":\"th07\",\"source\":\"advanced-practice\","
        "\"params\":{\"mode\":%d,\"stage\":%d,\"warp\":%d,\"section\":%d,\"phase\":%d,\"frame\":%d,"
        "\"dlg\":%s,\"score\":%lld,\"life\":%d,\"bomb\":%d,\"power\":%d,\"graze\":%d,\"point\":%d,"
        "\"point_total\":%d,\"point_stage\":%d,\"cherry\":%d,\"cherryMax\":%d,\"cherryPlus\":%d,"
        "\"spellBonus\":%d,\"rank\":%d,\"rankLock\":%s}}\n",
        g_Config.mode, g_Config.stage, g_Config.warp, g_Config.section, g_Config.phase, g_Config.frame,
        g_Config.dialogue ? "true" : "false", static_cast<long long>(g_Config.score), g_Config.life,
        g_Config.bomb, g_Config.power, g_Config.graze, g_Config.point, g_Config.pointTotal,
        g_Config.pointStage, g_Config.cherry, g_Config.cherryMax, g_Config.cherryPlus,
        g_Config.spellBonus, g_Config.rank, g_Config.rankLock ? "true" : "false");
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(json))
        return false;
    SDL_IOStream *file = SDL_IOFromFile(sidecar.c_str(), "wb");
    if (!file)
        return false;
    const bool written = SDL_WriteIO(file, json, static_cast<size_t>(length)) == static_cast<size_t>(length);
    SDL_CloseIO(file);
    return written;
}

bool DebugReplayMetadataRoundTrip(const char *replayPath)
{
#if !defined(__EMSCRIPTEN__)
    Config expected;
    expected.active = true;
    expected.mode = 1;
    expected.stage = 5;
    expected.warp = 4;
    // Use a catalog-backed section so the portable adapter validates the same
    // payload that replay playback will consume.
    expected.section = 6;
    expected.phase = 2;
    expected.frame = 23456;
    expected.dialogue = true;
    expected.score = 87654320;
    expected.life = 4;
    expected.bomb = 2;
    expected.power = 112;
    expected.graze = 2300;
    expected.point = 450;
    expected.pointTotal = 460;
    expected.pointStage = 47;
    expected.cherry = 12000;
    expected.cherryMax = 321000;
    expected.cherryPlus = 8000;
    expected.spellBonus = 6;
    expected.rank = 28;
    expected.rankLock = true;
    SetConfig(expected);
    if (!SaveReplayMetadata(replayPath))
        return false;

    Config stale;
    stale.active = true;
    stale.stage = 1;
    stale.section = 999;
    SetConfig(stale);
    if (!LoadReplayMetadata(replayPath))
        return false;
    const Config &actual = GetConfig();
    return actual.active == expected.active && actual.mode == expected.mode && actual.stage == expected.stage &&
        actual.warp == expected.warp && actual.section == expected.section && actual.phase == expected.phase &&
        actual.frame == expected.frame && actual.dialogue == expected.dialogue && actual.score == expected.score &&
        actual.life == expected.life && actual.bomb == expected.bomb && actual.power == expected.power &&
        actual.graze == expected.graze && actual.point == expected.point && actual.pointTotal == expected.pointTotal &&
        actual.pointStage == expected.pointStage && actual.cherry == expected.cherry &&
        actual.cherryMax == expected.cherryMax && actual.cherryPlus == expected.cherryPlus &&
        actual.spellBonus == expected.spellBonus && actual.rank == expected.rank &&
        actual.rankLock == expected.rankLock;
#else
    (void)replayPath;
    return false;
#endif
}

bool DebugRestartPreservesConfig()
{
#if !defined(__EMSCRIPTEN__)
    Config expected;
    expected.active = true;
    expected.mode = 1;
    expected.stage = 4;
    expected.section = 6;
    expected.life = 3;
    expected.power = 104;
    expected.cherry = 54321;
    SetConfig(expected);
    g_PreserveConfigOnRestart = true;
    RefreshFromHost();
    const Config &actual = GetConfig();
    return actual.active && actual.mode == expected.mode && actual.stage == expected.stage &&
        actual.section == expected.section && actual.life == expected.life && actual.power == expected.power &&
        actual.cherry == expected.cherry && !g_PreserveConfigOnRestart;
#else
    return false;
#endif
}
} // namespace PracticeRuntime

#ifdef __EMSCRIPTEN__
#endif

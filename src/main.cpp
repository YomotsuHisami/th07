#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

// pull in gameerrorcontext::flush before anmmanager::releasesurfaces
#include "AnmManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "FileSystem.hpp"
#include "GameErrorContext.hpp"
#include "GameManager.hpp"
#include "GameWindow.hpp"
#include "MainMenu.hpp"
#include "PracticeRuntime.hpp"
#include "ResultScreen.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"
#include "Touch.hpp"
#include "ZunResult.hpp"
#include "dxutil.hpp"

static i32 renderRes = RENDER_RESULT_KEEP_RUNNING;
static bool g_AudioSuspendedByFocus = false;
#ifdef TH_ENABLE_THPRAC
static bool g_OpenThpracMenuForVisualTest = false;
static bool g_ThpracMenuVisualTestDispatched = false;
static bool g_AutoStartThpracVisualTest = false;
static bool g_AutoPauseThpracVisualTest = false;
static bool g_ThpracPauseVisualTestDispatched = false;
static int g_ThpracVisualTestFrames = 0;
#endif
static bool g_OpenMusicRoomForVisualTest = false;
static bool g_MusicRoomVisualTestDispatched = false;
#ifdef TH_ENABLE_THPRAC
static bool g_ThpracReplaySelfTest = false;
static bool g_ThpracRestartSelfTest = false;
#endif

static void SuspendAudioForInactiveWindow()
{
    if (g_AudioSuspendedByFocus)
    {
        return;
    }
    if (g_SoundPlayer.engine != nullptr)
    {
        ma_engine_stop(g_SoundPlayer.engine);
    }
    if (g_Supervisor.midiOutput != nullptr)
    {
        g_Supervisor.midiOutput->SetPaused(true);
    }
    g_AudioSuspendedByFocus = true;
}

static void ResumeAudioForActiveWindow()
{
    if (!g_AudioSuspendedByFocus)
    {
        return;
    }
    if (g_SoundPlayer.engine != nullptr)
    {
        ma_engine_start(g_SoundPlayer.engine);
    }
    if (g_Supervisor.midiOutput != nullptr)
    {
        g_Supervisor.midiOutput->SetPaused(false);
    }
    g_AudioSuspendedByFocus = false;
}

void AnmManager::TakeScreenshotIfRequested()
{
    if (this->screenshotTextureId >= 0)
    {
        Flush();

        TakeScreenshot(this->screenshotTextureId, this->screenshotSrcLeft, this->screenshotSrcTop,
                       this->screenshotSrcWidth, this->screenshotSrcHeight, this->screenshotDstLeft,
                       this->screenshotDstTop, this->screenshotDstWidth, this->screenshotDstHeight);
        this->screenshotTextureId = -1;
    }
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    for (int index = 1; index < argc; index++)
        if (std::strcmp(argv[index], "--music-room") == 0)
            g_OpenMusicRoomForVisualTest = true;
#ifdef TH_ENABLE_THPRAC
        else if (std::strcmp(argv[index], "--thprac-menu") == 0)
            g_OpenThpracMenuForVisualTest = true;
        else if (std::strcmp(argv[index], "--thprac-run") == 0)
            g_OpenThpracMenuForVisualTest = g_AutoStartThpracVisualTest = true;
        else if (std::strcmp(argv[index], "--thprac-pause") == 0)
            g_OpenThpracMenuForVisualTest = g_AutoStartThpracVisualTest = g_AutoPauseThpracVisualTest = true;
        else if (std::strcmp(argv[index], "--thprac-replay-selftest") == 0)
            g_ThpracReplaySelfTest = true;
        else if (std::strcmp(argv[index], "--thprac-restart-selftest") == 0)
            g_ThpracRestartSelfTest = true;
#endif
#ifdef __EMSCRIPTEN__
    g_OpenMusicRoomForVisualTest = EM_ASM_INT({ return Module.eaglerOptions?.debugHarness === 'music-room'; }) != 0;
#ifdef TH_ENABLE_THPRAC
    g_OpenThpracMenuForVisualTest = EM_ASM_INT({ return Module.eaglerOptions?.debugHarness === 'thprac-menu'; }) != 0;
#endif
#endif

    if (g_Supervisor.LoadConfig("th07.cfg") != ZUN_SUCCESS)
    {
        return SDL_APP_FAILURE;
    }
#ifdef TH_ENABLE_THPRAC
    if (g_ThpracReplaySelfTest)
    {
        const bool passed = PracticeRuntime::DebugReplayMetadataRoundTrip("thprac-replay-selftest.rpy");
        SDL_Log("th07 thprac replay metadata round-trip: %s", passed ? "PASS" : "FAIL");
        return passed ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    if (g_ThpracRestartSelfTest)
    {
        const bool passed = PracticeRuntime::DebugRestartPreservesConfig();
        SDL_Log("th07 thprac restart parameter preservation: %s", passed ? "PASS" : "FAIL");
        return passed ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
#endif

    GameWindow::ChecksumExecutable();
    g_GameWindow.frequency = SDL_GetPerformanceFrequency();

start:
    if (GameWindow::CreateGameWindow())
    {
        return SDL_APP_FAILURE;
    }

    if (GameWindow::InitInterface())
    {
        return SDL_APP_FAILURE;
    }

    if (GameWindow::InitRendering())
    {
        return SDL_APP_FAILURE;
    }

    g_SoundPlayer.InitializeSound();
    Controller::ResetKeyboard();
    g_AnmManager = new AnmManager();
    if (!g_Supervisor.cfg.windowed)
    {
        SDL_HideCursor();
    }
    renderRes = g_Supervisor.RegisterChain();
    if (renderRes != ZUN_SUCCESS)
    {
        return SDL_APP_FAILURE;
    }
    renderRes = RENDER_RESULT_KEEP_RUNNING;
    g_GameWindow.curFrame = -30;

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    if (g_OpenMusicRoomForVisualTest && !g_MusicRoomVisualTestDispatched &&
        g_MainMenuForDebug && g_MainMenuForDebug->calcChain)
    {
        g_Supervisor.curState = 8;
        g_MusicRoomVisualTestDispatched = true;
    }
#ifdef TH_ENABLE_THPRAC
    if (g_OpenThpracMenuForVisualTest && !g_ThpracMenuVisualTestDispatched &&
        g_MainMenuForDebug && g_MainMenuForDebug->calcChain)
    {
        g_Supervisor.cfg.defaultDifficulty = 1;
        g_GameManager.difficulty = 1;
        g_GameManager.character = 0;
        g_GameManager.shotType = 0;
        g_GameManager.practice = 1;
        g_MainMenuForDebug->SetGameState(STATE_SELECT_PRACTICE_STAGE);
        g_MainMenuForDebug->stateTimer = 0;
        g_ThpracMenuVisualTestDispatched = true;
    }
    if (g_AutoStartThpracVisualTest && g_ThpracMenuVisualTestDispatched &&
        ++g_ThpracVisualTestFrames == 30)
        PracticeRuntime::DebugAcceptPracticeMenu();
    if (g_AutoPauseThpracVisualTest && !g_ThpracPauseVisualTestDispatched &&
        PracticeRuntime::Active() && g_Supervisor.curState == 2 && g_ThpracVisualTestFrames > 180)
    {
        g_GameManager.isInPauseMenu = 1;
        g_ThpracPauseVisualTestDispatched = true;
    }
#endif
    renderRes = g_GameWindow.Render();
    if (renderRes != RENDER_RESULT_KEEP_RUNNING)
    {
        return SDL_APP_SUCCESS;
    }
    g_Supervisor.flags &= ~16;

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    switch (event->type)
    {
#ifdef TH_DEV_TOOLS
    case SDL_EVENT_KEY_DOWN:
        if (event->key.repeat == 0 && event->key.scancode == SDL_SCANCODE_F5)
        {
            g_DevSpeedMultiplier = g_DevSpeedMultiplier == 1.0f ? 4.0f :
                                   g_DevSpeedMultiplier == 4.0f ? 8.0f : 1.0f;
            SDL_Log("th07 dev: logic speed = %gx", g_DevSpeedMultiplier);
        }
        break;
#endif
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        ResumeAudioForActiveWindow();
        g_GameWindow.isAppActive = 1;
        if (!g_Supervisor.cfg.windowed)
        {
            SDL_HideCursor();
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        Touch::CancelTouches();
        SuspendAudioForInactiveWindow();
        g_GameWindow.isAppActive = 0;
        SDL_ShowCursor();
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        ResumeAudioForActiveWindow();
        g_GameWindow.isAppActive = 1;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!g_Supervisor.controller)
        {
            g_Supervisor.controller = SDL_OpenGamepad(event->gdevice.which);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (g_Supervisor.controller)
        {
            SDL_Joystick *joy = SDL_GetGamepadJoystick(g_Supervisor.controller);

            if (SDL_GetJoystickID(joy) == event->gdevice.which)
            {
                SDL_CloseGamepad(g_Supervisor.controller);
                g_Supervisor.controller = nullptr;
            }
        }
        break;
    case SDL_EVENT_FINGER_DOWN:
        Touch::FingerDown(event->tfinger);
        break;
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_FINGER_UP:
        Touch::FingerUp(event->tfinger);
        break;
    case SDL_EVENT_FINGER_MOTION:
        Touch::FingerMotion(event->tfinger);
        break;
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (g_GameManager.plst.base.magic != 0)
    {
        ResultScreen::RegisterChain(2);
    }
    g_Chain.Release();
    while (g_SoundPlayer.ProcessQueues())
        ;
    g_SoundPlayer.Release();

    SAFE_DELETE(g_AnmManager);
    SAFE_DELETE(g_Supervisor.gfxDevice);
    if (g_GameWindow.window)
    {
        SDL_DestroyWindow(g_GameWindow.window);
        g_GameWindow.window = NULL;
    }
    SDL_ShowCursor();
    if (renderRes == RENDER_RESULT_EXIT_ERROR)
    {
        g_GameErrorContext.m_BufferEnd = g_GameErrorContext.m_Buffer;
        *g_GameErrorContext.m_BufferEnd = '\0';
        g_GameErrorContext.Log("再起動を要するオプションが変更されたので再起動します\n");
        // we normally should restart the application but thats not really possible in this
        // model
        // however, this is really only ever used when the application is restarting after enabling
        // vsync. afaik, it should be pretty safe to just not have the application restart after
        // enabling vsync since theres nothing before the checkvsync call that needs vsyncenabled to
        // be there beforehand
    }
    FileSystem::WriteDataToFile("th07.cfg", &g_Supervisor.cfg, sizeof(GameConfiguration));
    g_GameErrorContext.Flush();
#ifdef __EMSCRIPTEN__
    EM_ASM({ globalThis.EaglerTouhouGameExited?.($0); }, static_cast<int>(result));
#endif
}

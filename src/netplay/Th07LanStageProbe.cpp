#include "Th07LanStageProbe.hpp"

#include "NetplayCore.hpp"
#include "NetplayInput.hpp"
#include "NetplayProtocol.hpp"
#include "NetplaySession.hpp"
#include "NetplaySideEffects.hpp"
#include "Th07CanonicalHash.hpp"
#include "Th07RollbackState.hpp"
#include "BrowserPeerTransport.hpp"
#include "WebSocketTransport.hpp"

#include "BulletManager.hpp"
#include "AsciiManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "EnemyManager.hpp"
#include "GameManager.hpp"
#include "Gui.hpp"
#include "ItemManager.hpp"
#include "Player.hpp"
#include "ReplayManager.hpp"
#include "Supervisor.hpp"
#include "Touch.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace Netplay::Th07LanStageProbe
{
namespace
{
constexpr std::uint64_t SESSION_ID = 0x5448374c414e5331ull; // TH7LANS1
constexpr std::uint32_t GAME_ID_TH07 = 7;
// ABI 2 adds room-selected difficulty/EX/PH startup and keeps the shared
// Ending scene inside the confirmed-input session. Refuse an ABI-1 peer
// instead of letting old and new runtimes simulate different scene routes.
constexpr std::uint32_t GAMEPLAY_ABI = 2;
constexpr std::uint32_t DEFAULT_TEST_FRAMES = 300;

WebSocketTransport g_Transport;
BrowserPeerTransport g_BrowserPeerTransport;
RollbackCore g_Core;
SessionGate g_Session;
bool g_Initialized = false;
bool g_Active = false;
bool g_Done = false;
bool g_ProductionTransportStarted = false;
bool g_LastTickAdvanced = false;
std::uint32_t g_SessionGeneration = 0;
std::uint8_t g_LocalPlayer = 0;
std::uint8_t g_PlayerCount = 2;
std::uint32_t g_SimFrame = 0;
std::uint32_t g_DriverTicks = 0;
std::uint32_t g_TestFrames = DEFAULT_TEST_FRAMES;
std::uint32_t g_Sequence = 1;
std::uint32_t g_LastReceivedSequence = 0;
std::uint32_t g_SentPackets = 0;
std::uint32_t g_ReceivedPackets = 0;
std::uint32_t g_SessionPacketsSent = 0;
std::uint32_t g_SessionPacketsReceived = 0;
std::uint32_t g_PredictedFrames = 0;
std::uint32_t g_RollbackCount = 0;
std::uint32_t g_ResimulatedFrames = 0;
std::uint32_t g_MaxRollbackSpan = 0;
std::size_t g_MaxSnapshotBytes = 0;
std::array<std::uint32_t, MAX_PLAYERS> g_LastConfirmedFrame{};
std::array<std::uint64_t, MAX_PLAYERS> g_LastConfirmedAdvanceMs{};
bool g_IndependentInputSlotsObserved = false;
std::uint8_t g_InputSlotObservedMask = 0;
bool g_PhysicalInputObserved = false;
bool g_PhysicalLoggedFrame0Sample = false;
bool g_PhysicalLoggedFrame0Wait = false;
bool g_PhysicalLoggedFrame0Sim = false;
bool g_PauseObserved = false;
bool g_PauseResumeObserved = false;
std::uint8_t g_HighestStageObserved = 0;
bool g_StageTransitionObserved = false;
bool g_StageTransitionInputResetObserved = false;
bool g_PostTransitionInputObserved = false;
bool g_EndingSkipHistoryNormalized = false;
std::uint32_t g_PeakEnemies = 0;
std::uint32_t g_PeakBullets = 0;
std::uint32_t g_PeakLasers = 0;
std::uint32_t g_PeakItems = 0;
std::array<std::uint32_t, MAX_PLAYERS> g_LastRemoteSenderFrame{};
struct PeerTimeSyncState
{
    std::array<std::int32_t, 64> samples{};
    std::size_t sampleCount = 0;
    std::size_t sampleCursor = 0;
    double averageLead = 0.0;
    bool ready = false;
};
std::array<PeerTimeSyncState, MAX_PLAYERS> g_PeerTimeSync{};
std::array<std::uint32_t, MAX_PLAYERS> g_PredictionDepth{};
std::array<std::uint32_t, MAX_PLAYERS> g_RollbackByPlayer{};
double g_RecommendedLead = 0.0;
double g_SimulationIntervalScale = 1.0;

bool UsePhysicalInput();
bool UsePauseCycle();
bool UseStageTransitionTest();
bool ProbeMode();
bool ProductionLanMode();
void ClearTransientModes();
std::uint16_t LocalScript(std::uint8_t player, std::uint32_t frame);

std::uint64_t CurrentSessionId()
{
    // A retry/new run is a new rollback session even if the browser keeps the
    // same RTC/DataChannel transport alive. Generation 0 preserves the wire id
    // used by existing rooms; later generations derive a distinct id so late
    // packets from the previous run are ignored instead of contaminating it.
    return SESSION_ID ^
           (static_cast<std::uint64_t>(g_SessionGeneration) *
            0x9e3779b97f4a7c15ull);
}

void RetireGameplaySession()
{
    ClearTransientModes();
    Th07Rollback::Clear();
    g_Initialized = false;
    g_Active = false;
    g_Done = false;
    g_SimFrame = 0;
    g_DriverTicks = 0;
    g_Sequence = 1;
    g_LastReceivedSequence = 0;
    ++g_SessionGeneration;
#ifdef __EMSCRIPTEN__
    if (ProductionLanMode())
    {
        EM_ASM({
            globalThis.__eaglerNetplayLanActive = false;
            globalThis.__eaglerNetplayLanFrame = 0;
        });
    }
#endif
}

bool TransportConnect(const char *url)
{
    if (ProductionLanMode())
        return g_BrowserPeerTransport.Connect(url, g_LocalPlayer, g_PlayerCount);
    return g_Transport.Connect(url);
}

bool TransportIsOpen()
{
    return ProductionLanMode() ? g_BrowserPeerTransport.IsOpen() : g_Transport.IsOpen();
}

bool TransportFailed()
{
    return ProductionLanMode() ? g_BrowserPeerTransport.Failed() : g_Transport.Failed();
}

bool TransportSend(const std::uint8_t *data, std::size_t size, bool control = false)
{
    if (!ProductionLanMode())
        return g_Transport.Send(data, size);
    return control ? g_BrowserPeerTransport.SendControl(data, size)
                   : g_BrowserPeerTransport.Send(data, size);
}

bool TransportSendTo(std::uint8_t peer, const std::uint8_t *data, std::size_t size)
{
    return ProductionLanMode() && g_BrowserPeerTransport.SendTo(peer, data, size);
}

bool TransportPoll(std::vector<std::uint8_t> *packet)
{
    return ProductionLanMode() ? g_BrowserPeerTransport.Poll(packet) : g_Transport.Poll(packet);
}

std::size_t TransportBufferedAmount()
{
    return ProductionLanMode() ? g_BrowserPeerTransport.BufferedAmount() : g_Transport.BufferedAmount();
}

const std::string &TransportLastError()
{
    return ProductionLanMode() ? g_BrowserPeerTransport.LastError() : g_Transport.LastError();
}

bool ProbeMode()
{
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
        return Module.eaglerOptions?.debugHarness === 'netplay-lan-stage1' ? 1 : 0;
    }) != 0;
#else
    return false;
#endif
}

std::int32_t SignedFrameDelta(std::uint32_t lhs, std::uint32_t rhs)
{
    return static_cast<std::int32_t>(lhs - rhs);
}

void RecordTimeSyncSample(const InputPacket &packet)
{
    // GGPO/GGRS maintain time-sync state per endpoint.  A 3P room must not
    // average two unrelated links into one sample stream: update the sender's
    // window, then use the largest local lead as the room wait recommendation.
    if (!ProductionLanMode() || packet.senderFrame == INVALID_FRAME ||
        packet.senderPlayer >= g_PlayerCount || packet.senderPlayer == g_LocalPlayer)
        return;

    std::uint32_t &lastRemoteFrame = g_LastRemoteSenderFrame[packet.senderPlayer];
    if (lastRemoteFrame != INVALID_FRAME && packet.senderFrame <= lastRemoteFrame)
        return;
    lastRemoteFrame = packet.senderFrame;

    const std::int32_t localAdvantage = SignedFrameDelta(g_SimFrame, packet.senderFrame);
    const std::int32_t advantageDifference =
        localAdvantage - static_cast<std::int32_t>(packet.frameAdvantage);
    const std::int32_t inferredLead = advantageDifference / 2;
    if (std::abs(inferredLead) > 30)
        return;

    PeerTimeSyncState &state = g_PeerTimeSync[packet.senderPlayer];
    state.samples[state.sampleCursor] = inferredLead;
    state.sampleCursor = (state.sampleCursor + 1) % state.samples.size();
    state.sampleCount = std::min(state.sampleCount + 1, state.samples.size());
    if (state.sampleCount < 20)
        return;

    std::vector<std::int32_t> sorted;
    sorted.reserve(state.sampleCount);
    for (std::size_t i = 0; i < state.sampleCount; ++i)
        sorted.push_back(state.samples[i]);
    std::sort(sorted.begin(), sorted.end());
    const std::size_t trim = std::min<std::size_t>(4, sorted.size() / 8);
    std::int64_t sum = 0;
    for (std::size_t i = trim; i < sorted.size() - trim; ++i)
        sum += sorted[i];
    state.averageLead = static_cast<double>(sum) /
        static_cast<double>(sorted.size() - trim * 2);
    state.ready = true;

    bool haveRecommendation = false;
    double recommendedLead = 0.0;
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
    {
        if (player == g_LocalPlayer || !g_PeerTimeSync[player].ready)
            continue;
        if (!haveRecommendation || g_PeerTimeSync[player].averageLead > recommendedLead)
            recommendedLead = g_PeerTimeSync[player].averageLead;
        haveRecommendation = true;
    }
    if (!haveRecommendation)
        return;
    g_RecommendedLead = recommendedLead;

    const double deadbandLead = std::abs(recommendedLead) < 0.5 ? 0.0 : recommendedLead;
    const double desiredScale = std::clamp(1.0 + deadbandLead * 0.003, 0.98, 1.02);
    g_SimulationIntervalScale += (desiredScale - g_SimulationIntervalScale) * 0.08;
    if (std::abs(g_SimulationIntervalScale - 1.0) < 0.0002)
        g_SimulationIntervalScale = 1.0;
#ifdef __EMSCRIPTEN__
    EM_ASM({
        globalThis.__eaglerNetplayLanFrameAdvantage = $0;
        globalThis.__eaglerNetplayLanPacingScale = $1;
        globalThis.__eaglerNetplayLanPeerAdvantages ||= [];
        globalThis.__eaglerNetplayLanPeerAdvantages[$2] = $3;
    }, recommendedLead, g_SimulationIntervalScale,
       static_cast<int>(packet.senderPlayer), state.averageLead);
#endif
}

bool ProductionLanMode()
{
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
        return Module.eaglerOptions?.netplayMode === 'lan' ? 1 : 0;
    }) != 0;
#else
    return false;
#endif
}

bool RequestedInternal()
{
    return ProbeMode() || ProductionLanMode();
}

bool UseStageTransitionTest()
{
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({ return Module.eaglerOptions?.netplayStageTransition ? 1 : 0; }) != 0;
#else
    return false;
#endif
}

std::uint32_t ConfirmedThroughAllRemotes()
{
    std::uint32_t confirmed = INVALID_FRAME;
    bool found = false;
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
    {
        if (player == g_LocalPlayer)
            continue;
        const std::uint32_t value = g_Core.ConfirmedThrough(player);
        if (!found || value == INVALID_FRAME ||
            (confirmed != INVALID_FRAME && value < confirmed))
            confirmed = value;
        found = true;
    }
    return found ? confirmed : INVALID_FRAME;
}

void PublishPeerDiagnostics(std::uint8_t predictedMask)
{
#ifdef __EMSCRIPTEN__
    if (!ProductionLanMode())
        return;
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
    {
        if (player == g_LocalPlayer)
            continue;
        const std::uint32_t confirmed = g_Core.ConfirmedThrough(player);
        const std::uint32_t gap = confirmed == INVALID_FRAME
                                      ? g_SimFrame + 1
                                      : confirmed < g_SimFrame ? g_SimFrame - confirmed : 0;
        g_PredictionDepth[player] =
            (predictedMask & static_cast<std::uint8_t>(1u << player)) != 0 ? gap : 0;
        EM_ASM({
            globalThis.__eaglerNetplayLanPeers ||= [];
            const entry = globalThis.__eaglerNetplayLanPeers[$0] ||= {};
            entry.player = $0;
            entry.confirmed = $1 >>> 0;
            entry.gap = $2 >>> 0;
            entry.predicted = $3 >>> 0;
            entry.rollbacks = $4 >>> 0;
        }, static_cast<int>(player), confirmed, gap,
           g_PredictionDepth[player], g_RollbackByPlayer[player]);
    }
#else
    (void)predictedMask;
#endif
}

std::uint8_t FirstRemotePlayer()
{
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
        if (player != g_LocalPlayer)
            return player;
    return 0xff;
}

std::uint8_t ReadPlayerCount()
{
#ifdef __EMSCRIPTEN__
    const int value = EM_ASM_INT({
        const count = Number(Module.eaglerOptions?.netplayPlayerCount ?? 2);
        return count === 3 ? 3 : 2;
    });
    return static_cast<std::uint8_t>(value);
#else
    return 2;
#endif
}

std::uint32_t ReadTestFrames()
{
#ifdef __EMSCRIPTEN__
    const int value = EM_ASM_INT({
        const value = Number(Module.eaglerOptions?.netplayTestFrames ?? 300);
        const maxFrames = Module.eaglerOptions?.netplayStageTransition ? 24000 : 12000;
        return Number.isInteger(value) && value >= 60 && value <= maxFrames ? value : 300;
    });
    return static_cast<std::uint32_t>(value);
#else
    return DEFAULT_TEST_FRAMES;
#endif
}

FrameInput CaptureLocalInput(std::uint32_t frame)
{
    if (!UsePhysicalInput())
        return FrameInput(LocalScript(g_LocalPlayer, frame));

    // Sample the local browser/controller exactly once when this logical
    // frame is first scheduled. The simulation pass below replays a
    // deterministic aggregate raw word, so Supervisor never consumes the
    // device a second time for prediction or rollback resimulation.
    Input::ClearReplayOverride();
    Input::BeginCapture();
    (void)Controller::GetInput();
    float x = 0.0f;
    float y = 0.0f;
    if (Touch::GetFreeJoystickVector(&x, &y))
        Input::CaptureJoystick(x, y);
    else if (Touch::GetPlayerDelta(&x, &y))
        Input::CaptureDirectTouch(x, y, Touch::IsUnlimited());
    FrameInput input = Input::EndCapture();
    input.touchUsed = Touch::WasUsedThisRun();
    input.touchBomb = Touch::UsedTouchToBomb();
    if (input.buttons != 0 || input.analogMode != AnalogMode::None)
        g_PhysicalInputObserved = true;
    return input;
}

std::uint16_t CombinedButtons(const FrameDecision &decision)
{
    std::uint16_t bits = 0;
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
        bits = static_cast<std::uint16_t>(bits | decision.inputs[player].buttons);
    return bits;
}

bool UsePhysicalInput()
{
#ifdef __EMSCRIPTEN__
    return ProductionLanMode() ||
           EM_ASM_INT({ return Module.eaglerOptions?.netplayPhysicalInput ? 1 : 0; }) != 0;
#else
    return false;
#endif
}

bool UsePauseCycle()
{
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({ return Module.eaglerOptions?.netplayPauseCycle ? 1 : 0; }) != 0;
#else
    return false;
#endif
}

bool EligibleForInitialNetplayStart()
{
    const int stage = g_GameManager.currentStage;
    const bool validInitialStage = stage == 1 || stage == 7 || stage == 8;
    return g_GameManager.notInMenu && !g_GameManager.replay &&
           !g_GameManager.isPaused && !g_GameManager.isInPauseMenu &&
           !g_GameManager.isInRetryMenu && !g_GameManager.isTimeStopped &&
           validInitialStage && g_GameManager.globals != nullptr &&
           g_GameManager.defaultCfg != nullptr && g_ReplayManager != nullptr &&
           g_Supervisor.curState == 2 && g_Supervisor.wantedState == g_Supervisor.curState;
}

bool SessionStillOwnsStageState()
{
    // Once the session gate has taken ownership, pause/retry/message time-stop
    // are part of the synchronized stage state as well.  Dropping back to a
    // local g_Chain.RunCalcChain() here would re-sample each machine's hardware
    // and make the first pause the first desync.
    const bool gameplayScene =
        (g_Supervisor.curState == 2 || g_Supervisor.curState == 3) &&
        (g_Supervisor.wantedState == 2 || g_Supervisor.wantedState == 3);
    // TH07 writes the requested scene into curState while wantedState still
    // names the scene being left. Keep ownership for the exact confirmed
    // transition from gameplay into Ending, then for the complete Ending.
    const bool endingScene = g_Supervisor.curState == 9 &&
        (g_Supervisor.wantedState == 2 || g_Supervisor.wantedState == 3 ||
         g_Supervisor.wantedState == 9);
    return !g_GameManager.replay && g_GameManager.currentStage >= 1 &&
           g_GameManager.currentStage <= 8 &&
           g_GameManager.globals != nullptr && g_GameManager.defaultCfg != nullptr &&
           g_ReplayManager != nullptr &&
           (gameplayScene || endingScene);
}

bool SharedUiNeedsConfirmedInputs()
{
    return g_GameManager.isPaused || g_GameManager.isInPauseMenu ||
           g_GameManager.isInRetryMenu ||
           (g_Gui.impl && (g_Gui.impl->finishedStage ||
                           g_Gui.impl->transitionToScoreScreen)) ||
           g_Supervisor.curState != 2 || g_Supervisor.wantedState != 2;
}

void NormalizeMultiplayerEndingSkipHistory()
{
    if (g_EndingSkipHistoryNormalized || g_Supervisor.curState != 9 ||
        !g_GameManager.globals)
        return;

    const int shotType = g_GameManager.shotTypeAndCharacter;
    const int difficulty = g_GameManager.difficulty;
    if (shotType < 0 || shotType >= 6 || difficulty < 0 || difficulty >= 6)
        return;

    // Port final sbrik's Ending contract. Ending::AddedCallback derives Ctrl
    // fast-forward from each machine's local score.dat history. Mark the clear
    // one callback earlier on every peer, which is the same eventual save
    // result but makes the parser speed deterministic for this shared scene.
    if (g_GameManager.globals->numRetries == 0)
        g_GameManager.clrd[shotType]
            .difficultyClearedWithRetries[difficulty] = 99;
    else
        g_GameManager.clrd[shotType]
            .difficultyClearedWithoutRetries[difficulty] = 99;
    g_EndingSkipHistoryNormalized = true;
    std::printf("netplay lan ending: synchronized skip history\n");
}

bool RemoteInputsTimedOut()
{
    // Native sbrik can contract a 3P UDP session around an absent guest by
    // announcing host-authored lifecycle controls and relaying synthesized
    // inputs. The browser relay has no equivalent authority protocol. Do not
    // guess that simulation behavior locally: end the room visibly if any
    // required WebSocket lane stops advancing instead of leaving every canvas
    // in an unbounded silent stall.
    constexpr std::uint64_t timeoutMs = 15000;
    const std::uint64_t now = SDL_GetTicks();
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
    {
        if (player == g_LocalPlayer)
            continue;
        const std::uint32_t confirmed = g_Core.ConfirmedThrough(player);
        if (confirmed != g_LastConfirmedFrame[player])
        {
            g_LastConfirmedFrame[player] = confirmed;
            g_LastConfirmedAdvanceMs[player] = now;
        }
        else if (g_Session.CanStart() &&
                 now - g_LastConfirmedAdvanceMs[player] >= timeoutMs)
        {
            std::printf(
                "netplay lan stage: ERROR remote input timeout player=%u confirmed=%u sim=%u\n",
                static_cast<unsigned>(player), confirmed, g_SimFrame);
            return true;
        }
    }
    return false;
}

std::uint8_t ReadPlayer()
{
#ifdef __EMSCRIPTEN__
    const int value = EM_ASM_INT({ return Module.eaglerOptions?.netplayPlayer ?? -1; });
    return value >= 0 && value < g_PlayerCount ? static_cast<std::uint8_t>(value) : 0xff;
#else
    return 0xff;
#endif
}

bool ReadUrl(char *out, std::size_t capacity)
{
#ifdef __EMSCRIPTEN__
    if (!out || capacity == 0)
        return false;
    EM_ASM({ stringToUTF8(Module.eaglerOptions?.netplayUrl || "", $0, $1); }, out, capacity);
    return out[0] != '\0';
#else
    (void)out;
    (void)capacity;
    return false;
#endif
}

bool StartProductionTransportEarly()
{
    if (!ProductionLanMode() || g_ProductionTransportStarted)
        return true;

    // ICE/DataChannel setup is asynchronous and can take several seconds on a
    // real public route. Start it while the vanilla title/loading chain is
    // still presenting frames; waiting until the first Stage 1 deterministic
    // tick can freeze the frame-zero gate before HELLO has a transport.
    g_PlayerCount = ReadPlayerCount();
    g_LocalPlayer = ReadPlayer();
    char url[512] = {};
    if (g_LocalPlayer >= g_PlayerCount || !ReadUrl(url, sizeof(url)) ||
        !TransportConnect(url))
        return false;
    g_ProductionTransportStarted = true;
    std::printf("netplay lan stage: PRECONNECT player=%u players=%u url=%s\n",
                static_cast<unsigned>(g_LocalPlayer),
                static_cast<unsigned>(g_PlayerCount), url);
    return true;
}

std::uint16_t LocalScript(std::uint8_t player, std::uint32_t frame)
{
    if (UsePauseCycle() && frame >= 600 && frame <= 700)
    {
        // Exercise the exact shared pause path without gameplay buttons
        // accidentally navigating the menu.  P1 opens at 600 and closes at
        // 680; every peer receives the same synchronized edge through normal
        // rollback/network input.
        return player == 0 && (frame == 600 || frame == 680)
                   ? TH_BUTTON_MENU
                   : 0;
    }

    // Give every logical player a distinct deterministic pattern so a lane
    // alias or dropped P3 input is observable in both movement and the hash.
    if (player == 0)
    {
        std::uint16_t bits = TH_BUTTON_SHOOT;
        if (UseStageTransitionTest())
            bits |= TH_BUTTON_SKIP;
        const std::uint32_t phase = (frame / 40) & 3u;
        bits |= phase == 0 ? TH_BUTTON_LEFT :
                phase == 1 ? TH_BUTTON_RIGHT :
                phase == 2 ? TH_BUTTON_LEFT : TH_BUTTON_RIGHT;
        return bits;
    }
    if (player == 2)
    {
        std::uint16_t bits = TH_BUTTON_SHOOT;
        const std::uint32_t phase = (frame / 30) & 3u;
        bits |= phase == 0 ? TH_BUTTON_RIGHT :
                phase == 1 ? TH_BUTTON_UP :
                phase == 2 ? TH_BUTTON_LEFT : TH_BUTTON_DOWN;
        if ((frame / 45) & 1u)
            bits |= TH_BUTTON_FOCUS;
        return bits;
    }
    std::uint16_t bits = 0;
    if (UseStageTransitionTest())
        bits |= TH_BUTTON_SHOOT;
    const std::uint32_t phase = (frame / 35) & 3u;
    bits |= phase == 0 ? TH_BUTTON_UP :
            phase == 1 ? TH_BUTTON_DOWN :
            phase == 2 ? TH_BUTTON_UP : TH_BUTTON_DOWN;
    if ((frame / 50) & 1u)
        bits |= TH_BUTTON_FOCUS;
    return bits;
}

void HashToHex(std::uint64_t value, char out[17])
{
    std::snprintf(out, 17, "%016llx", static_cast<unsigned long long>(value));
}

void PrintHash(const char *label, const Th07CanonicalHash::Sample &sample)
{
    char total[17], meta[17], stage[17], player[17], enemies[17], bullets[17], items[17];
    HashToHex(sample.composite, total);
    HashToHex(sample.meta, meta);
    HashToHex(sample.stage, stage);
    HashToHex(sample.player, player);
    HashToHex(sample.enemies, enemies);
    HashToHex(sample.bullets, bullets);
    HashToHex(sample.items, items);
    std::printf(
        "netplay lan stage: %s player=%u hash=%s meta=%s stage=%s playerHash=%s enemies=%s bullets=%s items=%s counts=%u/%u/%u/%u\n",
        label, static_cast<unsigned>(g_LocalPlayer), total, meta, stage, player,
        enemies, bullets, items, sample.enemyCount, sample.bulletCount,
        sample.laserCount, sample.itemCount);
}

void ClearTransientModes()
{
    Input::ClearReplayOverride();
    Input::ClearPlayerButtonOverrides();
    if (Input::CaptureActive())
        (void)Input::EndCapture();
    SideEffects::SetSpeculative(false);
}

void Fail(const char *reason)
{
    if (g_Done)
        return;
    ClearTransientModes();
    g_Done = true;
    std::printf(
        "netplay lan stage: FAIL player=%u reason=%s sim=%u sent=%u recv=%u rollback=%u resim=%u predicted=%u confirmed=%u buffered=%llu error=%s\n",
        static_cast<unsigned>(g_LocalPlayer), reason, g_SimFrame, g_SentPackets,
        g_ReceivedPackets, g_RollbackCount, g_ResimulatedFrames, g_PredictedFrames,
        static_cast<unsigned>(ConfirmedThroughAllRemotes()),
        static_cast<unsigned long long>(TransportBufferedAmount()),
        TransportLastError().c_str());
}

bool Initialize()
{
    g_PlayerCount = ReadPlayerCount();
    g_LocalPlayer = ReadPlayer();
    g_TestFrames = ProbeMode() ? ReadTestFrames() : 0xffffffffu;
    char url[512] = {};
    if (g_LocalPlayer >= g_PlayerCount || !ReadUrl(url, sizeof(url)))
        return false;

    CoreConfig coreConfig;
    coreConfig.sessionId = CurrentSessionId();
    coreConfig.playerCount = g_PlayerCount;
    coreConfig.localPlayer = g_LocalPlayer;
    coreConfig.inputDelay = 0;
    coreConfig.maxRollbackFrames = 12;
    coreConfig.predictableButtons =
        TH_BUTTON_DIRECTION | TH_BUTTON_FOCUS | TH_BUTTON_SHOOT | TH_BUTTON_SKIP;
    coreConfig.directionButtons = TH_BUTTON_DIRECTION;
    coreConfig.maxDirectionPredictionFrames = 3;
    if (!g_Core.Reset(coreConfig))
        return false;

    Th07Rollback::Config stateConfig;
    stateConfig.maxFrames = 16;
    stateConfig.maxBytesPerFrame = 8 * 1024 * 1024;
    stateConfig.maxBlocksPerFrame = 4096;
    // Final sbrik raised the 3P rollback ceiling to 1024 after simultaneous
    // Bombs routinely exceeded the earlier two-player-sized budget. The
    // journal stores only live BombEffects, so retain that gameplay-derived
    // headroom without importing the native snapshot/network shell.
    stateConfig.maxBombEffectsPerFrame = 1024;
    if (!Th07Rollback::Reset(stateConfig))
        return false;

#ifdef TH_ENABLE_MULTIPLAYER_GAMEPLAY
    if (UseStageTransitionTest())
    {
        // Upstream's full-run preset is invincible as well: the purpose of
        // this harness is to exercise the real normal-route stage lifecycle,
        // not measure how long a deterministic bot survives. Keep that test
        // policy local to the probe so production Player/GameManager semantics
        // stay untouched.
        for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
        {
            SetPlayerLives(player, 8);
            g_Players[player].playerState = PLAYER_STATE_INVULNERABLE;
            g_Players[player].invulnerabilityTimer = 0x3fffffff;
        }
    }
#endif

    SessionConfig sessionConfig;
    sessionConfig.sessionId = CurrentSessionId();
    sessionConfig.seed = static_cast<std::uint32_t>(g_Rng.seed);
    sessionConfig.gameplayAbi = GAMEPLAY_ABI;
    sessionConfig.gameId = GAME_ID_TH07;
    sessionConfig.playerCount = g_PlayerCount;
    sessionConfig.localPlayer = g_LocalPlayer;
    if (!g_Session.Reset(sessionConfig) ||
        (!g_ProductionTransportStarted && !TransportConnect(url)))
        return false;
    g_ProductionTransportStarted = true;

    g_LastConfirmedFrame.fill(INVALID_FRAME);
    g_LastRemoteSenderFrame.fill(INVALID_FRAME);
    for (PeerTimeSyncState &state : g_PeerTimeSync)
        state = PeerTimeSyncState{};
    g_PredictionDepth.fill(0);
    g_RollbackByPlayer.fill(0);
    g_RecommendedLead = 0.0;
    g_SimulationIntervalScale = 1.0;
    g_EndingSkipHistoryNormalized = false;
    g_LastConfirmedAdvanceMs.fill(SDL_GetTicks());

    g_Active = true;
#ifdef __EMSCRIPTEN__
    if (ProductionLanMode())
    {
        EM_ASM({
            globalThis.__eaglerNetplayLanActive = true;
            globalThis.__eaglerNetplayLanFrame = 0;
            globalThis.__eaglerNetplayLanHash = "";
            globalThis.__eaglerNetplayLanHashes = Object.create(null);
            globalThis.__eaglerNetplayLanPeerAdvantages = [];
            globalThis.__eaglerNetplayLanPeers = [];
        });
    }
#endif
    std::printf("netplay lan stage: CONNECT player=%u players=%u url=%s seed=%u generation=%u\n",
                static_cast<unsigned>(g_LocalPlayer), static_cast<unsigned>(g_PlayerCount), url,
                static_cast<unsigned>(g_Rng.seed),
                static_cast<unsigned>(g_Rng.generationCount));
    return true;
}

bool DrainPackets()
{
    std::vector<std::uint8_t> wire;
    while (TransportPoll(&wire))
    {
        ++g_ReceivedPackets;
        PacketType type;
        if (!PeekPacketType(wire.data(), wire.size(), &type))
            return false;
        if (type == PacketType::Session)
        {
            SessionPacket packet;
            if (!DecodeSessionPacket(wire.data(), wire.size(), &packet))
                return false;
            if (packet.sessionId != CurrentSessionId())
                continue;
            ++g_SessionPacketsReceived;
            const SessionPacketResult result = g_Session.Apply(packet);
            if (result == SessionPacketResult::InvalidPeer ||
                result == SessionPacketResult::ContractMismatch)
                return false;
            // Browser production uses a separate reliable/ordered RTC control
            // channel. The periodic session packets remain useful for the
            // legacy WebSocket/probe path and reconnect-safe startup logic.
            continue;
        }

        InputPacket packet;
        if (!DecodeInputPacket(wire.data(), wire.size(), &packet))
            return false;
        if (packet.sessionId != CurrentSessionId())
            continue;
        if (packet.senderPlayer == g_LocalPlayer)
            return false;
        RecordTimeSyncSample(packet);
        g_LastReceivedSequence = std::max(g_LastReceivedSequence, packet.sequence);
        RemoteInputResult result = RemoteInputResult::Duplicate;
        if (!g_Core.ApplyInputPacket(packet, &result))
            return false;
        if (result == RemoteInputResult::RollbackRequired)
            ++g_RollbackByPlayer[packet.senderPlayer];
    }
    return true;
}

bool SendSessionControl(bool forceReady = false)
{
    if (!TransportIsOpen() || (g_Session.CanStart() && !forceReady))
        return true;

    const auto sendPhase = [](SessionPhase phase) {
        const SessionPacket packet = g_Session.BuildPacket(phase);
        std::vector<std::uint8_t> wire;
        if (!EncodeSessionPacket(packet, &wire) ||
            !TransportSend(wire.data(), wire.size(), true))
            return false;
        ++g_SessionPacketsSent;
        return true;
    };

    if (forceReady)
        return sendPhase(SessionPhase::Ready);
    if (!g_Session.CanSendReady())
        return sendPhase(SessionPhase::Hello);

    // A slower peer can receive our HELLO before its own Stage 1 gate starts.
    // Its first packet would then be READY, which the earlier peer must reject
    // until it has seen that slower peer's HELLO. Keep HELLO retransmission in
    // the READY phase so asymmetric loading can always complete the ordered
    // session contract, then send READY on the reliable control channel.
    if (!sendPhase(SessionPhase::Hello))
        return false;
    g_Session.MarkLocalReady();
    return sendPhase(SessionPhase::Ready);
}

bool SendScheduledLocalFrame(std::uint32_t frame);

bool SendLocalFrame(std::uint32_t frame)
{
    const FrameInput input = CaptureLocalInput(frame);
    if (UsePhysicalInput() && frame == 0 && !g_PhysicalLoggedFrame0Sample)
    {
        g_PhysicalLoggedFrame0Sample = true;
        std::printf("netplay lan physical: FRAME0 SAMPLE player=%u bits=0x%04x\n",
                    static_cast<unsigned>(g_LocalPlayer),
                    static_cast<unsigned>(input.buttons));
    }
    if (!g_Core.ScheduleLocalInput(frame, input))
        return false;
    return SendScheduledLocalFrame(frame);
}

bool SendScheduledLocalFrame(std::uint32_t frame)
{
    // Rebuild only the redundant wire packet. The logical FrameInput was
    // captured and stored exactly once by SendLocalFrame, so a startup/stall
    // retry never samples keyboard, controller or touch displacement again.
    if (ProductionLanMode())
    {
        for (std::uint8_t peer = 0; peer < g_PlayerCount; ++peer)
        {
            if (peer == g_LocalPlayer)
                continue;
            InputPacket packet = g_Core.BuildInputPacket(
                peer, frame, g_Sequence++, g_LastReceivedSequence);
            packet.senderFrame = g_SimFrame;
            if (g_LastRemoteSenderFrame[peer] != INVALID_FRAME)
            {
                packet.frameAdvantage = static_cast<std::int16_t>(SignedFrameDelta(
                    g_SimFrame, g_LastRemoteSenderFrame[peer]));
            }
            std::vector<std::uint8_t> wire;
            if (!EncodeInputPacket(packet, &wire) ||
                !TransportSendTo(peer, wire.data(), wire.size()))
                return false;
            ++g_SentPackets;
        }
        return true;
    }

    const std::uint8_t peer = FirstRemotePlayer();
    InputPacket packet = g_Core.BuildInputPacket(
        peer, frame, g_Sequence++, g_LastReceivedSequence);
    packet.senderFrame = g_SimFrame;
    if (g_PlayerCount > 2)
    {
        // The minimal LAN relay broadcasts one wire packet to every other
        // peer. ACK state is peer-specific, so a 3P broadcast deliberately
        // carries no ACK and keeps the normal 32-frame redundant input tail.
        // This is a test-transport choice, not the public transport contract.
        packet.ackFrame = INVALID_FRAME;
    }
    std::vector<std::uint8_t> wire;
    if (!EncodeInputPacket(packet, &wire) || !TransportSend(wire.data(), wire.size()))
        return false;
    ++g_SentPackets;
    return true;
}

bool SendTailKeepalive()
{
    if (!TransportIsOpen() || g_SimFrame < g_TestFrames)
        return true;

    return SendScheduledLocalFrame(g_TestFrames - 1);
}

int SimulateFrame(std::uint32_t frame, const FrameDecision &decision, bool resimulation)
{
    if (UsePhysicalInput() && frame == 0 && !g_PhysicalLoggedFrame0Sim)
    {
        g_PhysicalLoggedFrame0Sim = true;
        std::printf(
            "netplay lan physical: FRAME0 SIM player=%u p1=0x%04x p2=0x%04x p3=0x%04x resim=%d\n",
            static_cast<unsigned>(g_LocalPlayer),
            static_cast<unsigned>(decision.inputs[0].buttons),
            static_cast<unsigned>(decision.inputs[1].buttons),
            static_cast<unsigned>(decision.inputs[2].buttons), resimulation ? 1 : 0);
    }
    NormalizeMultiplayerEndingSkipHistory();
    if (!Th07Rollback::BeginFrame(frame))
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    SideEffects::SetSpeculative(resimulation);
    // Legacy/global raw-input owners still need one deterministic word. Use
    // the synchronized union on every peer while Player consumes its own
    // slot below. This also prevents Supervisor from re-sampling hardware
    // during rollback resimulation.
    Input::SetReplayOverride(CombinedButtons(decision));
    Input::SetPlayerInputOverrides(decision.inputs.data(), g_PlayerCount);
    const std::uint8_t stageBeforeCalc =
        static_cast<std::uint8_t>(g_GameManager.currentStage);
    const int result = g_Chain.RunCalcChain();
    const bool stageChangedDuringCalc =
        static_cast<std::uint8_t>(g_GameManager.currentStage) != stageBeforeCalc;
#ifdef TH_ENABLE_MULTIPLAYER_GAMEPLAY
    if (g_GameManager.currentStage > g_HighestStageObserved)
    {
        g_HighestStageObserved = static_cast<std::uint8_t>(g_GameManager.currentStage);
        std::printf("netplay lan stage: STAGE player=%u stage=%u frame=%u resim=%d\n",
                    static_cast<unsigned>(g_LocalPlayer),
                    static_cast<unsigned>(g_HighestStageObserved), frame,
                    resimulation ? 1 : 0);
        if (g_HighestStageObserved >= 2)
            g_StageTransitionObserved = true;
    }
#endif
#ifdef TH_ENABLE_MULTIPLAYER_GAMEPLAY
    if (UsePauseCycle())
    {
        if (g_GameManager.isInPauseMenu || g_GameManager.isPaused)
            g_PauseObserved = true;
        else if (g_PauseObserved && frame > 680)
            g_PauseResumeObserved = true;
    }
#endif
#ifdef TH_ENABLE_MULTIPLAYER_GAMEPLAY
    bool allInputSlotsMatch = true;
    for (std::uint8_t player = 0; player < g_PlayerCount; ++player)
    {
        if (g_CurFrameGameInputs[player] != decision.inputs[player].buttons)
        {
            allInputSlotsMatch = false;
            // The original ReplayManager::RegisterChain contract clears both
            // input words while the next stage is installed. Accept only that
            // exact all-zero lifecycle boundary; the following frame must
            // prove that synchronized per-player input resumed.
            bool stageTransitionReset = stageChangedDuringCalc;
            for (std::uint8_t slot = 0; slot < g_PlayerCount; ++slot)
            {
                if (g_CurFrameGameInputs[slot] != 0 ||
                    g_LastFrameGameInputs[slot] != 0)
                    stageTransitionReset = false;
            }
            if (stageTransitionReset)
            {
                g_StageTransitionInputResetObserved = true;
                continue;
            }
            std::printf(
                "netplay lan stage: INPUT_MISMATCH frame=%u local=%u slot=%u expected=0x%04x actual=0x%04x raw=0x%04x lastRaw=0x%04x notInMenu=%d paused=%d pauseMenu=%d retryMenu=%d pauseState=%d resim=%d\n",
                frame, static_cast<unsigned>(g_LocalPlayer),
                static_cast<unsigned>(player),
                static_cast<unsigned>(decision.inputs[player].buttons),
                static_cast<unsigned>(g_CurFrameGameInputs[player]),
                static_cast<unsigned>(g_CurFrameRawInput),
                static_cast<unsigned>(g_LastFrameRawInput),
                g_GameManager.notInMenu ? 1 : 0,
                g_GameManager.isPaused ? 1 : 0,
                g_GameManager.isInPauseMenu ? 1 : 0,
                g_GameManager.isInRetryMenu ? 1 : 0,
                g_AsciiManager.pauseMenu.curState,
                resimulation ? 1 : 0);
            Fail("per-player input slot mismatch");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
        if (decision.inputs[player].buttons != 0 ||
            decision.inputs[player].analogMode != AnalogMode::None)
            g_InputSlotObservedMask |= static_cast<std::uint8_t>(1u << player);
    }
    if (g_StageTransitionObserved && !stageChangedDuringCalc && allInputSlotsMatch)
        g_PostTransitionInputObserved = true;
    if (decision.inputs[0] != decision.inputs[1] ||
        (g_PlayerCount >= 3 && decision.inputs[2] != decision.inputs[0]))
        g_IndependentInputSlotsObserved = true;
#endif
    Input::ClearPlayerButtonOverrides();
    Input::ClearReplayOverride();
    SideEffects::SetSpeculative(false);
    g_PeakEnemies = std::max(
        g_PeakEnemies, static_cast<std::uint32_t>(g_EnemyManager.enemyCountReal));
    g_PeakBullets = std::max(
        g_PeakBullets, static_cast<std::uint32_t>(g_BulletManager.bulletCount));
    g_PeakItems = std::max(
        g_PeakItems, static_cast<std::uint32_t>(g_ItemManager.activeItemCount));
    std::uint32_t activeLasers = 0;
    for (const auto &laser : g_BulletManager.lasers)
        activeLasers += laser.inUse ? 1u : 0u;
    g_PeakLasers = std::max(g_PeakLasers, activeLasers);
    if (UseStageTransitionTest() && !resimulation && (frame % 1800u) == 0u)
    {
        const i32 msg = g_Gui.impl ? g_Gui.impl->msg.currentMsgIdx : -999;
        const i32 msgTime = g_Gui.impl ? g_Gui.impl->msg.timer.GetCurrent() : -1;
        const i32 finishedStage = g_Gui.impl ? g_Gui.impl->finishedStage : -1;
        const i32 scoreTransition = g_Gui.impl ? g_Gui.impl->transitionToScoreScreen : -1;
        EclTimeline &timeline = g_EnemyManager.timelines[0];
        const i32 timelineTime = timeline.timelineTime.GetCurrent();
        const i32 timelineInstrTime = timeline.timelineInstr ? timeline.timelineInstr->time : -999;
        const i32 timelineOpcode = timeline.timelineInstr ? timeline.timelineInstr->opcode : -999;
        Enemy *boss = g_EnemyManager.bosses[0];
        std::printf(
            "netplay lan stage: PROGRESS player=%u frame=%u stage=%d stageFrames=%d msg=%d msgTime=%d finished=%d scoreTransition=%d enemies=%d timeline=%d/%d/%d boss=%d bossLife=%d/%d bossTimer=%d bossDamage=%d bossPos=%.1f/%.1f playerX=%.1f/%.1f/%.1f playerState=%d/%d/%d lives=%d/%d/%d\n",
            static_cast<unsigned>(g_LocalPlayer), frame,
            g_GameManager.currentStage, g_GameManager.framesThisStage,
            msg, msgTime, finishedStage, scoreTransition,
            g_EnemyManager.enemyCountReal, timelineTime, timelineInstrTime, timelineOpcode,
            boss ? 1 : 0, boss ? boss->life : -1, boss ? boss->maxLife : -1,
            boss ? boss->timer.GetCurrent() : -1, boss ? boss->canBeDamaged : 0,
            boss ? boss->pos.x : -999.0f, boss ? boss->pos.y : -999.0f,
            g_Players[0].positionCenter.x, g_Players[1].positionCenter.x,
            g_Players[2].positionCenter.x,
            g_Players[0].playerState, g_Players[1].playerState, g_Players[2].playerState,
            GetPlayerLives(0), GetPlayerLives(1), GetPlayerLives(2));
    }
    if (!Th07Rollback::EndFrame() || !g_Core.MarkSimulated(frame, decision))
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    g_MaxSnapshotBytes = std::max(g_MaxSnapshotBytes, Th07Rollback::CapturedBytes(frame));
#ifdef __EMSCRIPTEN__
    if (ProductionLanMode() && ((frame + 1u) % 300u) == 0u)
    {
        // This lives inside SimulateFrame rather than the outer driver so a
        // rollback resimulation overwrites the sample for the same logical
        // frame. Consumers only trust a sample once that frame is confirmed.
        const auto sample = Th07CanonicalHash::Capture();
        char total[17];
        HashToHex(sample.composite, total);
        EM_ASM({
            const hash = UTF8ToString($0);
            globalThis.__eaglerNetplayLanHash = hash;
            globalThis.__eaglerNetplayLanHashes[String($1)] = hash;
        }, total, frame + 1u);
    }
#endif
    if (resimulation)
        ++g_ResimulatedFrames;
    return result;
}

bool ReconcileRollback()
{
    if (!g_Core.HasRollbackRequest())
        return true;

    const std::uint32_t rollback = g_Core.RollbackFrame();
    const std::uint32_t last = g_Core.LastSimulatedFrame();
    if (last == INVALID_FRAME || rollback > last)
    {
        // The corrected frame has not run yet. Its exact input will be used
        // normally, so there is no state to rewind.
        g_Core.ClearRollbackRequest();
        return true;
    }
    std::uint32_t replayFrom = rollback;
    if (!Th07Rollback::RestoreTo(rollback, &replayFrom))
    {
        if (Th07Rollback::Failed())
            return false;
        // Stage changes deliberately invalidate the previous stage's journal.
        // Match final sbrik's mature failure policy: do not freeze/end the
        // whole session by retrying an impossible rewind forever. Continue on
        // the current state and surface the abandoned correction in the log.
        std::printf(
            "netplay lan stage: WARN rollback correction abandoned pending=%u last=%u\n",
            rollback, last);
        g_Core.ClearRollbackRequest();
        return true;
    }
    ++g_RollbackCount;
    g_MaxRollbackSpan = std::max(g_MaxRollbackSpan, last - rollback + 1);
    g_Core.ClearRollbackRequest();

    for (std::uint32_t frame = replayFrom; frame <= last; ++frame)
    {
        const FrameDecision decision = g_Core.PrepareFrame(frame);
        if (!decision.canAdvance)
            return false;
        const int result = SimulateFrame(frame, decision, true);
        if (result == CHAIN_CALLBACK_RESULT_EXIT_GAME_SUCCESS ||
            result == CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR)
            return false;
    }
    return true;
}
} // namespace

bool Active()
{
    return RequestedInternal() && g_Active;
}

bool TransportReady()
{
    return !ProductionLanMode() ||
           (g_ProductionTransportStarted && TransportIsOpen());
}

bool LastTickAdvanced()
{
    return !RequestedInternal() || g_LastTickAdvanced;
}

bool Requested()
{
    return RequestedInternal();
}

double SimulationIntervalScale()
{
    return ProductionLanMode() ? g_SimulationIntervalScale : 1.0;
}

int RunCalcChain()
{
    g_LastTickAdvanced = false;
    if (!RequestedInternal())
    {
        g_LastTickAdvanced = true;
        return g_Chain.RunCalcChain();
    }
    if (!g_Initialized && ProductionLanMode() &&
        !StartProductionTransportEarly())
    {
        Fail("transport preconnect");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }
    if (ProbeMode() && g_Done)
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    if (g_Initialized && !SessionStillOwnsStageState())
    {
        // Gameplay/retry/Ending ownership is one deterministic rollback
        // session. Once vanilla scene management takes over, retire that
        // session. A retry or later new run starts again from frame zero while
        // reusing only the already-established browser transport.
        RetireGameplaySession();
    }
    if (!g_Initialized && !EligibleForInitialNetplayStart())
    {
        // Stage/menu initialization remains vanilla. The first eligible call
        // occurs before this gameplay tick has run, then the netplay gate owns
        // all subsequent fixed-60-Hz simulation.
        g_LastTickAdvanced = true;
        return g_Chain.RunCalcChain();
    }
    if (!g_Initialized)
    {
        g_Initialized = true;
        if (!Initialize())
        {
            Fail("initialize");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
    }

    ++g_DriverTicks;
    if (!DrainPackets())
    {
        Fail("invalid packet");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }
    if (TransportFailed())
    {
        Fail("transport");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }
    if (RemoteInputsTimedOut())
    {
        Fail("remote input timeout");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }

    if (!g_Session.CanStart())
    {
        if (g_DriverTicks == 120)
        {
            std::printf(
                "netplay lan stage: GATE player=%u sent=%u recv=%u localReady=%d peer1=%d/%d peer2=%d/%d\n",
                static_cast<unsigned>(g_LocalPlayer), g_SessionPacketsSent,
                g_SessionPacketsReceived, g_Session.LocalReady() ? 1 : 0,
                g_Session.PeerHello(1) ? 1 : 0,
                g_Session.PeerReady(1) ? 1 : 0,
                g_Session.PeerHello(2) ? 1 : 0,
                g_Session.PeerReady(2) ? 1 : 0);
        }
        if (UsePhysicalInput() && g_DriverTicks <= 5)
        {
            std::printf(
                "netplay lan physical: GATE player=%u tick=%u sent=%u recv=%u canReady=%d\n",
                static_cast<unsigned>(g_LocalPlayer), g_DriverTicks,
                g_SessionPacketsSent, g_SessionPacketsReceived,
                g_Session.CanSendReady() ? 1 : 0);
        }
        if (!SendSessionControl())
        {
            Fail("session control");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
        // Control packets are deliberately separated from game inputs. Frame
        // zero is not scheduled until both peers have verified the complete
        // deterministic session contract and exchanged READY.
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (!ReconcileRollback())
    {
        Fail("rollback reconcile");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }
#ifdef __EMSCRIPTEN__
    if (ProductionLanMode())
    {
        const std::uint32_t confirmed = ConfirmedThroughAllRemotes();
        EM_ASM({
            globalThis.__eaglerNetplayLanConfirmed = $0;
            globalThis.__eaglerNetplayLanRollback = $1;
            globalThis.__eaglerNetplayLanResimulated = $2;
        }, confirmed, g_RollbackCount, g_ResimulatedFrames);
    }
#endif

    if ((!ProbeMode() || g_SimFrame < g_TestFrames) && TransportIsOpen())
    {
        bool localPresent = false;
        (void)g_Core.LocalInput(g_SimFrame, &localPresent);
        if (!localPresent && !SendLocalFrame(g_SimFrame))
        {
            Fail("send local input");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
        if (localPresent && (g_DriverTicks % 3u) == 0u &&
            !SendScheduledLocalFrame(g_SimFrame))
        {
            Fail("input retry");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }

        // Frame zero is the session barrier. Receive every peer's real first
        // input before gameplay advances. Later frames may use prediction.
        if (g_SimFrame == 0 && ConfirmedThroughAllRemotes() == INVALID_FRAME)
        {
            if (UsePhysicalInput() && !g_PhysicalLoggedFrame0Wait)
            {
                g_PhysicalLoggedFrame0Wait = true;
                std::printf(
                    "netplay lan physical: FRAME0 WAIT player=%u recv=%u session=%u/%u\n",
                    static_cast<unsigned>(g_LocalPlayer), g_ReceivedPackets,
                    g_SessionPacketsSent, g_SessionPacketsReceived);
            }
            // Even after our local gate opens, the peer may still be missing
            // our READY. Keep retransmitting READY until receiving peer frame
            // zero, which is an implicit acknowledgement that it crossed the
            // same session gate.
            if ((g_DriverTicks % 3u) == 0 && !SendSessionControl(true))
            {
                Fail("session ready keepalive");
                return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
            }
            return CHAIN_CALLBACK_RESULT_CONTINUE;
        }

        // Pause/retry UI is rewindable (GameManager + AsciiManager + relevant
        // Supervisor state are in the snapshot), but predicting menu input is
        // needlessly risky: a late Escape can change which UI frame consumes
        // subsequent keys.  Keep exchanging frames, but wait one round trip
        // for every remote player's real input while shared UI is active.
        if (SharedUiNeedsConfirmedInputs() &&
            ConfirmedThroughAllRemotes() < g_SimFrame)
            return CHAIN_CALLBACK_RESULT_CONTINUE;

        const FrameDecision decision = g_Core.PrepareFrame(g_SimFrame);
        if (!decision.canAdvance)
            return CHAIN_CALLBACK_RESULT_CONTINUE;
        PublishPeerDiagnostics(decision.predictedMask);
        if (decision.predictedMask)
            ++g_PredictedFrames;
        const int result = SimulateFrame(g_SimFrame, decision, false);
        if (result == CHAIN_CALLBACK_RESULT_EXIT_GAME_SUCCESS ||
            result == CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR)
            return result;
        ++g_SimFrame;
#ifdef __EMSCRIPTEN__
        if (ProductionLanMode())
            EM_ASM({ globalThis.__eaglerNetplayLanFrame = $0; }, g_SimFrame);
#endif
        g_LastTickAdvanced = true;
        return result;
    }

    if (!DrainPackets() || !ReconcileRollback())
    {
        Fail("final reconcile");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }
    // A low-rate tail keepalive is part of the protocol behavior, not a
    // transport retransmission. It closes the otherwise unavoidable hole where
    // the very last input or its ACK is the packet that gets lost.
    if ((g_DriverTicks % 3u) == 0 && !SendTailKeepalive())
    {
        Fail("tail keepalive");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }

    if (ProbeMode() && g_SimFrame >= g_TestFrames &&
        ConfirmedThroughAllRemotes() >= g_TestFrames - 1 &&
        !g_Core.HasRollbackRequest())
    {
#ifdef TH_ENABLE_MULTIPLAYER_GAMEPLAY
        const std::uint8_t expectedInputMask =
            static_cast<std::uint8_t>((1u << g_PlayerCount) - 1u);
        if (!g_IndependentInputSlotsObserved ||
            (g_InputSlotObservedMask & expectedInputMask) != expectedInputMask)
        {
            Fail("per-player input slots not exercised");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
        if (UsePhysicalInput() && !g_PhysicalInputObserved)
        {
            Fail("physical input not observed");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
        if (UsePauseCycle() && (!g_PauseObserved || !g_PauseResumeObserved))
        {
            Fail("pause cycle not completed");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
        if (UseStageTransitionTest() &&
            (!g_StageTransitionObserved || !g_StageTransitionInputResetObserved ||
             !g_PostTransitionInputObserved))
        {
            Fail("stage transition input lifecycle not completed");
            return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
        }
#endif
        const auto sample = Th07CanonicalHash::Capture();
        PrintHash("FINAL", sample);
#ifdef TH_ENABLE_MULTIPLAYER_GAMEPLAY
        if (g_PlayerCount >= 3)
        {
            std::printf(
                "netplay lan stage: PLAYERS p1=(%.2f,%.2f) p2=(%.2f,%.2f) p3=(%.2f,%.2f) independentInputs=1 physicalInput=%d\n",
                g_Players[0].positionCenter.x, g_Players[0].positionCenter.y,
                g_Players[1].positionCenter.x, g_Players[1].positionCenter.y,
                g_Players[2].positionCenter.x, g_Players[2].positionCenter.y,
                UsePhysicalInput() ? 1 : 0);
        }
        else
        {
            std::printf(
                "netplay lan stage: PLAYERS p1=(%.2f,%.2f) p2=(%.2f,%.2f) independentInputs=1 physicalInput=%d\n",
                g_Players[0].positionCenter.x, g_Players[0].positionCenter.y,
                g_Players[1].positionCenter.x, g_Players[1].positionCenter.y,
                UsePhysicalInput() ? 1 : 0);
        }
#endif
        std::printf(
            "netplay lan stage: PASS player=%u players=%u frames=%u sent=%u recv=%u session=%u/%u rollback=%u resim=%u maxRollback=%u predicted=%u confirmed=%u maxSnapshot=%llu buffered=%llu peak=%u/%u/%u/%u pause=%d/%d stageMax=%u\n",
            static_cast<unsigned>(g_LocalPlayer), static_cast<unsigned>(g_PlayerCount),
            g_TestFrames, g_SentPackets,
            g_ReceivedPackets, g_SessionPacketsSent, g_SessionPacketsReceived,
            g_RollbackCount, g_ResimulatedFrames, g_MaxRollbackSpan, g_PredictedFrames,
            static_cast<unsigned>(ConfirmedThroughAllRemotes()),
            static_cast<unsigned long long>(g_MaxSnapshotBytes),
            static_cast<unsigned long long>(TransportBufferedAmount()),
            g_PeakEnemies, g_PeakBullets, g_PeakLasers, g_PeakItems,
            g_PauseObserved ? 1 : 0, g_PauseResumeObserved ? 1 : 0,
            static_cast<unsigned>(g_HighestStageObserved));
        g_Done = true;
#ifdef __EMSCRIPTEN__
        EM_ASM({ globalThis.__eaglerNetplayLanStageDone = true; });
#endif
        return CHAIN_CALLBACK_RESULT_CONTINUE;
    }
    if (ProbeMode() && g_DriverTicks >= g_TestFrames * 6u)
    {
        Fail("timeout");
        return CHAIN_CALLBACK_RESULT_EXIT_GAME_ERROR;
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}
} // namespace Netplay::Th07LanStageProbe

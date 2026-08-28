#include "ReplayExtension.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL3/SDL_iostream.h>

#include "FileSystem.hpp"
#include "ReplayManager.hpp"

namespace ReplayExtension
{
namespace
{
struct InputSample
{
    f32 x;
    f32 y;
    u32 active;
};
struct DirectTouchSample
{
    f32 x;
    f32 y;
    u32 flags;
};
struct PackedFrameInput
{
    u16 buttons;
    u8 analogMode;
    u8 flags;
    f32 x;
    f32 y;
};
struct MultiplayerInputSample
{
    PackedFrameInput players[3];
};
static_assert(sizeof(InputSample) == 12);
static_assert(sizeof(DirectTouchSample) == 12);
static_assert(sizeof(TouchEvent) == 28);
static_assert(sizeof(PackedFrameInput) == 12);
static_assert(sizeof(MultiplayerInputSample) == 36);

std::vector<InputSample> g_RecordInputs[7];
std::vector<DirectTouchSample> g_RecordDirectTouch[7];
std::vector<TouchEvent> g_RecordTouchEvents[7];
std::vector<MultiplayerInputSample> g_RecordMultiplayerInputs[7];
std::vector<TouchEvent> g_PendingTouchEvents;
std::vector<InputSample> g_PlaybackInputs[7];
std::vector<DirectTouchSample> g_PlaybackDirectTouch[7];
std::vector<TouchEvent> g_PlaybackTouchEvents[7];
std::vector<MultiplayerInputSample> g_PlaybackMultiplayerInputs[7];
InputSample g_CapturedJoystick = {};
DirectTouchSample g_CapturedDirectTouch = {};
bool g_RecordUsesExtension = false;
bool g_RecordHasTouchState = false;
bool g_RecordHasMultiplayer = false;
MultiplayerReplayConfig g_RecordMultiplayerConfig{};
u32 g_PlaybackVersion = 0;
u32 g_PlaybackFlags = 0;
MultiplayerReplayConfig g_PlaybackMultiplayerConfig{};
i32 g_PlaybackStage = -1;
i32 g_PlaybackFrame = -1;
std::size_t g_PlaybackTouchStart = 0;
std::size_t g_PlaybackTouchCount = 0;

constexpr u32 VERSION = 1;
constexpr u32 DETERMINISM_ABI = 1;
constexpr u32 FLAG_JOYSTICK_INPUT = 1;
constexpr u32 FLAG_TOUCH_EVENTS = 2;
constexpr u32 FLAG_DIRECT_TOUCH_INPUT = 4;
constexpr u32 FLAG_TOUCH_STATE = 8;
constexpr u32 FLAG_MULTIPLAYER_INPUT = 32;
constexpr u32 DIRECT_TOUCH_ACTIVE = 1;
constexpr u32 DIRECT_TOUCH_UNLIMITED = 2;
constexpr u32 TOUCH_STATE_USED_THIS_RUN = 4;
constexpr u32 TOUCH_STATE_BOMBED_WITH_TOUCH = 8;
constexpr u32 TOUCH_STATE_CHEAT_MOVEMENT_USED = 16;
constexpr u32 TOUCH_STATE_MASK = TOUCH_STATE_USED_THIS_RUN | TOUCH_STATE_BOMBED_WITH_TOUCH |
                                 TOUCH_STATE_CHEAT_MOVEMENT_USED;
constexpr std::size_t HEADER_SIZE = 96;
constexpr std::size_t MP_HEADER_SIZE = 160;
constexpr std::size_t MP_PLAYER_COUNT_OFFSET = 96;
constexpr std::size_t MP_DIFFICULTY_OFFSET = 100;
constexpr std::size_t MP_GAMEPLAY_ABI_OFFSET = 104;
constexpr std::size_t MP_LOADOUT_OFFSET = 108;
constexpr std::size_t MP_COUNTS_OFFSET = 128;

u8 PackInputFlags(const Netplay::FrameInput &input)
{
    return (input.unlimited ? 1u : 0u) |
           (input.touchUsed ? 2u : 0u) |
           (input.touchBomb ? 4u : 0u);
}

PackedFrameInput PackInput(const Netplay::FrameInput &input)
{
    return {input.buttons, static_cast<u8>(input.analogMode), PackInputFlags(input),
            input.x, input.y};
}

Netplay::FrameInput UnpackInput(const PackedFrameInput &input)
{
    Netplay::FrameInput result;
    result.buttons = input.buttons;
    result.analogMode = input.analogMode <= static_cast<u8>(Netplay::AnalogMode::DirectTouch)
                            ? static_cast<Netplay::AnalogMode>(input.analogMode)
                            : Netplay::AnalogMode::None;
    result.x = input.x;
    result.y = input.y;
    result.unlimited = (input.flags & 1u) != 0;
    result.touchUsed = (input.flags & 2u) != 0;
    result.touchBomb = (input.flags & 4u) != 0;
    return result;
}

u32 ReadLe32(const u8 *bytes)
{
    return static_cast<u32>(bytes[0]) |
           (static_cast<u32>(bytes[1]) << 8) |
           (static_cast<u32>(bytes[2]) << 16) |
           (static_cast<u32>(bytes[3]) << 24);
}

void WriteLe32(u8 *bytes, u32 value)
{
    bytes[0] = static_cast<u8>(value);
    bytes[1] = static_cast<u8>(value >> 8);
    bytes[2] = static_cast<u8>(value >> 16);
    bytes[3] = static_cast<u8>(value >> 24);
}

bool FindTrailer(const u8 *bytes, std::size_t size, std::size_t &payloadOffset, std::size_t &payloadSize,
                 u32 *outVersion = nullptr, u32 *outFlags = nullptr)
{
    if (bytes == nullptr || size < HEADER_SIZE + 8 ||
        std::memcmp(bytes + size - 4, "EAGX", 4) != 0)
        return false;
    payloadSize = ReadLe32(bytes + size - 8);
    if (payloadSize < HEADER_SIZE || payloadSize > size - 8)
        return false;
    payloadOffset = size - 8 - payloadSize;
    const u32 version = ReadLe32(bytes + payloadOffset);
    const u32 flags = ReadLe32(bytes + payloadOffset + 4);
    if (version != VERSION)
        return false;
    const bool hasMultiplayer = (flags & FLAG_MULTIPLAYER_INPUT) != 0;
    const std::size_t headerSize = hasMultiplayer ? MP_HEADER_SIZE : HEADER_SIZE;
    if (payloadSize < headerSize || ReadLe32(bytes + payloadOffset + 92) != DETERMINISM_ABI)
        return false;
    if ((flags & (FLAG_JOYSTICK_INPUT | FLAG_TOUCH_EVENTS | FLAG_DIRECT_TOUCH_INPUT |
                  FLAG_TOUCH_STATE | FLAG_MULTIPLAYER_INPUT)) == 0)
        return false;
    if (hasMultiplayer)
    {
        const u32 playerCount = ReadLe32(bytes + payloadOffset + MP_PLAYER_COUNT_OFFSET);
        const u32 difficulty = ReadLe32(bytes + payloadOffset + MP_DIFFICULTY_OFFSET);
        if (playerCount < 2 || playerCount > 3 || difficulty > 5)
            return false;
        for (u32 player = 0; player < playerCount; ++player)
        {
            const u32 loadout = ReadLe32(bytes + payloadOffset + MP_LOADOUT_OFFSET + player * 4);
            if ((loadout & 0xffu) > 2 || ((loadout >> 8) & 0xffu) > 1)
                return false;
        }
    }
    std::size_t expected = headerSize;
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const u32 count = ReadLe32(bytes + payloadOffset + 8 + stage * 4);
        if (expected > payloadSize || count > (payloadSize - expected) / sizeof(InputSample))
            return false;
        expected += static_cast<std::size_t>(count) * sizeof(InputSample);
    }
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const u32 count = ReadLe32(bytes + payloadOffset + 36 + stage * 4);
        if (expected > payloadSize || count > (payloadSize - expected) / sizeof(TouchEvent))
            return false;
        expected += static_cast<std::size_t>(count) * sizeof(TouchEvent);
    }
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const u32 count = ReadLe32(bytes + payloadOffset + 64 + stage * 4);
        if (expected > payloadSize || count > (payloadSize - expected) / sizeof(DirectTouchSample))
            return false;
        expected += static_cast<std::size_t>(count) * sizeof(DirectTouchSample);
    }
    if (hasMultiplayer)
    {
        for (i32 stage = 0; stage < 7; ++stage)
        {
            const u32 count = ReadLe32(bytes + payloadOffset + MP_COUNTS_OFFSET + stage * 4);
            if (expected > payloadSize ||
                count > (payloadSize - expected) / sizeof(MultiplayerInputSample))
                return false;
            expected += static_cast<std::size_t>(count) * sizeof(MultiplayerInputSample);
        }
    }
    if (expected != payloadSize)
        return false;
    if (outVersion)
        *outVersion = version;
    if (outFlags)
        *outFlags = flags;
    return true;
}

void RecomputeRecordUsage()
{
    g_RecordUsesExtension = g_RecordHasMultiplayer;
    g_RecordHasTouchState = false;
    for (i32 stage = 0; stage < 7; ++stage)
    {
        for (const InputSample &sample : g_RecordInputs[stage])
            if (sample.active)
                g_RecordUsesExtension = true;
        if (!g_RecordTouchEvents[stage].empty())
            g_RecordUsesExtension = true;
        for (const DirectTouchSample &sample : g_RecordDirectTouch[stage])
        {
            if (sample.flags & DIRECT_TOUCH_ACTIVE)
                g_RecordUsesExtension = true;
            if (sample.flags & TOUCH_STATE_MASK)
            {
                g_RecordUsesExtension = true;
                g_RecordHasTouchState = true;
            }
        }
    }
}

bool EndsWith(const char *path, const char *suffix)
{
    if (!path || !suffix)
        return false;
    const std::size_t pathLength = std::strlen(path);
    const std::size_t suffixLength = std::strlen(suffix);
    if (suffixLength > pathLength)
        return false;
    return SDL_strcasecmp(path + pathLength - suffixLength, suffix) == 0;
}

std::string SwapExtension(const char *path, bool extended)
{
    std::string result = path ? path : "";
    if (EndsWith(result.c_str(), ".rpyx"))
        result.resize(result.size() - 5);
    else if (EndsWith(result.c_str(), ".rpy"))
        result.resize(result.size() - 4);
    result += extended ? ".rpyx" : ".rpy";
    return result;
}

bool ReadWholeFile(const char *path, std::vector<u8> &bytes)
{
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (!file)
        return false;
    const Sint64 size = SDL_GetIOSize(file);
    if (size <= 0 || size > 0x7fffffff || SDL_SeekIO(file, 0, SDL_IO_SEEK_SET) < 0)
    {
        SDL_CloseIO(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const bool ok = SDL_ReadIO(file, bytes.data(), bytes.size()) == bytes.size();
    SDL_CloseIO(file);
    if (!ok)
        bytes.clear();
    return ok;
}

bool WriteWholeFile(const char *path, const std::vector<u8> &bytes)
{
    SDL_IOStream *file = SDL_IOFromFile(path, "wb");
    if (!file)
        return false;
    const bool ok = SDL_WriteIO(file, bytes.data(), bytes.size()) == bytes.size();
    SDL_CloseIO(file);
    return ok;
}

void RecalculateChecksum(std::vector<u8> &bytes)
{
    std::vector<u8> checksumBytes(bytes.begin() + 13, bytes.end());
    u8 key = checksumBytes[0];
    for (std::size_t i = 0; i < bytes.size() - 16; ++i)
    {
        checksumBytes[3 + i] = static_cast<u8>(checksumBytes[3 + i] - key);
        key = static_cast<u8>(key + 7);
    }
    u32 checksum = 0x3f000318;
    for (u8 value : checksumBytes)
        checksum += value;
    WriteLe32(bytes.data() + offsetof(ReplayHeader, checksum), checksum);
}

bool PrepareAppend(const char *path, std::vector<u8> &bytes)
{
    if (!ReadWholeFile(path, bytes) || bytes.size() < sizeof(ReplayHeader) || std::memcmp(bytes.data(), "T7RP", 4) != 0)
        return false;

    std::size_t oldOffset = 0;
    std::size_t oldSize = 0;
    if (FindTrailer(bytes.data(), bytes.size(), oldOffset, oldSize))
        bytes.resize(oldOffset);
    return true;
}

bool AppendInputEvents(const char *path, const std::vector<InputSample> (&inputs)[7],
                       const std::vector<TouchEvent> (&touchEvents)[7],
                       const std::vector<DirectTouchSample> (&directTouch)[7],
                       const std::vector<MultiplayerInputSample> (&multiplayerInputs)[7],
                       const MultiplayerReplayConfig &multiplayerConfig)
{
    std::vector<u8> bytes;
    if (!PrepareAppend(path, bytes))
        return false;

    u32 flags = 0;
    std::size_t bodySize = 0;
    for (const auto &stage : inputs)
    {
        bodySize += stage.size() * sizeof(InputSample);
        for (const InputSample &sample : stage)
            if (sample.active)
                flags |= FLAG_JOYSTICK_INPUT;
    }
    for (const auto &stage : touchEvents)
    {
        bodySize += stage.size() * sizeof(TouchEvent);
        if (!stage.empty())
            flags |= FLAG_TOUCH_EVENTS;
    }
    for (const auto &stage : directTouch)
    {
        bodySize += stage.size() * sizeof(DirectTouchSample);
        for (const DirectTouchSample &sample : stage)
        {
            if (sample.flags & DIRECT_TOUCH_ACTIVE)
                flags |= FLAG_DIRECT_TOUCH_INPUT;
            if (sample.flags & TOUCH_STATE_MASK)
                flags |= FLAG_TOUCH_STATE;
        }
    }
    const bool hasMultiplayer = multiplayerConfig.playerCount >= 2 &&
                                multiplayerConfig.playerCount <= 3;
    if (hasMultiplayer)
    {
        flags |= FLAG_MULTIPLAYER_INPUT;
        for (const auto &stage : multiplayerInputs)
            bodySize += stage.size() * sizeof(MultiplayerInputSample);
    }
    if (flags == 0)
        return true;

    const std::size_t headerSize = hasMultiplayer ? MP_HEADER_SIZE : HEADER_SIZE;
    const std::size_t payloadSize = headerSize + bodySize;

    const std::size_t payloadOffset = bytes.size();
    bytes.resize(payloadOffset + payloadSize + 8, 0);
    WriteLe32(bytes.data() + payloadOffset, VERSION);
    WriteLe32(bytes.data() + payloadOffset + 4, flags);
    WriteLe32(bytes.data() + payloadOffset + 92, DETERMINISM_ABI);
    if (hasMultiplayer)
    {
        WriteLe32(bytes.data() + payloadOffset + MP_PLAYER_COUNT_OFFSET,
                  multiplayerConfig.playerCount);
        WriteLe32(bytes.data() + payloadOffset + MP_DIFFICULTY_OFFSET,
                  multiplayerConfig.difficulty);
        WriteLe32(bytes.data() + payloadOffset + MP_GAMEPLAY_ABI_OFFSET,
                  multiplayerConfig.gameplayAbi);
        for (u32 player = 0; player < 3; ++player)
        {
            const u32 loadout = static_cast<u32>(multiplayerConfig.characters[player]) |
                                (static_cast<u32>(multiplayerConfig.shots[player]) << 8);
            WriteLe32(bytes.data() + payloadOffset + MP_LOADOUT_OFFSET + player * 4,
                      loadout);
        }
    }

    std::size_t cursor = payloadOffset + headerSize;
    for (i32 stage = 0; stage < 7; ++stage)
    {
        WriteLe32(bytes.data() + payloadOffset + 8 + stage * 4, static_cast<u32>(inputs[stage].size()));
        WriteLe32(bytes.data() + payloadOffset + 36 + stage * 4,
                  static_cast<u32>(touchEvents[stage].size()));
        WriteLe32(bytes.data() + payloadOffset + 64 + stage * 4,
                  static_cast<u32>(directTouch[stage].size()));
        const std::size_t stageBytes = inputs[stage].size() * sizeof(InputSample);
        if (stageBytes != 0)
        {
            std::memcpy(bytes.data() + cursor, inputs[stage].data(), stageBytes);
            cursor += stageBytes;
        }
    }
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const std::size_t stageBytes = touchEvents[stage].size() * sizeof(TouchEvent);
        if (stageBytes != 0)
        {
            std::memcpy(bytes.data() + cursor, touchEvents[stage].data(), stageBytes);
            cursor += stageBytes;
        }
    }
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const std::size_t stageBytes = directTouch[stage].size() * sizeof(DirectTouchSample);
        if (stageBytes != 0)
        {
            std::memcpy(bytes.data() + cursor, directTouch[stage].data(), stageBytes);
            cursor += stageBytes;
        }
    }
    if (hasMultiplayer)
    {
        for (i32 stage = 0; stage < 7; ++stage)
        {
            WriteLe32(bytes.data() + payloadOffset + MP_COUNTS_OFFSET + stage * 4,
                      static_cast<u32>(multiplayerInputs[stage].size()));
            const std::size_t stageBytes =
                multiplayerInputs[stage].size() * sizeof(MultiplayerInputSample);
            if (stageBytes != 0)
            {
                std::memcpy(bytes.data() + cursor, multiplayerInputs[stage].data(), stageBytes);
                cursor += stageBytes;
            }
        }
    }
    WriteLe32(bytes.data() + payloadOffset + payloadSize, static_cast<u32>(payloadSize));
    std::memcpy(bytes.data() + payloadOffset + payloadSize + 4, "EAGX", 4);

    RecalculateChecksum(bytes);
    return WriteWholeFile(path, bytes);
}
} // namespace

void ResetRecording()
{
    for (auto &stage : g_RecordInputs)
        stage.clear();
    for (auto &stage : g_RecordDirectTouch)
        stage.clear();
    for (auto &stage : g_RecordTouchEvents)
        stage.clear();
    for (auto &stage : g_RecordMultiplayerInputs)
        stage.clear();
    g_PendingTouchEvents.clear();
    g_CapturedJoystick = {};
    g_CapturedDirectTouch = {};
    g_RecordUsesExtension = false;
    g_RecordHasTouchState = false;
    g_RecordHasMultiplayer = false;
    g_RecordMultiplayerConfig = {};
}

void BeginStageRecording(i32 stage)
{
    stage = std::clamp(stage, 0, 6);
    g_RecordInputs[stage].clear();
    g_RecordDirectTouch[stage].clear();
    g_RecordTouchEvents[stage].clear();
    g_RecordMultiplayerInputs[stage].clear();
    g_PendingTouchEvents.clear();
    g_CapturedJoystick = {};
    g_CapturedDirectTouch = {};
    RecomputeRecordUsage();
}

void BeginMultiplayerRecording(const MultiplayerReplayConfig &config)
{
    if (config.playerCount < 2 || config.playerCount > 3 || config.difficulty > 5)
        return;
    g_RecordMultiplayerConfig = config;
    g_RecordHasMultiplayer = true;
    g_RecordUsesExtension = true;
}

void RecordMultiplayerFrame(i32 stage, i32 frame, const Netplay::FrameInput *inputs,
                            std::size_t count)
{
    if (!g_RecordHasMultiplayer || !inputs || frame < 0 ||
        count < g_RecordMultiplayerConfig.playerCount)
        return;
    stage = std::clamp(stage, 0, 6);
    auto &samples = g_RecordMultiplayerInputs[stage];
    if (samples.size() <= static_cast<std::size_t>(frame))
        samples.resize(static_cast<std::size_t>(frame) + 1, {});
    MultiplayerInputSample &sample = samples[frame];
    for (u32 player = 0; player < 3; ++player)
        sample.players[player] = player < g_RecordMultiplayerConfig.playerCount
                                     ? PackInput(inputs[player])
                                     : PackedFrameInput{};
    g_RecordUsesExtension = true;
}

void BeginInputFrame()
{
    g_CapturedJoystick = {};
    g_CapturedDirectTouch = {};
}

void CaptureJoystick(f32 x, f32 y)
{
    g_CapturedJoystick = {x, y, 1};
    g_RecordUsesExtension = true;
}

void CaptureDirectTouch(f32 x, f32 y, bool unlimited)
{
    g_CapturedDirectTouch = {x, y, DIRECT_TOUCH_ACTIVE | (unlimited ? DIRECT_TOUCH_UNLIMITED : 0)};
    g_RecordUsesExtension = true;
}

void CaptureTouchState(bool usedThisRun, bool bombedWithTouch, bool cheatMovementUsed)
{
    if (!usedThisRun && !bombedWithTouch && !cheatMovementUsed && !g_RecordHasTouchState)
        return;
    g_RecordHasTouchState = true;
    g_RecordUsesExtension = true;
    g_CapturedDirectTouch.flags &= ~TOUCH_STATE_MASK;
    if (usedThisRun)
        g_CapturedDirectTouch.flags |= TOUCH_STATE_USED_THIS_RUN;
    if (bombedWithTouch)
        g_CapturedDirectTouch.flags |= TOUCH_STATE_BOMBED_WITH_TOUCH;
    if (cheatMovementUsed)
        g_CapturedDirectTouch.flags |= TOUCH_STATE_CHEAT_MOVEMENT_USED;
}

void CaptureTouchEvent(i32 fingerId, f32 x, f32 y, u32 action, u32 role, u32 flags)
{
    if (action < TOUCH_ACTION_DOWN || action > TOUCH_ACTION_CANCEL_ALL)
        return;
    g_PendingTouchEvents.push_back({0, fingerId, x, y, action, role, flags});
    g_RecordUsesExtension = true;
}

void CaptureTouchCancelAll()
{
    CaptureTouchEvent(0, 0.0f, 0.0f, TOUCH_ACTION_CANCEL_ALL, TOUCH_ROLE_NONE, 0);
}

void RecordFrame(i32 stage, i32 frame)
{
    stage = std::clamp(stage, 0, 6);
    if (frame < 0)
        return;
    auto &samples = g_RecordInputs[stage];
    if (samples.size() <= static_cast<std::size_t>(frame))
        samples.resize(static_cast<std::size_t>(frame) + 1, {0.0f, 0.0f, 0});
    samples[frame] = g_CapturedJoystick;
    g_CapturedJoystick = {};

    auto &touchSamples = g_RecordDirectTouch[stage];
    if (touchSamples.size() <= static_cast<std::size_t>(frame))
        touchSamples.resize(static_cast<std::size_t>(frame) + 1, {0.0f, 0.0f, 0});
    touchSamples[frame] = g_CapturedDirectTouch;
    g_CapturedDirectTouch = {};

    if (!g_PendingTouchEvents.empty())
    {
        auto &events = g_RecordTouchEvents[stage];
        for (TouchEvent &event : g_PendingTouchEvents)
            event.frame = static_cast<u32>(frame);
        events.insert(events.end(), g_PendingTouchEvents.begin(), g_PendingTouchEvents.end());
        g_PendingTouchEvents.clear();
    }
}

std::string ResolveSavePath(const char *requestedPath)
{
    return SwapExtension(requestedPath, g_RecordUsesExtension);
}

void RemoveAlternateSave(const char *savedPath)
{
    const std::string alternate = SwapExtension(savedPath, !EndsWith(savedPath, ".rpyx"));
    std::remove(alternate.c_str());
}

bool AppendRecording(const char *path)
{
    if (!g_RecordUsesExtension)
        return true;
    return AppendInputEvents(path, g_RecordInputs, g_RecordTouchEvents, g_RecordDirectTouch,
                             g_RecordMultiplayerInputs, g_RecordMultiplayerConfig);
}

bool AppendPlayback(const char *path)
{
    if (g_PlaybackVersion == 0)
        return true;
    if (g_PlaybackVersion != VERSION)
        return false;
    return AppendInputEvents(path, g_PlaybackInputs, g_PlaybackTouchEvents, g_PlaybackDirectTouch,
                             g_PlaybackMultiplayerInputs, g_PlaybackMultiplayerConfig);
}

void ClearPlayback()
{
    for (auto &stage : g_PlaybackInputs)
        stage.clear();
    for (auto &stage : g_PlaybackDirectTouch)
        stage.clear();
    for (auto &stage : g_PlaybackTouchEvents)
        stage.clear();
    for (auto &stage : g_PlaybackMultiplayerInputs)
        stage.clear();
    g_PlaybackVersion = 0;
    g_PlaybackFlags = 0;
    g_PlaybackMultiplayerConfig = {};
    g_PlaybackStage = -1;
    g_PlaybackFrame = -1;
    g_PlaybackTouchStart = 0;
    g_PlaybackTouchCount = 0;
}

bool LoadPlayback(const u8 *bytes, std::size_t size)
{
    ClearPlayback();
    std::size_t payloadOffset = 0;
    std::size_t payloadSize = 0;
    u32 version = 0;
    u32 flags = 0;
    if (!FindTrailer(bytes, size, payloadOffset, payloadSize, &version, &flags))
        return false;
    g_PlaybackVersion = version;
    g_PlaybackFlags = flags;

    const bool hasMultiplayer = (flags & FLAG_MULTIPLAYER_INPUT) != 0;
    const std::size_t headerSize = hasMultiplayer ? MP_HEADER_SIZE : HEADER_SIZE;
    if (hasMultiplayer)
    {
        g_PlaybackMultiplayerConfig.playerCount =
            static_cast<u8>(ReadLe32(bytes + payloadOffset + MP_PLAYER_COUNT_OFFSET));
        g_PlaybackMultiplayerConfig.difficulty =
            static_cast<u8>(ReadLe32(bytes + payloadOffset + MP_DIFFICULTY_OFFSET));
        g_PlaybackMultiplayerConfig.gameplayAbi =
            ReadLe32(bytes + payloadOffset + MP_GAMEPLAY_ABI_OFFSET);
        for (u32 player = 0; player < 3; ++player)
        {
            const u32 loadout = ReadLe32(bytes + payloadOffset + MP_LOADOUT_OFFSET + player * 4);
            g_PlaybackMultiplayerConfig.characters[player] = static_cast<u8>(loadout & 0xffu);
            g_PlaybackMultiplayerConfig.shots[player] = static_cast<u8>((loadout >> 8) & 0xffu);
        }
    }

    std::size_t cursor = payloadOffset + headerSize;
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const u32 count = ReadLe32(bytes + payloadOffset + 8 + stage * 4);
        g_PlaybackInputs[stage].resize(count);
        const std::size_t stageBytes = static_cast<std::size_t>(count) * sizeof(InputSample);
        if (stageBytes != 0)
        {
            std::memcpy(g_PlaybackInputs[stage].data(), bytes + cursor, stageBytes);
            cursor += stageBytes;
        }
    }
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const u32 count = ReadLe32(bytes + payloadOffset + 36 + stage * 4);
        g_PlaybackTouchEvents[stage].resize(count);
        const std::size_t stageBytes = static_cast<std::size_t>(count) * sizeof(TouchEvent);
        if (stageBytes != 0)
        {
            std::memcpy(g_PlaybackTouchEvents[stage].data(), bytes + cursor, stageBytes);
            cursor += stageBytes;
        }
    }
    for (i32 stage = 0; stage < 7; ++stage)
    {
        const u32 count = ReadLe32(bytes + payloadOffset + 64 + stage * 4);
        g_PlaybackDirectTouch[stage].resize(count);
        const std::size_t stageBytes = static_cast<std::size_t>(count) * sizeof(DirectTouchSample);
        if (stageBytes != 0)
        {
            std::memcpy(g_PlaybackDirectTouch[stage].data(), bytes + cursor, stageBytes);
            cursor += stageBytes;
        }
    }
    if (hasMultiplayer)
    {
        for (i32 stage = 0; stage < 7; ++stage)
        {
            const u32 count = ReadLe32(bytes + payloadOffset + MP_COUNTS_OFFSET + stage * 4);
            g_PlaybackMultiplayerInputs[stage].resize(count);
            const std::size_t stageBytes =
                static_cast<std::size_t>(count) * sizeof(MultiplayerInputSample);
            if (stageBytes != 0)
            {
                std::memcpy(g_PlaybackMultiplayerInputs[stage].data(), bytes + cursor, stageBytes);
                cursor += stageBytes;
            }
        }
    }
    return true;
}

void SetPlaybackFrame(i32 stage, i32 frame)
{
    g_PlaybackStage = std::clamp(stage, 0, 6);
    g_PlaybackFrame = frame;
    g_PlaybackTouchStart = 0;
    g_PlaybackTouchCount = 0;
    if (g_PlaybackVersion != VERSION || frame < 0)
        return;

    const auto &events = g_PlaybackTouchEvents[g_PlaybackStage];
    const auto first = std::lower_bound(events.begin(), events.end(), static_cast<u32>(frame),
                                        [](const TouchEvent &event, u32 value) { return event.frame < value; });
    const auto last = std::upper_bound(first, events.end(), static_cast<u32>(frame),
                                       [](u32 value, const TouchEvent &event) { return value < event.frame; });
    g_PlaybackTouchStart = static_cast<std::size_t>(first - events.begin());
    g_PlaybackTouchCount = static_cast<std::size_t>(last - first);
}

bool PlaybackActive()
{
    return g_PlaybackVersion != 0;
}

bool MultiplayerPlaybackActive()
{
    return g_PlaybackVersion == VERSION &&
           (g_PlaybackFlags & FLAG_MULTIPLAYER_INPUT) != 0 &&
           g_PlaybackMultiplayerConfig.playerCount >= 2;
}

bool GetMultiplayerPlaybackConfig(MultiplayerReplayConfig *out)
{
    if (!out || !MultiplayerPlaybackActive())
        return false;
    *out = g_PlaybackMultiplayerConfig;
    return true;
}

bool GetMultiplayerPlaybackFrame(i32 stage, i32 frame, Netplay::FrameInput *inputs,
                                 std::size_t count)
{
    if (!inputs || !MultiplayerPlaybackActive() || frame < 0 ||
        count < g_PlaybackMultiplayerConfig.playerCount)
        return false;
    stage = std::clamp(stage, 0, 6);
    const auto &samples = g_PlaybackMultiplayerInputs[stage];
    if (static_cast<std::size_t>(frame) >= samples.size())
        return false;
    for (u32 player = 0; player < g_PlaybackMultiplayerConfig.playerCount; ++player)
        inputs[player] = UnpackInput(samples[frame].players[player]);
    for (u32 player = g_PlaybackMultiplayerConfig.playerCount; player < count; ++player)
        inputs[player] = {};
    return true;
}

bool UsesFixedTickTouchPlayback()
{
    return g_PlaybackVersion == VERSION;
}

bool GetPlaybackJoystick(f32 *x, f32 *y)
{
    if (MultiplayerPlaybackActive())
        return false;
    if (g_PlaybackVersion != VERSION ||
        g_PlaybackStage < 0 || g_PlaybackFrame < 0 ||
        static_cast<std::size_t>(g_PlaybackFrame) >= g_PlaybackInputs[g_PlaybackStage].size())
        return false;
    const InputSample &sample = g_PlaybackInputs[g_PlaybackStage][g_PlaybackFrame];
    if (!sample.active)
        return false;
    *x = sample.x;
    *y = sample.y;
    return true;
}

bool GetPlaybackDirectTouch(f32 *x, f32 *y, bool *unlimited)
{
    if (MultiplayerPlaybackActive())
        return false;
    if (g_PlaybackVersion != VERSION ||
        g_PlaybackStage < 0 || g_PlaybackFrame < 0 ||
        static_cast<std::size_t>(g_PlaybackFrame) >= g_PlaybackDirectTouch[g_PlaybackStage].size())
        return false;
    const DirectTouchSample &sample = g_PlaybackDirectTouch[g_PlaybackStage][g_PlaybackFrame];
    if ((sample.flags & DIRECT_TOUCH_ACTIVE) == 0)
        return false;
    *x = sample.x;
    *y = sample.y;
    if (unlimited)
        *unlimited = (sample.flags & DIRECT_TOUCH_UNLIMITED) != 0;
    return true;
}

bool GetPlaybackTouchState(bool *usedThisRun, bool *bombedWithTouch, bool *cheatMovementUsed)
{
    if (g_PlaybackVersion != VERSION ||
        (g_PlaybackFlags & FLAG_TOUCH_STATE) == 0 ||
        g_PlaybackStage < 0 || g_PlaybackFrame < 0 ||
        static_cast<std::size_t>(g_PlaybackFrame) >= g_PlaybackDirectTouch[g_PlaybackStage].size())
        return false;
    const u32 flags = g_PlaybackDirectTouch[g_PlaybackStage][g_PlaybackFrame].flags;
    if (usedThisRun)
        *usedThisRun = (flags & TOUCH_STATE_USED_THIS_RUN) != 0;
    if (bombedWithTouch)
        *bombedWithTouch = (flags & TOUCH_STATE_BOMBED_WITH_TOUCH) != 0;
    if (cheatMovementUsed)
        *cheatMovementUsed = (flags & TOUCH_STATE_CHEAT_MOVEMENT_USED) != 0;
    return true;
}

const TouchEvent *GetPlaybackTouchEvents(std::size_t *count)
{
    if (count)
        *count = g_PlaybackTouchCount;
    if (g_PlaybackVersion != VERSION ||
        g_PlaybackStage < 0 || g_PlaybackTouchCount == 0)
        return nullptr;
    return g_PlaybackTouchEvents[g_PlaybackStage].data() + g_PlaybackTouchStart;
}

std::size_t BaseFileSize(const u8 *bytes, std::size_t size)
{
    std::size_t payloadOffset = 0;
    std::size_t payloadSize = 0;
    return FindTrailer(bytes, size, payloadOffset, payloadSize) ? payloadOffset : size;
}

bool MatchesPath(const char *path, const u8 *bytes, std::size_t size)
{
    const bool extendedPath = EndsWith(path, ".rpyx");
    std::size_t payloadOffset = 0;
    std::size_t payloadSize = 0;
    const bool extendedData = FindTrailer(bytes, size, payloadOffset, payloadSize);
    return extendedPath == extendedData;
}

#ifdef TH_DEV_TOOLS
bool DebugRoundTrip(const char *path)
{
    if (!path || !*path)
        return false;

    ResetRecording();
    const bool normalSuffix = EndsWith(ResolveSavePath(path).c_str(), ".rpy");
    BeginInputFrame();
    CaptureTouchEvent(9, 0.9f, 0.1f, TOUCH_ACTION_DOWN, TOUCH_ROLE_MOVE, 0);
    RecordFrame(0, 5);
    BeginStageRecording(0);
    BeginInputFrame();
    CaptureJoystick(0.5f, -0.25f);
    CaptureDirectTouch(3.25f, -7.5f, true);
    CaptureTouchState(true, true, true);
    CaptureTouchEvent(1, 0.25f, 0.75f, TOUCH_ACTION_DOWN, TOUCH_ROLE_MOVE, 0);
    CaptureTouchEvent(1, 0.50f, 0.60f, TOUCH_ACTION_MOTION, TOUCH_ROLE_MOVE, 0);
    RecordFrame(0, 0);
    BeginInputFrame();
    CaptureTouchState(true, false, true);
    CaptureTouchEvent(1, 0.50f, 0.60f, TOUCH_ACTION_UP, TOUCH_ROLE_MOVE, 0);
    RecordFrame(0, 1);
    const std::string extendedPath = ResolveSavePath(path);
    if (!normalSuffix || !EndsWith(extendedPath.c_str(), ".rpyx"))
        return false;

    std::vector<u8> base(sizeof(ReplayHeader), 0);
    std::memcpy(base.data(), "T7RP", 4);
    base[offsetof(ReplayHeader, key)] = 0x40;
    if (!WriteWholeFile(extendedPath.c_str(), base) || !AppendRecording(extendedPath.c_str()))
        return false;

    std::vector<u8> bytes;
    const bool read = ReadWholeFile(extendedPath.c_str(), bytes);
    const bool identity = read && MatchesPath(extendedPath.c_str(), bytes.data(), bytes.size()) &&
                          !MatchesPath(path, bytes.data(), bytes.size()) && BaseFileSize(bytes.data(), bytes.size()) == base.size();
    const bool loaded = identity && LoadPlayback(bytes.data(), bytes.size());
    SetPlaybackFrame(0, 0);
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 touchX = 0.0f;
    f32 touchY = 0.0f;
    bool unlimited = false;
    bool usedThisRun = false;
    bool bombedWithTouch = false;
    bool cheatMovementUsed = false;
    std::size_t eventCount = 0;
    const TouchEvent *events = GetPlaybackTouchEvents(&eventCount);
    const bool frame0 = loaded && GetPlaybackJoystick(&x, &y) &&
                        std::abs(x - 0.5f) < 0.0001f && std::abs(y + 0.25f) < 0.0001f &&
                        GetPlaybackDirectTouch(&touchX, &touchY, &unlimited) && unlimited &&
                        std::abs(touchX - 3.25f) < 0.0001f && std::abs(touchY + 7.5f) < 0.0001f &&
                        GetPlaybackTouchState(&usedThisRun, &bombedWithTouch, &cheatMovementUsed) &&
                        usedThisRun && bombedWithTouch && cheatMovementUsed &&
                        eventCount == 2 && events != nullptr &&
                        events[0].action == TOUCH_ACTION_DOWN && events[0].role == TOUCH_ROLE_MOVE &&
                        events[1].action == TOUCH_ACTION_MOTION &&
                        std::abs(events[1].x - 0.50f) < 0.0001f && std::abs(events[1].y - 0.60f) < 0.0001f;
    SetPlaybackFrame(0, 1);
    events = GetPlaybackTouchEvents(&eventCount);
    usedThisRun = bombedWithTouch = cheatMovementUsed = false;
    const bool frame1 = eventCount == 1 && events != nullptr && events[0].action == TOUCH_ACTION_UP &&
                        GetPlaybackTouchState(&usedThisRun, &bombedWithTouch, &cheatMovementUsed) &&
                        usedThisRun && !bombedWithTouch && cheatMovementUsed;
    SetPlaybackFrame(0, 5);
    events = GetPlaybackTouchEvents(&eventCount);
    const bool oldAttemptGone = eventCount == 0 && events == nullptr &&
                                !GetPlaybackDirectTouch(&touchX, &touchY, &unlimited);

    std::size_t payloadOffset = 0;
    std::size_t payloadSize = 0;
    u32 version = 0;
    const bool abiPresent = FindTrailer(bytes.data(), bytes.size(), payloadOffset, payloadSize, &version) &&
                            version == VERSION && ReadLe32(bytes.data() + payloadOffset + 92) == DETERMINISM_ABI;
    std::vector<u8> incompatible = bytes;
    if (abiPresent)
        WriteLe32(incompatible.data() + payloadOffset + 92, DETERMINISM_ABI + 1);
    const bool incompatibleRejected = abiPresent && !LoadPlayback(incompatible.data(), incompatible.size());
    std::vector<u8> developmentVersion = bytes;
    if (abiPresent)
        WriteLe32(developmentVersion.data() + payloadOffset, VERSION + 1);
    const bool developmentVersionRejected = abiPresent &&
                                            !LoadPlayback(developmentVersion.data(), developmentVersion.size());

    // EAGX v1 multiplayer type round-trip: save and load all synchronized logical
    // lanes, including analog/touch flags, rather than a local-controller
    // approximation. This is the same payload SaveReplay uses in live MP.
    ResetRecording();
    MultiplayerReplayConfig mpConfig;
    mpConfig.playerCount = 3;
    mpConfig.difficulty = 3;
    mpConfig.gameplayAbi = 2;
    mpConfig.characters[0] = 0;
    mpConfig.shots[0] = 1;
    mpConfig.characters[1] = 1;
    mpConfig.shots[1] = 0;
    mpConfig.characters[2] = 2;
    mpConfig.shots[2] = 1;
    BeginMultiplayerRecording(mpConfig);
    BeginStageRecording(0);
    Netplay::FrameInput mpInputs[3]{};
    mpInputs[0].buttons = 0x11;
    mpInputs[1].buttons = 0x22;
    mpInputs[1].analogMode = Netplay::AnalogMode::Joystick;
    mpInputs[1].x = 0.25f;
    mpInputs[1].y = -0.75f;
    mpInputs[2].buttons = 0x44;
    mpInputs[2].analogMode = Netplay::AnalogMode::DirectTouch;
    mpInputs[2].x = 6.5f;
    mpInputs[2].y = -2.25f;
    mpInputs[2].unlimited = true;
    mpInputs[2].touchUsed = true;
    mpInputs[2].touchBomb = true;
    RecordMultiplayerFrame(0, 0, mpInputs, 3);
    const std::string multiplayerPath = ResolveSavePath(path);
    const bool multiplayerWritten =
        WriteWholeFile(multiplayerPath.c_str(), base) && AppendRecording(multiplayerPath.c_str());
    std::vector<u8> multiplayerBytes;
    const bool multiplayerRead = multiplayerWritten &&
        ReadWholeFile(multiplayerPath.c_str(), multiplayerBytes) &&
        LoadPlayback(multiplayerBytes.data(), multiplayerBytes.size());
    MultiplayerReplayConfig loadedConfig;
    Netplay::FrameInput loadedInputs[3]{};
    const bool multiplayerRoundTrip = multiplayerRead &&
        GetMultiplayerPlaybackConfig(&loadedConfig) && loadedConfig.playerCount == 3 &&
        loadedConfig.difficulty == 3 && loadedConfig.gameplayAbi == 2 &&
        loadedConfig.characters[0] == 0 && loadedConfig.shots[0] == 1 &&
        loadedConfig.characters[1] == 1 && loadedConfig.shots[1] == 0 &&
        loadedConfig.characters[2] == 2 && loadedConfig.shots[2] == 1 &&
        GetMultiplayerPlaybackFrame(0, 0, loadedInputs, 3) &&
        loadedInputs[0].buttons == mpInputs[0].buttons &&
        loadedInputs[1].buttons == mpInputs[1].buttons &&
        loadedInputs[1].analogMode == Netplay::AnalogMode::Joystick &&
        std::abs(loadedInputs[1].x - mpInputs[1].x) < 0.0001f &&
        std::abs(loadedInputs[1].y - mpInputs[1].y) < 0.0001f &&
        loadedInputs[2].buttons == mpInputs[2].buttons &&
        loadedInputs[2].analogMode == Netplay::AnalogMode::DirectTouch &&
        loadedInputs[2].unlimited && loadedInputs[2].touchUsed && loadedInputs[2].touchBomb &&
        std::abs(loadedInputs[2].x - mpInputs[2].x) < 0.0001f &&
        std::abs(loadedInputs[2].y - mpInputs[2].y) < 0.0001f;

    std::remove(extendedPath.c_str());
    std::remove(multiplayerPath.c_str());
    ResetRecording();
    ClearPlayback();
    return frame0 && frame1 && oldAttemptGone && incompatibleRejected &&
           developmentVersionRejected && multiplayerRoundTrip;
}
#endif
} // namespace ReplayExtension

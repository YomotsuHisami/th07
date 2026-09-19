#pragma once

// Gameplay-only multiplayer constants imported from sbrik1111/th07_multi_player.
//
// Keep this header deliberately free of transport/session/rollback types.  The
// original game owners may depend on a fixed player-slot count, but networking
// remains isolated under src/netplay/ so the gameplay feature can be exercised
// locally without pulling a socket implementation into Player/GameManager.
constexpr int TH07_MULTI_MAX_PLAYERS = 3;
constexpr int TH07_MULTI_MAX_GUESTS = TH07_MULTI_MAX_PLAYERS - 1;
// ABI 5 adds once-only touch displacement streams and rewindable remainder.
// Older live peers must reject the session before seeing the new analog modes.
constexpr unsigned int TH07_MULTI_GAMEPLAY_ABI = 5;
// Live synchronization also covers fixed-tick presentation state. ABI 6 moves
// item appearance transitions out of Draw. Keep Replay's gameplay/input ABI 5:
// its input encoding and gameplay rules are unchanged and old files remain readable.
constexpr unsigned int TH07_MULTI_NETPLAY_ABI = 6;

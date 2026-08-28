#!/usr/bin/env python3
"""Focused contracts for the sbrik ReplayManager feature-rebase class."""

from __future__ import annotations

import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "ReplayManager.cpp").read_text(encoding="utf-8")
EXTENSION = (ROOT / "src" / "ReplayExtension.cpp").read_text(encoding="utf-8")
BASE = "5b9ebe892914ff5666ef68c0cd02719dde7d4ee9"
FINAL = "022c533"


def require(name: str, condition: bool) -> None:
    if not condition:
        raise AssertionError(name)
    print(f"PASS: {name}")


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True, encoding="utf-8"
    ).replace("\r\n", "\n")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(signature)


def main() -> None:
    diff = git("diff", "--unified=0", f"{BASE}..{FINAL}", "--",
               "src/th07/ReplayManager.cpp")
    require("frozen ReplayManager.cpp has eight hunks",
            len(re.findall(r"^@@ ", diff, flags=re.MULTILINE)) == 8)
    require("native Netplay replay shell is excluded",
            '#include "Netplay.hpp"' not in SOURCE and "Netplay::NoSave" not in SOURCE)

    update = function_body(SOURCE, "u32 ReplayManager::OnUpdate(ReplayManager")
    require("netplay does not advance logical edges twice",
            "if (!Netplay::Input::PlayerButtonOverridesActive())" in update)
    require("non-netplay multiplayer advances all previous lanes",
            "g_LastFrameGameInputs[playerId] = g_CurFrameGameInputs[playerId]" in update)
    require("non-netplay fallback keeps only physical P1",
            "g_CurFrameGameInputs[0] = g_CurFrameRawInput" in update and
            "g_CurFrameGameInputs[playerId] = 0" in update)
    require("speculative pass does not append replay frames",
            "if (Netplay::SideEffects::IsSpeculative())" in update)
    require("committed multiplayer replay records synchronized FrameInput lanes",
            "ReplayExtension::RecordMultiplayerFrame" in update and
            "Netplay::Input::PlayerInputOverride" in update)

    demo = function_body(SOURCE, "u32 ReplayManager::OnUpdateDemoHighPrio")
    require("vanilla replay playback clears P2 and P3",
            "for (i32 playerId = 1; playerId < TH07_MULTI_MAX_PLAYERS; ++playerId)" in demo and
            "g_CurFrameGameInputs[playerId] = 0" in demo)
    require("EAGX playback frame is applied before vanilla input",
            demo.index("ReplayExtension::SetPlaybackFrame") <
            demo.index("g_CurFrameGameInput = arg->replayInputs->frameNum"))
    require("multiplayer replay restores every synchronized player lane",
            "ReplayExtension::GetMultiplayerPlaybackFrame" in demo and
            "Netplay::Input::SetPlayerInputOverrides" in demo)

    require("replay Border cap uses shared multiplayer threshold",
            "const i32 borderThreshold" in SOURCE and
            "GetSharedBorderThreshold();" in SOURCE and
            "cherryStart + borderThreshold" in SOURCE)
    register = function_body(SOURCE, "ZunResult ReplayManager::RegisterChain")
    require("replay registration clears every player lane",
            "playerId < TH07_MULTI_MAX_PLAYERS" in register and
            "g_LastFrameGameInputs[playerId] = 0" in register and
            "g_CurFrameGameInputs[playerId] = 0" in register)

    save = function_body(SOURCE, "void ReplayManager::SaveReplay(")
    save2 = function_body(SOURCE, "void ReplayManager::SaveReplay2(")
    require("multiplayer save is no longer blocked by the vanilla single-lane guard",
            "MultiplayerGameplay::IsMultiplayer()" not in save and
            "MultiplayerGameplay::IsMultiplayer()" not in save2)
    require("ordinary save retains EAGX recording append",
            "ReplayExtension::AppendRecording" in save)
    require("ordinary rewrite retains EAGX playback append",
            "ReplayExtension::AppendPlayback" in save2)
    require("EAGX touch events remain per frame",
            "ReplayExtension::GetPlaybackTouchEvents" in SOURCE and
            "Touch::ApplyReplayTouchEvent" in SOURCE)
    require("EAGX trailer remains optional for vanilla replay",
            'std::memcmp(bytes + size - 4, "EAGX", 4)' in EXTENSION and
            "return false;" in EXTENSION)
    require("EAGX v1 multiplayer type carries metadata and three synchronized lanes",
            "FLAG_MULTIPLAYER_INPUT" in EXTENSION and
            "MultiplayerInputSample" in EXTENSION and
            "MP_PLAYER_COUNT_OFFSET" in EXTENSION and
            "GetMultiplayerPlaybackConfig" in EXTENSION)


if __name__ == "__main__":
    main()

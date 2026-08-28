#!/usr/bin/env python3
"""Focused contracts for the sbrik Player/resources feature-rebase class."""

from __future__ import annotations

import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
PLAYER = (ROOT / "src" / "Player.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "src" / "Player.hpp").read_text(encoding="utf-8")
RESOURCES = (ROOT / "src" / "MultiplayerResources.cpp").read_text(encoding="utf-8")
MULTIPLAYER = (ROOT / "src" / "Multiplayer.hpp").read_text(encoding="utf-8")
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


def hunk_count(path: str) -> int:
    diff = git("diff", "--unified=0", f"{BASE}..{FINAL}", "--", path)
    return len(re.findall(r"^@@ ", diff, flags=re.MULTILINE))


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
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def select_life_receiver(candidates: list[tuple[int, bool, int, float]]) -> int | None:
    # tuple: slot, is_spirit, lives, squared distance
    eligible = [candidate for candidate in candidates if candidate[3] <= 400.0]
    if not eligible:
        return None
    return min(eligible, key=lambda item: (not item[1], item[2], item[0]))[0]


def select_power_receiver(candidates: list[tuple[int, int, float]]) -> int | None:
    # tuple: slot, power, squared distance
    eligible = [candidate for candidate in candidates if candidate[1] < 128 and candidate[2] <= 400.0]
    return min(eligible, key=lambda item: (item[1], item[0]))[0] if eligible else None


def overlap_alpha(distance: float, remote: bool) -> int:
    if not remote or distance >= 260.0:
        return 255
    distance = max(distance, 220.0)
    progress = (distance - 220.0) / 40.0
    progress = progress * progress * (3.0 - 2.0 * progress)
    return int(progress * (255 - 8)) + 8


def main() -> None:
    require("frozen Player.cpp has 126 hunks", hunk_count("src/th07/Player.cpp") == 126)
    require("frozen Player.hpp has 4 hunks", hunk_count("src/th07/Player.hpp") == 4)

    upstream_resources = git("show", f"{FINAL}:src/th07/MultiplayerResources.cpp")
    adapted_resources = upstream_resources.replace("ShowFullPowerMode", "ShowStatusPopup")
    require(
        "MultiplayerResources matches final upstream except Eagler GUI API",
        adapted_resources == RESOURCES.replace("\r\n", "\n"),
    )
    require("multiplayer slot count is three", "TH07_MULTI_MAX_PLAYERS = 3" in MULTIPLAYER)
    require("multiplayer guest count derives from slots", "TH07_MULTI_MAX_GUESTS = TH07_MULTI_MAX_PLAYERS - 1" in MULTIPLAYER)

    header_contracts = {
        "Spirit and Eliminated states": "PLAYER_STATE_SPIRIT = 5,\n    PLAYER_STATE_ELIMINATED = 6",
        "life transfer reuses fixed-layout words": "i32 lifeGiveTargetToken;",
        "three-player storage": "extern Player g_Players[TH07_MULTI_MAX_PLAYERS];",
        "eight-tap power transfer": "POWER_GIVE_TAPS_REQUIRED = 8",
        "twenty-power transfer": "POWER_GIVE_AMOUNT = 20",
        "presentation offset API": "GetPlayerPresentationOffset(u8 playerId)",
    }
    for name, fragment in header_contracts.items():
        require(name, fragment in HEADER)

    source_contracts = {
        "independent SHT power": "? GetPlayerPower(player->initParam)\n                                 : (i32)g_GameManager.globals->currentPower",
        "missile ANM normalization": "missileAnmIdx -= player->initParam == 0",
        "three logical input lanes": "IS_PRESSED_PLAYER(this, TH_BUTTON_UP)",
        "raw touch is local-slot gated": "CanSampleRawTouchForPlayer(this) &&\n               Touch::GetFreeJoystickVector",
        "direct touch is local-slot gated": "CanSampleRawTouchForPlayer(this) &&\n                  Touch::GetPlayerDelta",
        "deathbomb touch is lane-aware": "PlayerUsedTouchToBomb(this)",
        "fixed player identity is repaired": "arg->initParam = static_cast<u8>(expectedPlayerId);",
        "shared Border has one owner": "GetSharedBorderOwner()",
        "bomb damage has 3P scaling": "GetMultiplayerBombDamageMultiplier()",
        "Spirit starts with exactly three bombs": "SetPlayerBombs(player->initParam, 3);",
        "loadout chooses player character": "MultiplayerGameplay::GetPlayerCharacter(arg->initParam)",
        "slot-specific Bomb callback": "g_BombData[loadoutIndex].calc",
        "slot-specific ANM file": "playerAnmFile = ANM_FILE_PLAYER3",
        "sidecar reset occurs after SHT load": "ResetMultiplayerPlayerResources(playerId);",
        "same-character tint is drawn": "MultiplayerGameplay::ShouldTintPlayer(arg->initParam)",
        "proximity fade is centered on the local player": "Player *localPlayer = &g_Players[localPlayerId];",
        "proximity fade follows smoothed presentation position": "g_RemotePresentation[player->initParam].position",
        "option sprites share teammate proximity fade": "ApplyPlayerProximityAlpha(arg->optionsSprite[0].color.color, arg)",
        "always-visible hitbox shares teammate proximity fade": "const u8 alpha = GetPlayerOverlapAlpha(player);",
        "temporary absence hides options": "!MultiplayerGameplay::IsPlayerTemporarilyAbsent(arg->initParam)",
        "temporary absence cannot make proximity fade more opaque": "const u8 absentAlpha = currentAlpha < 0x50 ? currentAlpha : 0x50;",
        "prompt uses interpolated position": "prevPositionCenter.Lerp(giver->positionCenter, g_RenderAlpha)",
        "prompt uses presentation offset": "GetPlayerPresentationOffset(giver->initParam)",
    }
    for name, fragment in source_contracts.items():
        require(name, fragment in PLAYER)

    on_draw = function_body(PLAYER, "u32 Player::OnDrawHighPrio")
    require(
        "eliminated bullets draw before ship suppression",
        on_draw.index("arg->DrawBullets();") < on_draw.index("arg->playerState == PLAYER_STATE_ELIMINATED"),
    )
    on_update = function_body(PLAYER, "u32 Player::OnUpdate")
    require(
        "identity repair precedes input and rollback-sensitive updates",
        on_update.index("expectedPlayerId") < on_update.index("arg->UpdateBombProjectiles();"),
    )
    life_transfer = function_body(PLAYER, "void UpdateLifeTransfer")
    require(
        "Power tapping cancels life charge before Focus charge",
        life_transfer.index("g_powerGiveTaps") < life_transfer.index("!giver->isFocus"),
    )
    require(
        "Spirit revival preserves Focus and drift speeds",
        "receiver->isFocus" not in life_transfer
        and "receiver->previousHorizontalSpeed" not in life_transfer
        and "receiver->previousVerticalSpeed" not in life_transfer,
    )
    require("native Netplay shell is not included", '#include "Netplay.hpp"' not in PLAYER)
    require(
        "full multiplayer team wipe returns to TH07 retry lifecycle",
        "bool teamWiped = true;" in PLAYER
        and "state != PLAYER_STATE_SPIRIT && state != PLAYER_STATE_ELIMINATED" in PLAYER
        and "g_GameManager.isInRetryMenu = 1;" in PLAYER,
    )

    require(
        "life selection prioritizes Spirit",
        select_life_receiver([(1, False, 0, 100.0), (2, True, 7, 100.0)]) == 2,
    )
    require(
        "life selection then chooses lowest lives",
        select_life_receiver([(1, False, 2, 100.0), (2, False, 1, 100.0)]) == 2,
    )
    require(
        "life selection ties by lower slot",
        select_life_receiver([(2, False, 1, 100.0), (1, False, 1, 100.0)]) == 1,
    )
    require(
        "life selection excludes beyond 20 pixels",
        select_life_receiver([(1, True, 0, 401.0)]) is None,
    )
    require(
        "power selection chooses lowest power then slot",
        select_power_receiver([(2, 40, 100.0), (1, 20, 100.0)]) == 1,
    )
    require(
        "power selection excludes MAX and distant players",
        select_power_receiver([(1, 128, 100.0), (2, 0, 401.0)]) is None,
    )
    require("local overlap cue remains opaque", overlap_alpha(120.0, False) == 255)
    require("remote overlap clamps to alpha 8", overlap_alpha(20.0, True) == 8)
    require("remote player stays at alpha 8 throughout close overlap", overlap_alpha(220.0, True) == 8)
    require("remote outer fade midpoint is smooth", overlap_alpha(240.0, True) == 131)
    require("remote overlap returns opaque at 260 px", overlap_alpha(260.0, True) == 255)
    require("2P Border threshold stays 50000", (50000 if 2 < 3 else 75000) == 50000)
    require("3P Border threshold is 75000", (50000 if 3 < 3 else 75000) == 75000)
    require("3P bomb multiplier is two thirds", abs((2.0 / 3.0) - 0.6666666667) < 1e-9)


if __name__ == "__main__":
    main()

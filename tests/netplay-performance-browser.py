#!/usr/bin/env python3
"""Delayed MP + heterogeneous CPU stress, not a real-phone FPS benchmark.

Uses existing canonical-state telemetry to reject a speedup that desynchronizes
peers. RAF cadence is measured in the game iframe with no GPU readback. Probe
startup/final waiting is excluded. Network and CPU settings are in the report.
PASS means completion and sampled deterministic-state agreement, not a phone
smoothness guarantee. CPU throttling is not a specific mobile chipset model.
"""
from __future__ import annotations

import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time
import urllib.parse

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
RELAY_ROOT = ROOT.parent / "eagler-touhou"


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _format: str, *_args: object) -> None:
        pass


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def distribution(values: list[float]) -> dict:
    if not values:
        raise AssertionError("no in-game timing samples collected")
    ordered = sorted(values)
    return {
        "samples": len(values), "mean_ms": sum(values) / len(values),
        "p50_ms": ordered[len(ordered) // 2],
        "p95_ms": ordered[min(len(ordered) - 1, len(ordered) * 95 // 100)],
        "p99_ms": ordered[min(len(ordered) - 1, len(ordered) * 99 // 100)],
        "max_ms": ordered[-1],
        "over_25ms": sum(value > 25 for value in values),
        "over_50ms": sum(value > 50 for value in values),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--players", type=int, choices=(2, 3), default=2)
    parser.add_argument("--frames", type=int, default=900)
    parser.add_argument("--delay-ms", type=int, default=55)
    parser.add_argument("--jitter-ms", type=int, default=10)
    parser.add_argument("--cpu-rate", type=float, default=1)
    parser.add_argument("--slow-player", type=int, default=1)
    parser.add_argument("--analog", action="store_true")
    parser.add_argument("--input-delay", type=int, choices=range(7), default=3)
    parser.add_argument("--touch-stream", choices=("snapshot", "delta"), default="snapshot")
    parser.add_argument("--physical-touch", action="store_true", help="Inject real browser touch events and verify once-only displacement")
    parser.add_argument("--prediction-window", type=int, choices=range(1, 13), default=12)
    parser.add_argument("--touch-prediction", choices=("legacy", "stable"), default="legacy")
    parser.add_argument("--touch-workload", choices=("default", "steady", "variable", "stop", "reverse", "burst"), default="default")
    parser.add_argument("--no-rollback", action="store_true", help="Pure buffered lockstep: exact remote input only, no rollback snapshots")
    parser.add_argument("--dense-bullets", type=int, default=0, help="Test-only stationary BulletManager load (0..960)")
    parser.add_argument("--require-rollback", action="store_true")
    parser.add_argument("--relay-diagnostics", action="store_true")
    parser.add_argument("--rtc", action="store_true", help="Use real RTC; relay delay does not affect RTC input")
    parser.add_argument("--build", default="build-web-th07-netplay")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.physical_touch and (args.players != 2 or args.analog or args.touch_workload != "default"):
        parser.error("physical-touch fixture requires 2P, default workload and no --analog")
    if (not re.fullmatch(r"[A-Za-z0-9_.-]+", args.build) or args.frames < 600
            or args.frames > 12000 or args.frames % 300 or args.cpu_rate < 1
            or not 0 <= args.slow_player < args.players
            or args.delay_ms < 0 or args.jitter_ms < 0 or args.timeout <= 0
            or args.dense_bullets < 0 or args.dense_bullets > 960
            or (args.no_rollback and args.require_rollback)):
        parser.error("invalid build/timing settings; frames must be 600..12000 in steps of 300")
    build = ROOT / args.build
    if not (build / "th07.wasm").is_file():
        parser.error(f"build missing: {build}")
    if args.analog:
        cache = build / "CMakeCache.txt"
        manifest = build / "netplay-benchmark-build.json"
        dev_tools = ("TH_DEV_TOOLS:BOOL=ON" in cache.read_text() if cache.is_file()
                     else json.loads(manifest.read_text())["dev_tools"])
        if dev_tools:
            parser.error("analog fixture requires TH_DEV_TOOLS=OFF (uses Replay input stimulus only)")

    http_port, relay_port = free_port(), free_port()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("EAGLER_NETPLAY_", "TH07_RELAY_", "TH07_TEST_", "TH07_STUN_", "TH07_TURN_"))}
    env.update({
        "EAGLER_NETPLAY_RELAY_HOST": "127.0.0.1",
        "EAGLER_NETPLAY_RELAY_PORT": str(relay_port),
        "EAGLER_NETPLAY_RELAY_DELAY_MS": str(args.delay_ms),
        "EAGLER_NETPLAY_RELAY_JITTER_MS": str(args.jitter_ms),
        "EAGLER_NETPLAY_STUN_URLS": "",
    })
    handler = functools.partial(QuietHandler, directory=str(ROOT))
    room = f"th07mp-perf-{time.time_ns()}"
    messages: list[list[str]] = [[] for _ in range(args.players)]
    errors: list[list[str]] = [[] for _ in range(args.players)]
    report = {"settings": {key: str(value) if isinstance(value, Path) else value
                            for key, value in vars(args).items()}, "peers": []}
    with http.server.ThreadingHTTPServer(("127.0.0.1", http_port), handler) as server, tempfile.TemporaryFile(mode="w+t") as relay_log:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        relay_command = ["node"]
        if args.relay_diagnostics:
            relay_command += ["--import", (ROOT / "tests/netplay-relay-timing-probe.mjs").as_uri()]
        relay = subprocess.Popen([*relay_command, str(RELAY_ROOT / "server/netplay-relay.mjs")],
                                 cwd=RELAY_ROOT, env=env, stdout=relay_log, stderr=relay_log)
        try:
            deadline = time.monotonic() + 10
            while True:
                if relay.poll() is not None or time.monotonic() >= deadline:
                    relay_log.seek(0)
                    raise RuntimeError("relay startup failed: " + relay_log.read()[-8000:])
                try:
                    with socket.create_connection(("127.0.0.1", relay_port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.1)
            with sync_playwright() as playwright:
                browsers, pages, touch_sessions = [], [], []
                try:
                    for player in range(args.players):
                        browser = playwright.chromium.launch(headless=True, args=[
                            "--disable-background-timer-throttling",
                            "--disable-backgrounding-occluded-windows",
                            "--disable-renderer-backgrounding",
                        ])
                        browsers.append(browser)
                        context = browser.new_context(viewport={"width": 960, "height": 720})
                        if not args.rtc:
                            context.add_init_script("delete globalThis.RTCPeerConnection")
                        context.add_init_script("""(() => {
                          if (window.parent === window) return;
                          const state = globalThis.__th07Perf = {
                            gaps: [], advanceGaps: [], firstTime: 0, lastTime: 0,
                            firstFrame: 0, lastFrame: 0
                          };
                          let last = 0, lastAdvance = 0, previousFrame = -1;
                          function measure(now) {
                            const frame = Number(globalThis.__eaglerNetplayLanFrame || 0);
                            const active = frame >= 120 && frame < FRAME_LIMIT;
                            if (active) {
                              if (!state.firstTime) { state.firstTime = now; state.firstFrame = frame; }
                              if (last) state.gaps.push(now - last);
                              if (frame !== previousFrame) {
                                if (lastAdvance) state.advanceGaps.push(now - lastAdvance);
                                lastAdvance = now;
                              }
                              state.lastTime = now; state.lastFrame = frame; last = now;
                            } else { last = 0; lastAdvance = 0; }
                            previousFrame = frame;
                            requestAnimationFrame(measure);
                          }
                          requestAnimationFrame(measure);
                        })();""".replace("FRAME_LIMIT", str(args.frames)))
                        page = context.new_page()
                        if args.physical_touch:
                            touch_session = context.new_cdp_session(page)
                            touch_session.send("Emulation.setTouchEmulationEnabled", {"enabled": True, "maxTouchPoints": 1})
                            touch_sessions.append(touch_session)
                        page.on("console", lambda message, i=player: messages[i].append(message.text))
                        page.on("pageerror", lambda error, i=player: errors[i].append(str(error)))
                        if player == args.slow_player and args.cpu_rate != 1:
                            context.new_cdp_session(page).send("Emulation.setCPUThrottlingRate", {"rate": args.cpu_rate})
                        query = urllib.parse.urlencode({
                            "player": player, "players": args.players, "frames": args.frames,
                            "relay": f"ws://127.0.0.1:{relay_port}/?room={room}&player={player}",
                            "build": args.build, "analog": int(args.analog),
                            "inputDelay": args.input_delay, "prediction": args.touch_prediction,
                            "predictionWindow": args.prediction_window,
                            "trace": args.touch_workload,
                            "noRollback": int(args.no_rollback),
                            "denseBullets": args.dense_bullets,
                            "touchStream": args.touch_stream,
                            "physicalTouch": int(args.physical_touch),
                        })
                        page.goto(f"http://127.0.0.1:{http_port}/tests/netplay-performance-browser-host.html?{query}",
                                  wait_until="load", timeout=60000)
                        pages.append(page)
                    deadline = time.monotonic() + args.timeout
                    previous_frames = [-1] * args.players
                    progress_times = [time.monotonic()] * args.players
                    stalls = report.setdefault("stalls", [])
                    touch_started = touch_checked = False
                    initial_positions = []
                    while time.monotonic() < deadline:
                        states = [page.evaluate("""() => {
                          const runtime = document.getElementById('runtime').contentWindow;
                          return {done: !!runtime.__eaglerNetplayLanStageDone,
                            frame: runtime.__eaglerNetplayLanFrame || 0,
                            failed: !!runtime.__eaglerNetplayFailed,
                            error: globalThis.__th07PerfFailure || runtime.__eaglerNetplayError || '',
                            transport: runtime.__eaglerNetplayTransport,
                            queued: runtime.__th07PeerTransport?.received?.length,
                            queueHead: runtime.__th07PeerTransport?.receivedHead,
                            confirmed: runtime.__eaglerNetplayLanConfirmed,
                            pause: runtime.__eaglerNetplayPauseState,
                            inputReceived: runtime.__th07PeerTransport?.perf?.received,
                            inputSent: runtime.__th07PeerTransport?.perf?.sent,
                            bufferedBytes: runtime.__th07PeerTransport?.relay?.bufferedAmount,
                            lastWireFrame: runtime.__th07PeerTransport?.perf?.lastInputFrame,
                            lastEventAge: runtime.__th07PeerTransport?.perf?.lastEventMs
                              ? runtime.performance.now() - runtime.__th07PeerTransport.perf.lastEventMs : 0,
                            socket: runtime.__th07PeerTransport?.relay?.readyState};
                        }""") for page in pages]
                        if any(state["failed"] or state["error"] for state in states) or any(errors):
                            raise AssertionError(f"runtime failure: {states}; errors={errors}")
                        fatals = [line for lines in messages for line in lines
                                  if "chain result=-1" in line or "netplay lan stage: FAIL" in line]
                        if fatals:
                            raise AssertionError(f"runtime exited before completion: {fatals[-5:]}")
                        if all(state["done"] for state in states):
                            break
                        if args.physical_touch and not touch_started and all(state["frame"] >= 130 for state in states):
                            initial_positions = [page.evaluate("document.getElementById('runtime').contentWindow.__eaglerNetplayPerf.positions") for page in pages]
                            for i, touch_session in enumerate(touch_sessions):
                                point = {"x": 308, "y": 388, "id": 1}
                                touch_session.send("Input.dispatchTouchEvent", {"type": "touchStart", "touchPoints": [point]})
                                point["x"] += 12 if i == 0 else -14
                                touch_session.send("Input.dispatchTouchEvent", {"type": "touchMove", "touchPoints": [point]})
                            touch_started = True
                        if args.physical_touch and touch_started and not touch_checked and all(state["frame"] >= 230 for state in states):
                            observed = [page.evaluate("""() => {
                              const p = document.getElementById('runtime').contentWindow.__eaglerNetplayPerf;
                              return {positions:p.positions, x:p.capturedTouchX, y:p.capturedTouchY};
                            }""") for page in pages]
                            for peer, sample in enumerate(observed):
                                for slot in range(2):
                                    delta = sample["positions"][slot * 2] - initial_positions[peer][slot * 2]
                                    expected = observed[slot]["x"]
                                    if abs(expected) < 1 or abs(delta - expected) > 0.001:
                                        raise AssertionError(f"physical touch duplicated/lost: peer={peer} slot={slot} delta={delta} expected={expected} observed={observed}")
                            report["physical_touch"] = {"initial_positions": initial_positions, "observed": observed}
                            for touch_session in touch_sessions:
                                touch_session.send("Input.dispatchTouchEvent", {"type":"touchEnd", "touchPoints":[]})
                            touch_checked = True
                        now = time.monotonic()
                        for i, state in enumerate(states):
                            if state["frame"] != previous_frames[i]:
                                progress_times[i] = now
                                previous_frames[i] = state["frame"]
                            elif state["frame"] > 0 and not state["done"] and now - progress_times[i] > 0.4:
                                if len(stalls) < 150:
                                    stalls.append({"peer": i, "no_advance_ms": (now - progress_times[i]) * 1000, **state})
                        pages[0].wait_for_timeout(200)
                    else:
                        raise AssertionError(f"gameplay timeout: {states}")
                    hashes = []
                    if args.physical_touch and not touch_checked:
                        raise AssertionError("physical touch stimulus did not complete")
                    report["stalls"] = stalls
                    for player, page in enumerate(pages):
                        data = page.evaluate("""() => {
                          const w = document.getElementById('runtime').contentWindow;
                          return {timing: w.__th07Perf, hashes: w.__eaglerNetplayLanHashes,
                            canonical: w.__eaglerNetplayLanCanonical,
                            canonical_meta: w.__eaglerNetplayLanCanonicalMeta,
                            canonical_players: w.__eaglerNetplayLanCanonicalPlayers,
                            costs: w.__eaglerNetplayPerf,
                            input_delay: w.__eaglerNetplayInputDelayFrames,
                            prediction_window: w.__eaglerNetplayMaxPredictionFrames,
                            no_rollback: !!w.__eaglerNetplayNoRollback,
                            dense_bullets: w.__eaglerNetplayDenseBullets || 0,
                            queue_stats: w.__th07PeerTransport?.perf || null,
                            transport: w.__eaglerNetplayTransport,
                            rollback: w.__eaglerNetplayLanRollback, resimulated: w.__eaglerNetplayLanResimulated,
                            confirmed: w.__eaglerNetplayLanConfirmed};
                        }""")
                        hashes.append(data["hashes"])
                        timing = data.pop("timing")
                        for key in ("drawMs", "driverMs", "presentGapMs"):
                            samples = data.get("costs", {}).pop(key, [])
                            if samples:
                                data[key] = distribution(samples)
                        data["raf"] = distribution(timing["gaps"])
                        data["visible_advance"] = distribution(timing["advanceGaps"])
                        data["simulation_fps"] = 1000 * (timing["lastFrame"] - timing["firstFrame"]) / (timing["lastTime"] - timing["firstTime"])
                        data["pass_log"] = next((line for line in messages[player] if "netplay lan stage: PASS" in line), "")
                        if not data["pass_log"] or data["confirmed"] < args.frames - 1:
                            raise AssertionError(f"unconfirmed/incomplete run: {data}")
                        report["peers"].append(data)
                    for frame in range(300, args.frames + 1, 300):
                        values = [entry.get(str(frame)) for entry in hashes]
                        if None in values or len(set(values)) != 1:
                            raise AssertionError(f"canonical divergence at frame {frame}: {values}")
                    if args.require_rollback and not any(peer["rollback"] for peer in report["peers"]):
                        raise AssertionError("workload did not exercise rollback")
                    expected_route = "rtc" if args.rtc else "relay"
                    if any(peer.get("transport") != expected_route for peer in report["peers"]):
                        raise AssertionError("requested transport was not used")
                    if any(peer.get("input_delay") != args.input_delay for peer in report["peers"]):
                        raise AssertionError("requested input delay was not applied")
                    if any(peer.get("prediction_window") != args.prediction_window for peer in report["peers"]):
                        raise AssertionError("requested prediction window was not applied")
                    if any(bool(peer.get("no_rollback")) != args.no_rollback for peer in report["peers"]):
                        raise AssertionError("requested rollback mode was not applied")
                    if any(peer.get("dense_bullets") != args.dense_bullets for peer in report["peers"]):
                        raise AssertionError("requested dense bullet load was not applied")
                    if any(not peer.get("costs", {}).get("captures") for peer in report["peers"]):
                        raise AssertionError("cost instrumentation was not active")
                    report["status"] = "PASS"
                finally:
                    for browser in browsers:
                        browser.close()
        except Exception as error:
            report["status"] = "FAIL"
            report["error"] = str(error)
            report["console_tails"] = [lines[-35:] for lines in messages]
        finally:
            relay.terminate()
            try:
                relay.wait(timeout=5)
            except subprocess.TimeoutExpired:
                relay.kill()
                relay.wait(timeout=5)
            relay_log.seek(0)
            report["relay_log"] = relay_log.read()[-8000:]
            server.shutdown()
            thread.join(timeout=5)
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text, flush=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    return 0 if report.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Run pure netplay behavior tests natively and with WASM SAFE_HEAP."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "touch-pipeline": ["tests/netplay-touch-pipeline-test.cpp", "src/netplay/NetplayCore.cpp",
                       "src/netplay/NetplayProtocol.cpp", "src/netplay/NetplayInput.cpp",
                       "src/netplay/RollbackJournal.cpp"],
    "delay-prediction": ["tests/netplay-delay-prediction-test.cpp", "src/netplay/NetplayCore.cpp",
                         "src/netplay/NetplayProtocol.cpp"],
    "rollback-journal": ["tests/rollback-journal-test.cpp", "src/netplay/RollbackJournal.cpp"],
    "frame-budget": ["tests/netplay-frame-budget-test.cpp"],
    "frame-advantage": ["tests/netplay-frame-advantage-test.cpp"],
    "netplay-core": ["tests/netplay-core-test.cpp", "src/netplay/NetplayCore.cpp",
                     "src/netplay/NetplayProtocol.cpp", "src/netplay/NetplaySession.cpp"],
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", choices=("native", "wasm", "both"), default="both")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = {"status": "PASS", "tests": []}
    env = os.environ.copy()
    emsdk = ROOT.parent / "toolchains/emsdk"
    env["EM_CONFIG"] = str(emsdk / ".emscripten")
    empp = str(emsdk / "upstream/emscripten" / ("em++.exe" if os.name == "nt" else "em++"))
    try:
        with tempfile.TemporaryDirectory(prefix="th07-netplay-tests-") as directory:
            for toolchain in (("native", "wasm") if args.toolchain == "both" else (args.toolchain,)):
                for name, sources in CASES.items():
                    executable = Path(directory) / (name + (".cjs" if toolchain == "wasm" else ".exe"))
                    command = [empp if toolchain == "wasm" else "g++", "-std=c++17", "-O2", "-UNDEBUG",
                               "-Wall", "-Wextra", "-Isrc", *sources, "-o", str(executable)]
                    if toolchain == "wasm":
                        # The standalone core test places several complete
                        # input histories on its stack (production uses globals).
                        command += ["-sASSERTIONS=2", "-sSAFE_HEAP=1", "-sSTACK_SIZE=1048576",
                                    "-sALLOW_MEMORY_GROWTH=1", "-sENVIRONMENT=node"]
                    subprocess.run(command, cwd=ROOT, env=env, check=True, timeout=120)
                    run = ["node", str(executable)] if toolchain == "wasm" else [str(executable)]
                    text = subprocess.check_output(run, cwd=ROOT, text=True, timeout=90).strip()
                    report["tests"].append({"toolchain": toolchain, "name": name, "result": text})
                    print(f"{toolchain}/{name}: {text}", flush=True)
    except (subprocess.SubprocessError, OSError) as error:
        report["status"] = "FAIL"
        report["error"] = str(error)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

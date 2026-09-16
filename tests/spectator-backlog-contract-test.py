from pathlib import Path


WORKSPACE = Path(__file__).resolve().parents[2]
RELAY = (WORKSPACE / "eagler-touhou/server/netplay-relay.mjs").read_text(encoding="utf-8")
TH06 = (WORKSPACE / "th06-eagler/src/netplay/BrowserPeerTransport.cpp").read_text(encoding="utf-8")
TH07 = (WORKSPACE / "th07-eagler/src/netplay/BrowserPeerTransport.cpp").read_text(encoding="utf-8")


def require(ok: bool, label: str) -> None:
    if not ok:
        raise AssertionError(label)


require("TH07_SPECTATOR_CONNECT_GRACE_MS" in RELAY, "spectator join window is configurable")
require("TH07_SPECTATOR_MAX_BUFFERED_BYTES" in RELAY, "relay spectator backlog cap is configurable")
require("socket.bufferedAmount" in RELAY and "spectator fell too far behind" in RELAY,
        "relay closes a spectator whose socket backlog grows without bound")
require("spectatorAdmissionOpen" in RELAY and "SPECTATOR WINDOW CLOSE" in RELAY,
        "relay keeps an explicit finite late-join window")
require("if (run.spectatorAdmissionOpen)" in RELAY and "run.spectatorHistory.push(payload)" in RELAY,
        "frame-zero history is retained throughout the late-join window")
require("run.spectatorHistory.length = 0" in RELAY,
        "frame-zero history is released when the window closes")

for name, source in (("TH06", TH06), ("TH07", TH07)):
    require("maxReceivedPackets: 16384" in source, f"{name} spectator receive queue cap")
    require("this.received.length - this.receivedHead >= this.maxReceivedPackets" in source,
            f"{name} spectator queue checks unconsumed packets")
    require("Spectator fell too far behind" in source and
            "close(1008, 'spectator fell too far behind')" in source,
            f"{name} slow spectator fails instead of accumulating forever")
    send = source[source.index("peer_send_spectator"):source.index("peer_has_spectators")]
    require("spectatorCount <= 0" not in send,
            f"{name} P1 can publish frame-zero history before a late spectator volunteers")

print("Spectator backlog contract: PASS finite-late-join-window=1")

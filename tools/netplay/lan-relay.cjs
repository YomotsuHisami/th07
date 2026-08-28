const { WebSocketServer, WebSocket } = require('ws');
const { createHmac } = require('node:crypto');

const host = process.env.TH07_RELAY_HOST || '0.0.0.0';
const port = Number.parseInt(process.env.TH07_RELAY_PORT || '18142', 10);
const delayMs = Math.max(0, Number.parseInt(process.env.TH07_RELAY_DELAY_MS || '0', 10) || 0);
const jitterMs = Math.max(0, Number.parseInt(process.env.TH07_RELAY_JITTER_MS || '0', 10) || 0);
const dropEvery = Math.max(0, Number.parseInt(process.env.TH07_RELAY_DROP_EVERY || '0', 10) || 0);
const rtcTimeoutMs = Math.max(1000, Number.parseInt(
  process.env.TH07_RTC_TIMEOUT_MS || process.env.TH07_DIRECT_TIMEOUT_MS || '4500', 10
) || 4500);
const stunUrls = (process.env.TH07_STUN_URLS || 'stun:stun.cloudflare.com:3478')
  .split(',').map(value => value.trim()).filter(Boolean);
const turnUrls = (process.env.TH07_TURN_URLS || '')
  .split(',').map(value => value.trim()).filter(Boolean);
const turnSharedSecret = process.env.TH07_TURN_SHARED_SECRET || '';
const turnUsername = process.env.TH07_TURN_USERNAME || '';
const turnCredential = process.env.TH07_TURN_CREDENTIAL || '';
const turnTtlSeconds = Math.max(60, Number.parseInt(process.env.TH07_TURN_TTL_SECONDS || '3600', 10) || 3600);
const lobbyReconnectGraceMs = Math.max(100, Number.parseInt(process.env.TH07_LOBBY_RECONNECT_GRACE_MS || '12000', 10) || 12000);

for (const url of stunUrls) {
  if (!/^stuns?:/i.test(url)) throw new Error(`invalid TH07_STUN_URLS entry: ${url}`);
}
for (const url of turnUrls) {
  if (!/^turns?:/i.test(url)) throw new Error(`invalid TH07_TURN_URLS entry: ${url}`);
}
if (turnUrls.length && !turnSharedSecret && !(turnUsername && turnCredential)) {
  throw new Error('TH07_TURN_URLS requires TH07_TURN_SHARED_SECRET or both static TURN credentials');
}
if ((turnUsername && !turnCredential) || (!turnUsername && turnCredential)) {
  throw new Error('TH07_TURN_USERNAME and TH07_TURN_CREDENTIAL must be configured together');
}
const rooms = new Map();
const forwardCounters = new Map();
const targetedEnvelopeMarker = 0xe7;

function iceServersFor(roomId, runId, player) {
  const servers = [];
  if (stunUrls.length) servers.push({ urls: stunUrls });
  if (!turnUrls.length) return servers;
  if (turnSharedSecret) {
    // coturn TURN REST API style ephemeral credentials. The shared secret
    // stays server-side; only the short-lived username/credential is sent to
    // the browser during signaling.
    const expires = Math.floor(Date.now() / 1000) + turnTtlSeconds;
    const username = `${expires}:${roomId}-${runId}-p${player}`;
    const credential = createHmac('sha1', turnSharedSecret).update(username).digest('base64');
    servers.push({ urls: turnUrls, username, credential });
  } else if (turnUsername && turnCredential) {
    // Static credentials are supported for local testing only; production
    // deployments should prefer TH07_TURN_SHARED_SECRET.
    servers.push({ urls: turnUrls, username: turnUsername, credential: turnCredential });
  }
  return servers;
}

function getRoom(id) {
  let room = rooms.get(id);
  if (!room) {
    room = {
      clients: new Map(),
      lobbyClients: new Map(),
      lobbyDisconnectTimers: new Map(),
      runs: new Map(),
      lobby: {
        playerCount: 2,
        difficulty: 1,
        seats: [null, null, null],
        startSerial: 0,
      },
    };
    rooms.set(id, room);
  }
  return room;
}

function getRun(room, runId) {
  let run = room.runs.get(runId);
  if (!run) {
    run = {
      clients: new Map(),
      signalClients: new Map(),
      rtcReady: new Set(),
      rtcFailed: false,
      playerCount: 0,
      route: null,
      routeTimer: null,
    };
    room.runs.set(runId, run);
  }
  return run;
}

function maybeDeleteRun(room, runId, run) {
  if (run.clients.size !== 0 || run.signalClients.size !== 0) return;
  if (run.routeTimer) clearTimeout(run.routeTimer);
  room.runs.delete(runId);
}

function maybeDeleteRoom(roomId, room) {
  if (room.clients.size === 0 && room.lobbyClients.size === 0 &&
      room.lobbyDisconnectTimers.size === 0 && room.runs.size === 0)
    rooms.delete(roomId);
}

function removeClient(roomId, runId, player, socket) {
  const room = rooms.get(roomId);
  if (!room) return;
  const run = getRun(room, runId);
  if (run.clients.get(player) === socket)
    run.clients.delete(player);
  maybeDeleteRun(room, runId, run);
  maybeDeleteRoom(roomId, room);
}

function sendSignal(socket, payload) {
  if (socket.readyState === WebSocket.OPEN)
    socket.send(JSON.stringify(payload));
}

function broadcastSignal(run, payload) {
  const message = JSON.stringify(payload);
  for (const socket of run.signalClients.values())
    if (socket.readyState === WebSocket.OPEN) socket.send(message);
}

function broadcastRoute(run, payload) {
  broadcastSignal(run, payload);
  const message = JSON.stringify(payload);
  for (const socket of run.clients.values())
    if (socket.readyState === WebSocket.OPEN) socket.send(message);
}

function chooseRoute(roomId, runId, room, run, route) {
  if (run.route) return;
  run.route = route;
  if (run.routeTimer) {
    clearTimeout(run.routeTimer);
    run.routeTimer = null;
  }
  console.log(`ROUTE room=${roomId} run=${runId} mode=${route}`);
  // Signaling is preferred, but a browser/network can leave one signaling
  // WebSocket stuck in CONNECTING while both gameplay relay sockets are live.
  // Deliver the barrier on both channels so fallback never depends on the
  // unhealthy signaling path it is meant to recover from.
  broadcastRoute(run, { type: 'route', mode: route });
}

function maybeStartRouteTimer(roomId, runId, room, run) {
  const allSignalsPresent = run.signalClients.size >= run.playerCount;
  const allRelaysPresent = run.clients.size >= run.playerCount;
  if (run.route || run.routeTimer || run.playerCount < 2 || (!allSignalsPresent && !allRelaysPresent)) return;
  run.routeTimer = setTimeout(() => {
    run.routeTimer = null;
    run.rtcFailed = true;
    maybeResolveRoute(roomId, runId, room, run);
  }, rtcTimeoutMs);
}

function maybeResolveRoute(roomId, runId, room, run) {
  if (run.route || run.playerCount < 2) return;
  if (run.rtcFailed) {
    // Relay release is a barrier too: do not let the first endpoint begin
    // frame-zero traffic until every expected relay socket and signaling
    // client is already present. Route delivery itself is asynchronous, so
    // clients also buffer relay packets during that tiny propagation window.
    if (run.clients.size === run.playerCount)
      chooseRoute(roomId, runId, room, run, 'relay');
    return;
  }
  if (run.signalClients.size === run.playerCount && run.rtcReady.size === run.playerCount) {
    chooseRoute(roomId, runId, room, run, 'rtc');
    return;
  }
  maybeStartRouteTimer(roomId, runId, room, run);
}

function handleSignalConnection(socket, roomId, runId, player, playerCount) {
  const room = getRoom(roomId);
  const run = getRun(room, runId);
  if (run.playerCount && run.playerCount !== playerCount) {
    socket.close(1008, 'player count mismatch');
    return;
  }
  run.playerCount = playerCount;
  const previous = run.signalClients.get(player);
  if (previous && previous !== socket && previous.readyState === WebSocket.OPEN)
    previous.close(1000, 'signaling reconnected');
  const existingPeers = [...run.signalClients.keys()].filter(peer => peer !== player);
  run.signalClients.set(player, socket);
  sendSignal(socket, {
    type: 'peers', peers: existingPeers, route: run.route,
    iceServers: iceServersFor(roomId, runId, player),
  });
  for (const [peer, target] of run.signalClients) {
    if (peer !== player) sendSignal(target, { type: 'peer-join', player });
  }
  console.log(`SIGNAL JOIN room=${roomId} run=${runId} player=${player} peers=${run.signalClients.size}`);
  maybeResolveRoute(roomId, runId, room, run);

  socket.on('message', (data, isBinary) => {
    if (isBinary) { socket.close(1003, 'signaling expects text'); return; }
    let message;
    try { message = JSON.parse(String(data)); }
    catch { sendSignal(socket, { type: 'error', error: 'invalid signaling message' }); return; }
    if (message.type === 'signal') {
      const to = Number(message.to);
      if (!Number.isInteger(to) || to < 0 || to >= playerCount || to === player) return;
      const target = run.signalClients.get(to);
      if (!target) return;
      sendSignal(target, {
        type: 'signal', from: player,
        description: message.description || null,
        candidate: message.candidate || null,
      });
      return;
    }
    if (message.type === 'rtc-ready') {
      run.rtcReady.add(player);
      maybeResolveRoute(roomId, runId, room, run);
      return;
    }
    if (message.type === 'ice-restart-request' && run.route === 'rtc') {
      const to = Number(message.to);
      if (!Number.isInteger(to) || to < 0 || to >= playerCount || to === player) return;
      const target = run.signalClients.get(to);
      if (target) sendSignal(target, { type: 'ice-restart-request', from: player });
      return;
    }
    if (message.type === 'rtc-failed') {
      run.rtcFailed = true;
      maybeResolveRoute(roomId, runId, room, run);
      return;
    }
  });

  socket.on('close', () => {
    if (run.signalClients.get(player) === socket) {
      run.signalClients.delete(player);
      run.rtcReady.delete(player);
      if (!run.route) {
        run.rtcFailed = true;
        maybeResolveRoute(roomId, runId, room, run);
      }
    }
    console.log(`SIGNAL LEAVE room=${roomId} run=${runId} player=${player}`);
    maybeDeleteRun(room, runId, run);
    maybeDeleteRoom(roomId, room);
  });
}

function lobbySnapshot(room) {
  return {
    playerCount: room.lobby.playerCount,
    difficulty: room.lobby.difficulty,
    startSerial: room.lobby.startSerial,
    seats: room.lobby.seats.map(seat => seat ? {
      clientId: seat.clientId,
      loadout: seat.loadout,
      ready: !!seat.ready,
      offline: !room.lobbyClients.has(seat.clientId),
    } : null),
  };
}

function sendLobby(socket, payload) {
  if (socket.readyState === WebSocket.OPEN)
    socket.send(JSON.stringify(payload));
}

function broadcastLobby(room, payload = null) {
  const message = JSON.stringify(payload || { type: 'state', room: lobbySnapshot(room) });
  for (const socket of room.lobbyClients.values())
    if (socket.readyState === WebSocket.OPEN) socket.send(message);
}

function clearLobbySeat(room, clientId) {
  let changed = false;
  for (let index = 0; index < room.lobby.seats.length; index++) {
    if (room.lobby.seats[index]?.clientId !== clientId) continue;
    room.lobby.seats[index] = null;
    changed = true;
  }
  return changed;
}

function lobbySeatOf(room, clientId) {
  return room.lobby.seats.findIndex(seat => seat?.clientId === clientId);
}

function validLoadout(value) {
  return Number.isInteger(value) && value >= 0 && value < 6;
}

function handleLobbyConnection(socket, roomId, clientId) {
  const room = getRoom(roomId);
  const pendingDisconnect = room.lobbyDisconnectTimers.get(clientId);
  if (pendingDisconnect) {
    clearTimeout(pendingDisconnect);
    room.lobbyDisconnectTimers.delete(clientId);
  }
  const previous = room.lobbyClients.get(clientId);
  if (previous && previous !== socket && previous.readyState === WebSocket.OPEN)
    previous.close(1000, 'lobby reconnected');
  room.lobbyClients.set(clientId, socket);
  console.log(`LOBBY JOIN room=${roomId} client=${clientId} peers=${room.lobbyClients.size}`);
  sendLobby(socket, { type: 'state', room: lobbySnapshot(room) });
  if (pendingDisconnect) broadcastLobby(room);

  socket.on('message', (data, isBinary) => {
    if (isBinary) {
      socket.close(1003, 'lobby expects text frames');
      return;
    }
    let message;
    try { message = JSON.parse(String(data)); }
    catch { sendLobby(socket, { type: 'error', error: 'invalid lobby message' }); return; }

    if (message.type === 'take-seat') {
      const seat = Number(message.seat);
      if (!Number.isInteger(seat) || seat < 0 || seat >= room.lobby.playerCount || !validLoadout(Number(message.loadout))) {
        sendLobby(socket, { type: 'error', error: 'invalid seat or loadout' });
        return;
      }
      const occupant = room.lobby.seats[seat];
      if (occupant && occupant.clientId !== clientId) {
        sendLobby(socket, { type: 'error', error: `P${seat + 1} 已被占用` });
        sendLobby(socket, { type: 'state', room: lobbySnapshot(room) });
        return;
      }
      clearLobbySeat(room, clientId);
      room.lobby.seats[seat] = { clientId, loadout: Number(message.loadout), ready: !!message.ready };
      broadcastLobby(room);
      return;
    }

    if (message.type === 'stand-up') {
      if (clearLobbySeat(room, clientId)) broadcastLobby(room);
      return;
    }

    const seat = lobbySeatOf(room, clientId);
    if (seat < 0) {
      sendLobby(socket, { type: 'error', error: '请先选择 P 位' });
      return;
    }
    const occupant = room.lobby.seats[seat];

    if (message.type === 'set-loadout') {
      const loadout = Number(message.loadout);
      if (!validLoadout(loadout)) { sendLobby(socket, { type: 'error', error: 'invalid loadout' }); return; }
      occupant.loadout = loadout;
      broadcastLobby(room);
      return;
    }
    if (message.type === 'set-ready') {
      occupant.ready = !!message.ready;
      broadcastLobby(room);
      return;
    }
    if (message.type === 'settings') {
      if (seat !== 0) { sendLobby(socket, { type: 'error', error: '只有 P1 可以修改房间设置' }); return; }
      const playerCount = Number(message.playerCount) === 3 ? 3 : 2;
      const difficulty = Math.max(0, Math.min(5, Number(message.difficulty) || 0));
      room.lobby.playerCount = playerCount;
      room.lobby.difficulty = difficulty;
      for (let index = playerCount; index < room.lobby.seats.length; index++) room.lobby.seats[index] = null;
      broadcastLobby(room);
      return;
    }
    if (message.type === 'start') {
      if (seat !== 0) { sendLobby(socket, { type: 'error', error: '只有 P1 可以开始游戏' }); return; }
      const activeSeats = room.lobby.seats.slice(0, room.lobby.playerCount);
      if (activeSeats.some(entry => !entry || !entry.ready)) {
        sendLobby(socket, { type: 'error', error: '仍有玩家未入座或未准备' });
        return;
      }
      room.lobby.startSerial++;
      broadcastLobby(room, { type: 'start', serial: room.lobby.startSerial, room: lobbySnapshot(room) });
      return;
    }
    sendLobby(socket, { type: 'error', error: 'unknown lobby message' });
  });

  socket.on('close', (code, reason) => {
    if (room.lobbyClients.get(clientId) === socket) {
      room.lobbyClients.delete(clientId);
      const deliberateLeave = code === 1000 && String(reason || '') === 'leave room';
      if (deliberateLeave) {
        if (clearLobbySeat(room, clientId)) broadcastLobby(room);
      } else if (lobbySeatOf(room, clientId) >= 0) {
        const timer = setTimeout(() => {
          if (room.lobbyDisconnectTimers.get(clientId) !== timer) return;
          room.lobbyDisconnectTimers.delete(clientId);
          if (!room.lobbyClients.has(clientId) && clearLobbySeat(room, clientId)) broadcastLobby(room);
          maybeDeleteRoom(roomId, room);
        }, lobbyReconnectGraceMs);
        room.lobbyDisconnectTimers.set(clientId, timer);
        // Keep the seat visible during a short network transition. Clients can
        // show it as reconnecting instead of presenting an empty room.
        broadcastLobby(room);
      }
    }
    console.log(`LOBBY LEAVE room=${roomId} client=${clientId}`);
    maybeDeleteRoom(roomId, room);
  });
  socket.on('error', error => {
    console.error(`LOBBY SOCKET room=${roomId} client=${clientId} ${error.message}`);
  });
}

const server = new WebSocketServer({ host, port, perMessageDeflate: false });

server.on('connection', (socket, request) => {
  const url = new URL(request.url || '/', `ws://${request.headers.host || 'localhost'}`);
  const roomId = url.searchParams.get('room') || '';
  const runId = url.searchParams.get('run') || '0';
  const lobbyClient = url.searchParams.get('lobby') || '';
  if (/^[A-Za-z0-9_-]{1,64}$/.test(roomId) && /^[A-Za-z0-9_-]{8,64}$/.test(lobbyClient)) {
    handleLobbyConnection(socket, roomId, lobbyClient);
    return;
  }
  const player = Number.parseInt(url.searchParams.get('player') || '-1', 10);
  const playerCount = Number.parseInt(url.searchParams.get('players') || '2', 10);
  const signaling = url.searchParams.get('signal') === '1';
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(roomId) || !/^[A-Za-z0-9_-]{1,64}$/.test(runId) ||
      !Number.isInteger(player) || player < 0 || player > 2 || ![2, 3].includes(playerCount)) {
    socket.close(1008, 'invalid room or player');
    return;
  }

  if (signaling) {
    handleSignalConnection(socket, roomId, runId, player, playerCount);
    return;
  }

  const room = getRoom(roomId);
  const run = getRun(room, runId);
  if (run.clients.has(player)) {
    socket.close(1008, 'player slot already occupied');
    return;
  }
  run.clients.set(player, socket);
  console.log(`JOIN room=${roomId} run=${runId} player=${player} peers=${run.clients.size}`);
  maybeResolveRoute(roomId, runId, room, run);

  socket.on('message', (data, isBinary) => {
    if (!isBinary) {
      socket.close(1003, 'binary frames only');
      return;
    }
    const incoming = Buffer.from(data);
    let requestedPeer = null;
    let forwardedPayload = incoming;
    if (incoming.length >= 2 && incoming[0] === targetedEnvelopeMarker) {
      requestedPeer = incoming[1];
      forwardedPayload = incoming.subarray(2);
      if (!Number.isInteger(requestedPeer) || requestedPeer < 0 ||
          requestedPeer >= playerCount || requestedPeer === player) {
        socket.close(1008, 'invalid relay target');
        return;
      }
    }
    for (const [peer, target] of run.clients) {
      if (peer === player || target.readyState !== WebSocket.OPEN) continue;
      if (requestedPeer != null && peer !== requestedPeer) continue;
      const key = `${roomId}:${runId}:${player}->${peer}`;
      const sequence = (forwardCounters.get(key) || 0) + 1;
      forwardCounters.set(key, sequence);
      if (dropEvery > 0 && sequence % dropEvery === 0)
        continue;
      const spread = jitterMs > 0 ? ((sequence * 17) % (jitterMs * 2 + 1)) - jitterMs : 0;
      const wait = Math.max(0, delayMs + spread);
      const payload = Buffer.from(forwardedPayload);
      if (wait === 0) {
        target.send(payload, { binary: true });
      } else {
        setTimeout(() => {
          if (target.readyState === WebSocket.OPEN)
            target.send(payload, { binary: true });
        }, wait);
      }
    }
  });

  socket.on('close', () => {
    removeClient(roomId, runId, player, socket);
    console.log(`LEAVE room=${roomId} run=${runId} player=${player}`);
  });
  socket.on('error', error => {
    console.error(`SOCKET room=${roomId} player=${player} ${error.message}`);
  });
});

server.on('listening', () => {
  console.log(`TH07 LAN relay listening ws://${host}:${port} delay=${delayMs} jitter=${jitterMs} dropEvery=${dropEvery}`);
});

server.on('error', error => {
  console.error(error && error.stack || error);
  process.exitCode = 1;
});

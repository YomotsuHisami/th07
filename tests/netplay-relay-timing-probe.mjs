// Test-process-only observer. Imported before the shared relay; does not
// change relay policy, wire bytes, delivery order, or production services.
import { createRequire } from 'node:module';
import { performance } from 'node:perf_hooks';
const require = createRequire(new URL('../../eagler-touhou/package.json', import.meta.url));
const { WebSocket } = require('ws');
let received = 0, forwarded = 0, previousTime = performance.now();
const originalEmit = WebSocket.prototype.emit;
const originalSend = WebSocket.prototype.send;
function isInput(data) {
  if (!Buffer.isBuffer(data) && !(data instanceof Uint8Array)) return false;
  const offset = data[0] === 0xe7 ? 2 : 0;
  return data.length >= offset + 32 && data[offset] === 0x45 && data[offset + 5] === 1;
}
WebSocket.prototype.emit = function (name, ...args) {
  if (name === 'message' && isInput(args[0])) received++;
  return originalEmit.call(this, name, ...args);
};
WebSocket.prototype.send = function (data, ...args) {
  if (isInput(data)) forwarded++;
  return originalSend.call(this, data, ...args);
};
setInterval(() => {
  const now = performance.now();
  console.log(JSON.stringify({ probe: 'relay-timing', intervalMs: now - previousTime,
    received, forwarded }));
  previousTime = now;
}, 1000).unref();

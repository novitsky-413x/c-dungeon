/*
  Dependency-free WebSocket ↔ TCP bridge (single file).

  - Listens for WebSocket connections on an HTTP server
  - Upgrades on path /ws
  - For each WS client, opens a TCP socket to TARGET_HOST:TARGET_PORT
  - Pipes bytes bidirectionally (WS frames ⇄ raw TCP)

  Environment variables:
    PORT         HTTP listen port (default 8080)
    TARGET_HOST  TCP target host (default 127.0.0.1)
    TARGET_PORT  TCP target port (default 5555)

  Run:
    node ws_tcp_bridge.js

  Behind HTTPS:
    Put this behind a reverse proxy (Nginx/Caddy) that terminates TLS and forwards
    WebSocket upgrades from wss://runcode.at/ws → http://127.0.0.1:8080/ws
*/

const http = require('http');
const https = require('https');
const net = require('net');
const crypto = require('crypto');
const fs = require('fs');

const SSL_CERT = process.env.SSL_CERT || '';
const SSL_KEY = process.env.SSL_KEY || '';
const SSL_CA = process.env.SSL_CA || '';
const HOST = process.env.HOST || '0.0.0.0';
const TARGET_HOST = process.env.TARGET_HOST || '127.0.0.1';
const TARGET_PORT = parseInt(process.env.TARGET_PORT || '5555', 10);
const DEFAULT_PORT = 3000; // unique bridge port; no TLS required
const HTTP_PORT = parseInt(process.env.PORT || String(DEFAULT_PORT), 10);

function makeWebSocketAccept(secWebSocketKey) {
  const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
  return crypto.createHash('sha1').update(secWebSocketKey + GUID).digest('base64');
}

function buildWsFrame(payloadBuffer, opcode) {
  const finOpcode = 0x80 | (opcode & 0x0f);
  const length = payloadBuffer.length >>> 0;
  let header;
  if (length < 126) {
    header = Buffer.allocUnsafe(2);
    header[0] = finOpcode;
    header[1] = length; // no mask from server
  } else if (length <= 0xffff) {
    header = Buffer.allocUnsafe(4);
    header[0] = finOpcode;
    header[1] = 126; // 16-bit length
    header.writeUInt16BE(length, 2);
  } else {
    header = Buffer.allocUnsafe(10);
    header[0] = finOpcode;
    header[1] = 127; // 64-bit length
    // write big-endian 64-bit
    const hi = Math.floor(length / 2 ** 32);
    const lo = length >>> 0;
    header.writeUInt32BE(hi, 2);
    header.writeUInt32BE(lo, 6);
  }
  return Buffer.concat([header, payloadBuffer]);
}

function parseWsFrames(buffer, onFrame) {
  let offset = 0;
  while (true) {
    if (buffer.length - offset < 2) break;
    const b0 = buffer[offset];
    const b1 = buffer[offset + 1];
    const fin = (b0 & 0x80) !== 0;
    const opcode = b0 & 0x0f;
    const masked = (b1 & 0x80) !== 0;
    let length = b1 & 0x7f;
    let pos = offset + 2;
    if (length === 126) {
      if (buffer.length - pos < 2) break;
      length = buffer.readUInt16BE(pos);
      pos += 2;
    } else if (length === 127) {
      if (buffer.length - pos < 8) break;
      const hi = buffer.readUInt32BE(pos);
      const lo = buffer.readUInt32BE(pos + 4);
      pos += 8;
      const bigLen = hi * 2 ** 32 + lo;
      if (!Number.isSafeInteger(bigLen)) return { consumed: offset }; // too large; bail
      length = bigLen;
    }
    let maskingKey = null;
    if (masked) {
      if (buffer.length - pos < 4) break;
      maskingKey = buffer.slice(pos, pos + 4);
      pos += 4;
    }
    if (buffer.length - pos < length) break;
    let payload = buffer.slice(pos, pos + length);
    if (masked) {
      const out = Buffer.allocUnsafe(length);
      for (let i = 0; i < length; i++) out[i] = payload[i] ^ maskingKey[i & 3];
      payload = out;
    }
    onFrame({ fin, opcode, payload });
    offset = pos + length;
    if (!fin && opcode !== 0x0) {
      // Start of fragmented message; for this bridge, treat each frame as chunk anyway
    }
  }
  return { consumed: offset };
}

function writeWsText(socket, text) {
  const buf = Buffer.from(text, 'utf8');
  socket.write(buildWsFrame(buf, 0x1));
}

function writeWsBinary(socket, data) {
  const buf = Buffer.isBuffer(data) ? data : Buffer.from(data);
  socket.write(buildWsFrame(buf, 0x2));
}

function writeWsClose(socket, code, reason) {
  const body = Buffer.allocUnsafe(2 + (reason ? Buffer.byteLength(reason) : 0));
  body.writeUInt16BE(code || 1000, 0);
  if (reason) body.write(reason, 2);
  socket.write(buildWsFrame(body, 0x8));
}

let server;
let scheme;
if (SSL_CERT && SSL_KEY) {
  const opts = { key: fs.readFileSync(SSL_KEY), cert: fs.readFileSync(SSL_CERT) };
  if (SSL_CA) { opts.ca = fs.readFileSync(SSL_CA); }
  server = https.createServer(opts, (req, res) => {
    res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8' });
    res.end('WebSocket bridge (TLS) is running. Connect via wss://host:PORT/ws');
  });
  scheme = 'wss';
} else {
  server = http.createServer((req, res) => {
    res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8' });
    res.end('WebSocket bridge is running. Connect via ws://host:PORT/ws');
  });
  scheme = 'ws';
}

server.on('upgrade', (req, socket, head) => {
  if (!req.url || !req.headers.upgrade || req.headers.upgrade.toLowerCase() !== 'websocket') {
    socket.destroy();
    return;
  }
  const path = req.url.split('?')[0];
  if (path !== '/ws') {
    socket.write('HTTP/1.1 404 Not Found\r\n\r\n');
    socket.destroy();
    return;
  }
  const key = req.headers['sec-websocket-key'];
  const version = req.headers['sec-websocket-version'];
  if (!key || version !== '13') {
    socket.write('HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\n\r\n');
    socket.destroy();
    return;
  }
  const accept = makeWebSocketAccept(key);
  const headers = [
    'HTTP/1.1 101 Switching Protocols',
    'Upgrade: websocket',
    'Connection: Upgrade',
    `Sec-WebSocket-Accept: ${accept}`,
    '\r\n'
  ];
  socket.write(headers.join('\r\n'));

  const clientAddress = `${req.socket.remoteAddress}:${req.socket.remotePort}`;
  const tcp = net.createConnection({ host: TARGET_HOST, port: TARGET_PORT });

  let wsBuffer = Buffer.alloc(0);
  let closed = false;

  function safeClose() {
    if (closed) return;
    closed = true;
    try { tcp.destroy(); } catch {}
    try { socket.destroy(); } catch {}
  }

  tcp.on('connect', () => {
    // Connected to game TCP
  });

  tcp.on('data', (chunk) => {
    // Send to browser as binary WS frame
    try { writeWsBinary(socket, chunk); } catch { safeClose(); }
  });

  tcp.on('error', () => { try { writeWsClose(socket, 1011, 'TCP error'); } catch {}; safeClose(); });
  tcp.on('close', () => { try { writeWsClose(socket, 1000, 'TCP closed'); } catch {}; safeClose(); });

  socket.on('data', (data) => {
    wsBuffer = Buffer.concat([wsBuffer, data]);
    try {
      const { consumed } = parseWsFrames(wsBuffer, (frame) => {
        const { opcode, payload } = frame;
        if (opcode === 0x8) { // close
          try { writeWsClose(socket, 1000); } catch {}
          safeClose();
          return;
        }
        if (opcode === 0x9) { // ping
          try { socket.write(buildWsFrame(payload, 0xA)); } catch {}
          return;
        }
        if (opcode === 0x1 || opcode === 0x2 || opcode === 0x0) { // text/binary/continuation
          if (!tcp.destroyed) tcp.write(payload);
          return;
        }
      });
      if (consumed > 0) wsBuffer = wsBuffer.slice(consumed);
    } catch {
      safeClose();
    }
  });

  socket.on('error', () => { safeClose(); });
  socket.on('end', () => { safeClose(); });
});

function listenWithRetry(port, attemptsLeft) {
  server.once('error', (err) => {
    if (err && err.code === 'EADDRINUSE' && attemptsLeft > 0) {
      const next = port + 1;
      console.error(`[bridge] Port ${port} in use. Trying ${next} …`);
      setTimeout(() => listenWithRetry(next, attemptsLeft - 1), 100);
      return;
    }
    console.error('[bridge] Failed to start server:', err && err.message ? err.message : err);
    process.exit(1);
  });
  server.listen(port, HOST, () => {
    console.log(`[bridge] Listening on ${scheme}://${HOST}:${port}/ws → tcp://${TARGET_HOST}:${TARGET_PORT}`);
  });
}

listenWithRetry(HTTP_PORT, 20);



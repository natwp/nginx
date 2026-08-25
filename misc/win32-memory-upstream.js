'use strict';

const http = require('http');

const port = Number(process.argv[2]);
const delayMs = Number(process.argv[3] || 0);
const bodySize = Number(process.argv[4] || 0);

if (!Number.isInteger(port) || port < 1 || port > 65535
    || !Number.isInteger(delayMs) || delayMs < 0
    || !Number.isInteger(bodySize) || bodySize < 0) {
  throw new Error('usage: win32-memory-upstream.js port delay-ms body-bytes');
}

const body = Buffer.alloc(bodySize, 0x61);
let closing = false;

const server = http.createServer((request, response) => {
  if (request.url === '/__shutdown') {
    response.writeHead(200, {
      'Content-Length': '2',
      'Connection': 'close'
    });
    response.end('ok', () => {
      closing = true;
      if (typeof server.closeAllConnections === 'function') {
        server.closeAllConnections();
      }
      server.close(() => process.exit(0));
    });
    return;
  }

  const respond = () => {
    if (closing) {
      return;
    }

    response.writeHead(200, {
      'Content-Length': String(body.length),
      'Content-Type': 'application/octet-stream',
      'Connection': 'keep-alive'
    });
    response.end(body);
  };

  if (delayMs) {
    setTimeout(respond, delayMs);
  } else {
    respond();
  }
});

server.keepAliveTimeout = 30000;
server.headersTimeout = 35000;
server.listen(port, '127.0.0.1');

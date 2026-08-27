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
let activeRequests = 0;
let maxActiveRequests = 0;
let openConnections = 0;
let maxOpenConnections = 0;
let totalRequests = 0;

function stats() {
  return {
    delayMs,
    bodySize,
    activeRequests,
    maxActiveRequests,
    openConnections,
    maxOpenConnections,
    totalRequests
  };
}

const server = http.createServer((request, response) => {
  if (request.url === '/__health') {
    response.writeHead(200, {'Content-Length': '2'});
    response.end('ok');
    return;
  }

  if (request.url === '/__stats') {
    const payload = Buffer.from(`${JSON.stringify(stats())}\n`);
    response.writeHead(200, {
      'Content-Length': String(payload.length),
      'Content-Type': 'application/json',
      'Connection': 'close'
    });
    response.end(payload);
    return;
  }

  if (request.url === '/__reset') {
    maxActiveRequests = 0;
    maxOpenConnections = openConnections;
    totalRequests = 0;
    response.writeHead(200, {'Content-Length': '2'});
    response.end('ok');
    return;
  }

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

  activeRequests++;
  totalRequests++;
  maxActiveRequests = Math.max(maxActiveRequests, activeRequests);

  let completed = false;
  const complete = () => {
    if (!completed) {
      completed = true;
      activeRequests--;
    }
  };

  response.once('finish', complete);
  response.once('close', complete);

  const respond = () => {
    if (closing) {
      complete();
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

server.on('connection', socket => {
  openConnections++;
  maxOpenConnections = Math.max(maxOpenConnections, openConnections);
  socket.once('close', () => {
    openConnections--;
  });
});

server.keepAliveTimeout = 30000;
server.headersTimeout = 35000;
server.listen(port, '127.0.0.1');

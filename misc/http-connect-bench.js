'use strict';

const fs = require('fs');
const net = require('net');
const tls = require('tls');
const processCpu = require('./win32-process-cpu');

const port = Number(process.argv[2]);
const requestPath = process.argv[3] || '/empty.gif';
const connections = Number(process.argv[4] || 32);
const warmupMs = Number(process.argv[5] || 1000);
const sampleMs = Number(process.argv[6] || 2000);
const sampleCount = Number(process.argv[7] || 3);
const requestHeaderPaddingBytes = Number(process.argv[8] || 0);
const useTls = process.env.BENCH_TLS === '1';
const readPauseMs = Number(process.env.READ_PAUSE_MS || 0);
const outputFile = process.argv[9] || process.env.BENCH_OUTPUT_FILE || '';
const processPrefix = process.argv[10] || '';
const nginxPids = processCpu.findPids(processPrefix);
let sampleCpuStart;
let sampleClientCpuStart;

function writeResult(result) {
  const line = `${JSON.stringify(result)}\n`;

  if (outputFile) {
    fs.appendFileSync(outputFile, line);
    return;
  }

  process.stdout.write(line);
}

if (!Number.isInteger(port) || port < 1 || port > 65535) {
  throw new Error('a valid TCP port is required');
}

if (!Number.isInteger(connections) || connections < 1) {
  throw new Error('connections must be a positive integer');
}

if (!Number.isInteger(warmupMs) || warmupMs < 1
    || !Number.isInteger(sampleMs) || sampleMs < 1
    || !Number.isInteger(sampleCount) || sampleCount < 1) {
  throw new Error('warmup, sample duration, and sample count must be positive');
}

if (!Number.isInteger(requestHeaderPaddingBytes)
    || requestHeaderPaddingBytes < 0 || requestHeaderPaddingBytes > 12000) {
  throw new Error('request header padding must be an integer from 0 to 12000');
}

if (!Number.isInteger(readPauseMs) || readPauseMs < 0) {
  throw new Error('READ_PAUSE_MS must be a non-negative integer');
}

const padding = requestHeaderPaddingBytes
  ? `X-Bench-Padding: ${'x'.repeat(requestHeaderPaddingBytes)}\r\n`
  : '';

const request = Buffer.from(
  `GET ${requestPath} HTTP/1.1\r\n` +
  'Host: localhost\r\nConnection: close\r\n' +
  padding + '\r\n'
);

let phase = 'warmup';
let completed = 0;
let responseBytes = 0;
let errors = 0;
let latencies = [];
let sampleIndex = 0;
let phaseStarted = 0;
let timer;
let drainTimer;

const slots = Array.from({length: connections}, () => ({
  socket: null,
  header: Buffer.alloc(0),
  remaining: null,
  pending: false,
  started: 0,
  responseSize: 0
}));

function percentile(values, fraction) {
  if (values.length === 0) {
    return 0;
  }

  values.sort((a, b) => a - b);
  return values[Math.min(values.length - 1,
                         Math.floor(values.length * fraction))];
}

function resetStats() {
  completed = 0;
  responseBytes = 0;
  errors = 0;
  latencies = [];
  phaseStarted = performance.now();
}

function finishDrain() {
  if (phase !== 'drain' || slots.some(slot => slot.pending)) {
    return;
  }

  phase = 'done';
  clearTimeout(drainTimer);
}

function restartSlot(slot) {
  if (phase === 'drain') {
    finishDrain();
    return;
  }

  if (phase !== 'done') {
    setImmediate(() => startRequest(slot));
  }
}

function finishRequest(slot) {
  if (!slot.pending) {
    return;
  }

  slot.pending = false;

  if (phase === 'sample') {
    completed++;
    responseBytes += slot.responseSize;
    latencies.push(performance.now() - slot.started);
  }

  if (slot.socket) {
    slot.socket.destroy();
    slot.socket = null;
  }

  restartSlot(slot);
}

function failRequest(slot) {
  if (!slot.pending) {
    return;
  }

  slot.pending = false;

  if (phase === 'sample') {
    errors++;
  }

  if (slot.socket) {
    slot.socket.destroy();
    slot.socket = null;
  }

  restartSlot(slot);
}

function parseResponse(slot, input) {
  let chunk = input;

  while (chunk.length && slot.pending) {
    if (slot.remaining !== null) {
      const size = Math.min(chunk.length, slot.remaining);

      slot.remaining -= size;
      slot.responseSize += size;
      chunk = chunk.subarray(size);

      if (slot.remaining === 0) {
        finishRequest(slot);
      }

      continue;
    }

    slot.header = slot.header.length
      ? Buffer.concat([slot.header, chunk])
      : chunk;

    const headerEnd = slot.header.indexOf('\r\n\r\n');

    if (headerEnd < 0) {
      if (slot.header.length > 65536) {
        failRequest(slot);
      }

      return;
    }

    const header = slot.header.subarray(0, headerEnd).toString('latin1');
    const status = /^HTTP\/1\.[01] (\d{3})/.exec(header);
    const length = /\r\nContent-Length:\s*(\d+)/i.exec(`\r\n${header}`);

    if (!status || status[1] !== '200' || !length) {
      failRequest(slot);
      return;
    }

    chunk = slot.header.subarray(headerEnd + 4);
    slot.header = Buffer.alloc(0);
    slot.remaining = Number(length[1]);
    slot.responseSize = headerEnd + 4;

    if (slot.remaining === 0) {
      finishRequest(slot);
    }
  }
}

function startRequest(slot) {
  if (phase === 'drain' || phase === 'done') {
    finishDrain();
    return;
  }

  const options = {host: '127.0.0.1', port};
  const socket = useTls
    ? tls.connect({...options, rejectUnauthorized: false, servername: 'localhost'})
    : net.createConnection(options);

  slot.socket = socket;
  slot.header = Buffer.alloc(0);
  slot.remaining = null;
  slot.pending = true;
  slot.started = performance.now();
  slot.responseSize = 0;

  socket.setNoDelay(true);
  socket.setTimeout(10000);
  socket.on(useTls ? 'secureConnect' : 'connect', () => socket.write(request));
  socket.on('data', chunk => {
    parseResponse(slot, chunk);

    if (readPauseMs && !socket.destroyed) {
      socket.pause();
      setTimeout(() => {
        if (!socket.destroyed) {
          socket.resume();
        }
      }, readPauseMs);
    }
  });
  socket.on('error', () => failRequest(slot));
  socket.on('timeout', () => failRequest(slot));
  socket.on('end', () => failRequest(slot));
}

function beginSample() {
  sampleCpuStart = processCpu.snapshot(nginxPids);
  sampleClientCpuStart = processCpu.clientSnapshot();
  phase = 'sample';
  resetStats();
  timer = setTimeout(endSample, sampleMs);
}

function endSample() {
  const elapsed = (performance.now() - phaseStarted) / 1000;
  const sampleCpuEnd = processCpu.snapshot(nginxPids);
  const cpu = processCpu.utilization(sampleCpuStart, sampleCpuEnd,
                                     nginxPids.length);
  const clientCpu = processCpu.clientUtilization(sampleClientCpuStart,
                                                 elapsed);
  const average = latencies.length
    ? latencies.reduce((sum, value) => sum + value, 0) / latencies.length
    : 0;

  writeResult({
    path: requestPath,
    protocol: useTls ? 'https' : 'http',
    connectionMode: 'close',
    readPauseMs,
    requestHeaderPaddingBytes,
    sample: sampleIndex + 1,
    seconds: elapsed,
    requests: completed,
    requestsPerSecond: completed / elapsed,
    responseMiBPerSecond: responseBytes / elapsed / 1048576,
    latencyAverageMs: average,
    latencyP50Ms: percentile(latencies, 0.50),
    latencyP95Ms: percentile(latencies, 0.95),
    errors,
    ...cpu,
    ...clientCpu
  });

  sampleIndex++;

  if (sampleIndex < sampleCount) {
    beginSample();
    return;
  }

  phase = 'drain';
  clearTimeout(timer);
  drainTimer = setTimeout(() => {
    process.stderr.write('timed out draining final connection attempts\n');
    process.exitCode = 1;
    phase = 'done';

    for (const slot of slots) {
      if (slot.socket) {
        slot.socket.destroy();
      }
    }
  }, 30000);

  finishDrain();
}

resetStats();

for (const slot of slots) {
  startRequest(slot);
}

timer = setTimeout(beginSample, warmupMs);

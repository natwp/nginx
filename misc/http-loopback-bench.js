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
  'Host: localhost\r\nConnection: keep-alive\r\n' +
  padding + '\r\n'
);

let phase = 'connect';
let completed = 0;
let responseBytes = 0;
let errors = 0;
let latencies = [];
let connected = 0;
let sampleIndex = 0;
let phaseStarted = 0;
let timer;
let drainTimer;
const sockets = [];

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

function sendRequest(state) {
  if (phase === 'drain' || phase === 'done' || state.pending
      || state.socket.destroyed) {
    return;
  }

  state.pending = true;
  state.started = performance.now();
  state.socket.write(request);
}

function completeResponse(state) {
  state.pending = false;

  if (phase === 'sample') {
    completed++;
    responseBytes += state.responseSize;
    latencies.push(performance.now() - state.started);
  }

  state.responseSize = 0;

  if (phase === 'drain') {
    finishDrain();
    return;
  }

  sendRequest(state);
}

function finishDrain() {
  if (phase !== 'drain' || sockets.some(state => state.pending)) {
    return;
  }

  phase = 'done';
  clearTimeout(drainTimer);

  for (const state of sockets) {
    state.socket.destroy();
  }
}

function beginWarmup() {
  phase = 'warmup';
  resetStats();

  for (const state of sockets) {
    sendRequest(state);
  }

  timer = setTimeout(beginSample, warmupMs);
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
    process.stderr.write('timed out draining final in-flight requests\n');
    process.exitCode = 1;
    phase = 'done';

    for (const state of sockets) {
      state.socket.destroy();
    }
  }, 30000);

  finishDrain();
}

function parseResponses(state, input) {
  let chunk = input;

  while (chunk.length) {
    if (state.remaining !== null) {
      const size = Math.min(chunk.length, state.remaining);

      state.remaining -= size;
      state.responseSize += size;
      chunk = chunk.subarray(size);

      if (state.remaining === 0) {
        state.remaining = null;
        completeResponse(state);
      }

      continue;
    }

    state.header = state.header.length
      ? Buffer.concat([state.header, chunk])
      : chunk;

    const headerEnd = state.header.indexOf('\r\n\r\n');

    if (headerEnd < 0) {
      if (state.header.length > 65536) {
        errors++;
        state.socket.destroy();
      }

      return;
    }

    const header = state.header.subarray(0, headerEnd).toString('latin1');
    const status = /^HTTP\/1\.[01] (\d{3})/.exec(header);
    const length = /\r\nContent-Length:\s*(\d+)/i.exec(`\r\n${header}`);

    if (!status || status[1] !== '200' || !length) {
      errors++;
      state.socket.destroy();
      return;
    }

    chunk = state.header.subarray(headerEnd + 4);
    state.header = Buffer.alloc(0);
    state.remaining = Number(length[1]);
    state.responseSize = headerEnd + 4;

    if (state.remaining === 0) {
      state.remaining = null;
      completeResponse(state);
    }
  }
}

for (let i = 0; i < connections; i++) {
  const options = {host: '127.0.0.1', port};
  const socket = useTls
    ? tls.connect({...options, rejectUnauthorized: false, servername: 'localhost'})
    : net.createConnection(options);
  const state = {
    socket,
    header: Buffer.alloc(0),
    remaining: null,
    pending: false,
    started: 0,
    responseSize: 0
  };

  socket.setNoDelay(true);
  sockets.push(state);

  socket.on(useTls ? 'secureConnect' : 'connect', () => {
    connected++;

    if (connected === connections) {
      beginWarmup();
    }
  });

  socket.on('data', chunk => {
    parseResponses(state, chunk);

    if (readPauseMs && !socket.destroyed) {
      socket.pause();
      setTimeout(() => {
        if (!socket.destroyed) {
          socket.resume();
        }
      }, readPauseMs);
    }
  });

  socket.on('error', () => {
    errors++;

    if (phase === 'connect') {
      process.exitCode = 1;
    }
  });
}

setTimeout(() => {
  if (phase === 'connect') {
    throw new Error(`only ${connected}/${connections} connections opened`);
  }
}, 5000);

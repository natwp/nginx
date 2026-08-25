'use strict';

const fs = require('fs');
const dgram = require('dgram');
const processCpu = require('./win32-process-cpu');

const port = Number(process.argv[2]);
const connections = Number(process.argv[3] || 32);
const payloadBytes = Number(process.argv[4] || 64);
const expectedBytes = Number(process.argv[5] || 8);
const warmupMs = Number(process.argv[6] || 1000);
const sampleMs = Number(process.argv[7] || 2000);
const sampleCount = Number(process.argv[8] || 3);
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
  throw new Error('a valid UDP port is required');
}

if (!Number.isInteger(connections) || connections < 1 || connections > 1024) {
  throw new Error('connections must be an integer from 1 to 1024');
}

if (!Number.isInteger(payloadBytes) || payloadBytes < 0 || payloadBytes > 65507) {
  throw new Error('payload bytes must be an integer from 0 to 65507');
}

if (!Number.isInteger(expectedBytes) || expectedBytes < 0) {
  throw new Error('expected response bytes must be a non-negative integer');
}

const payload = Buffer.alloc(payloadBytes, 0x5a);
const expected = Buffer.alloc(expectedBytes, 0x55);
let phase = 'connect';
let connected = 0;
let sampleIndex = 0;
let completed = 0;
let responseBytes = 0;
let errors = 0;
let latencies = [];
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

function sendDatagram(state) {
  if (phase === 'drain' || phase === 'done' || state.pending
      || state.socket.destroyed) {
    return;
  }

  state.pending = true;
  state.started = performance.now();
  state.socket.send(payload, error => {
    if (error) {
      errors++;
      state.pending = false;
      if (phase === 'connect') {
        process.exitCode = 1;
      }
    }
  });
}

function finishDrain() {
  if (phase !== 'drain' || sockets.some(state => state.pending)) {
    return;
  }

  phase = 'done';
  clearTimeout(drainTimer);

  for (const state of sockets) {
    state.socket.close();
  }
}

function handleMessage(state, message) {
  if (!state.pending) {
    errors++;
    return;
  }

  state.pending = false;

  if (!message.equals(expected)) {
    errors++;
  }

  if (phase === 'sample' && state.started >= phaseStarted) {
    completed++;
    responseBytes += message.length;
    latencies.push(performance.now() - state.started);
  }

  if (phase === 'drain') {
    finishDrain();
    return;
  }

  sendDatagram(state);
}

function beginWarmup() {
  phase = 'warmup';
  resetStats();

  for (const state of sockets) {
    sendDatagram(state);
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
    path: `udp-${payloadBytes}`,
    payloadBytes,
    responseBytes: expectedBytes,
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
    process.stderr.write('timed out draining final UDP datagrams\n');
    process.exitCode = 1;
    phase = 'done';

    for (const state of sockets) {
      state.socket.close();
    }
  }, 30000);

  finishDrain();
}

for (let i = 0; i < connections; i++) {
  const socket = dgram.createSocket('udp4');
  const state = {
    socket,
    pending: false,
    started: 0
  };

  sockets.push(state);
  socket.on('message', message => handleMessage(state, message));
  socket.on('error', () => {
    errors++;
    state.pending = false;
    if (phase === 'connect') {
      process.exitCode = 1;
    }
  });
  socket.connect(port, '127.0.0.1', () => {
    connected++;

    if (connected === connections) {
      beginWarmup();
    }
  });
}

setTimeout(() => {
  if (phase === 'connect') {
    throw new Error(`only ${connected}/${connections} UDP sockets opened`);
  }
}, 5000);

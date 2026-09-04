'use strict';

const childProcess = require('child_process');
const fs = require('fs');
const processCpu = require('./win32-process-cpu');

const [ohaPath, url, connectionsText, durationText, nginxPrefix,
  outputPath, backend, label, workload, clientProcessesText = '1',
  connectionAuditText = '0', httpVersionText = ''] =
  process.argv.slice(2);

if (!ohaPath || !url || !connectionsText || !durationText || !nginxPrefix
    || !outputPath) {
  throw new Error(
    'usage: win32-oha-runner.js oha.exe url connections duration '
    + 'nginx-prefix output [backend] [label] [workload] [client-processes] '
    + '[connection-audit] [http-version]'
  );
}

const connections = Number(connectionsText);
const clientProcesses = Number(clientProcessesText);
const connectionAudit = Number(connectionAuditText);
const duration = durationText;

if (!Number.isInteger(connections) || connections < 1) {
  throw new Error('connections must be a positive integer');
}

if (!Number.isInteger(clientProcesses) || clientProcesses < 1
    || clientProcesses > connections) {
  throw new Error(
    'client-processes must be a positive integer no larger than connections'
  );
}

if (connectionAudit !== 0 && connectionAudit !== 1) {
  throw new Error('connection-audit must be 0 or 1');
}

function durationMilliseconds(value) {
  const match = /^([0-9]+(?:\.[0-9]+)?)(ms|s|m)$/.exec(value);
  if (!match) {
    throw new Error(`unsupported duration: ${value}`);
  }

  const number = Number(match[1]);
  const scale = match[2] === 'ms' ? 1 : match[2] === 's' ? 1000 : 60000;
  return number * scale;
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2
    ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

function finiteMedian(values) {
  const finite = values.map(Number).filter(Number.isFinite);
  return finite.length ? median(finite) : null;
}

function normalizeWindowsSocketError(value) {
  const text = String(value);
  const match = /\(os error ([0-9]+)\)/.exec(text);
  const messages = {
    10013: 'permission denied',
    10048: 'address already in use',
    10049: 'cannot assign requested address',
    10054: 'connection reset by peer',
    10055: 'no buffer space available',
    10060: 'connection timed out',
    10061: 'connection refused'
  };

  if (!match || !messages[match[1]]) {
    return text;
  }

  const prefix = text.lastIndexOf(':', match.index);
  const error = `${messages[match[1]]} (os error ${match[1]})`;

  return prefix === -1 ? error : `${text.slice(0, prefix + 1)} ${error}`;
}

function spawnOha(clientConnections) {
  const child = childProcess.spawn(ohaPath, [
    '-z', duration,
    '-c', String(clientConnections),
    '--wait-ongoing-requests-after-deadline',
    '--no-tui',
    '--output-format', 'json',
    '--ipv4',
    ...(httpVersionText ? ['--http-version', httpVersionText] : []),
    url
  ], {
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe']
  });
  const stdout = [];
  const stderr = [];

  child.stdout.on('data', chunk => stdout.push(chunk));
  child.stderr.on('data', chunk => stderr.push(chunk));

  const done = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => {
      const out = Buffer.concat(stdout).toString('utf8');
      const err = Buffer.concat(stderr).toString('utf8');

      if (code !== 0) {
        reject(new Error(
          normalizeWindowsSocketError(err)
            || `oha exited with code ${code}, signal ${signal || 'none'}`
        ));
        return;
      }

      try {
        resolve(JSON.parse(out));
      } catch (error) {
        process.stderr.write(out || 'oha produced no JSON\n');
        reject(error);
      }
    });
  });

  return {child, done};
}

function tcpServerConnections(port) {
  try {
    const output = childProcess.execFileSync(
      'C:\\Windows\\System32\\netstat.exe',
      ['-ano', '-p', 'tcp'],
      {
        encoding: 'utf8',
        maxBuffer: 64 * 1024 * 1024,
        windowsHide: true
      }
    );
    let established = 0;
    let total = 0;

    for (const line of output.replace(/\r/g, '').split('\n')) {
      const fields = line.trim().split(/\s+/);
      if (fields.length < 4 || fields[0].toUpperCase() !== 'TCP') {
        continue;
      }

      const local = fields[1];
      const state = fields[3].toUpperCase();
      if (!local.endsWith(`:${port}`)) {
        continue;
      }

      total++;
      if (state === 'ESTABLISHED') {
        established++;
      }
    }

    return {port, established, total, error: null};
  } catch (error) {
    return {
      port,
      established: null,
      total: null,
      error: String(error.message || error)
    };
  }
}

async function main() {
  const nginxProcesses = processCpu.findProcesses(nginxPrefix);
  const baseConnections = Math.floor(connections / clientProcesses);
  let remaining = connections % clientProcesses;
  const clients = [];

  for (let i = 0; i < clientProcesses; i++) {
    const count = baseConnections + (remaining-- > 0 ? 1 : 0);
    clients.push(spawnOha(count));
  }

  const clientPids = clients.map(client => client.child.pid)
    .filter(pid => Number.isInteger(pid) && pid > 0);
  const nginxPids = nginxProcesses.map(process => process.pid);
  const sampledPids = [...nginxPids, ...clientPids];
  const durationMs = durationMilliseconds(duration);
  const target = new URL(url);
  const targetPort = Number(target.port || (target.protocol === 'https:'
    ? 443 : 80));
  const auditDelay = Math.min(1000, Math.max(250, durationMs / 3));
  const connectionAuditPromise = connectionAudit
    ? new Promise(resolve => {
      setTimeout(() => resolve(tcpServerConnections(targetPort)), auditDelay);
    })
    : Promise.resolve(null);
  await new Promise(resolve => setTimeout(resolve, 150));
  const cpuStart = processCpu.snapshot(sampledPids);
  const started = performance.now();
  const probeDelay = Math.max(250, durationMs - 350);
  const cpuEndPromise = new Promise(resolve => {
    setTimeout(() => resolve(processCpu.snapshot(sampledPids)), probeDelay);
  });
  const results = await Promise.all(clients.map(client => client.done));
  const cpuEnd = await cpuEndPromise;
  const tcpAudit = await connectionAuditPromise;
  const elapsed = (performance.now() - started) / 1000;
  const summaries = results.map(result => result.summary || {});
  const latencies = results.map(result =>
    (result.metrics || {}).latency_ms || {});
  const durationSeconds = finiteMedian(summaries.map(summary => summary.total))
    || elapsed;
  const requestsPerSecond = summaries.reduce((sum, summary) =>
    sum + Number(summary.requestsPerSec || 0), 0);
  const requests = summaries.reduce((sum, summary) =>
    sum + Number(summary.requestsPerSec || 0)
      * Number(summary.total || durationSeconds), 0);
  const successfulRequests = summaries.reduce((sum, summary) => {
    const rate = Number(summary.requestsPerSec || 0);
    const seconds = Number(summary.total || durationSeconds);
    return sum + rate * seconds * Number(summary.successRate || 0);
  }, 0);
  const latencyWeights = summaries.map((summary, index) =>
    Number(summary.requestsPerSec || 0)
      * Number(summary.total || durationSeconds));
  const latencyWeight = latencyWeights.reduce((sum, value) => sum + value, 0);
  const latencyAverageMs = latencyWeight
    ? summaries.reduce((sum, summary, index) =>
      sum + Number(summary.average || 0) * 1000 * latencyWeights[index], 0)
      / latencyWeight
    : null;
  const nginxCpu = processCpu.utilizationByRole(
    cpuStart, cpuEnd, nginxProcesses
  );
  const clientCpu = processCpu.utilization(
    cpuStart, cpuEnd, clientPids.length, clientPids
  );
  const errorDistribution = {};
  const nginxWorkerCpu = nginxCpu.nginxCpuByRole
    ? (nginxCpu.nginxCpuByRole.worker || nginxCpu.nginxCpuByRole.single)
    : null;
  const nginxMasterRouterCpu = nginxCpu.nginxCpuByRole
    ? nginxCpu.nginxCpuByRole['master-router']
    : null;

  for (const result of results) {
    for (const [error, count] of Object.entries(
      result.errorDistribution || {}
    )) {
      const normalized = normalizeWindowsSocketError(error);
      errorDistribution[normalized] = (errorDistribution[normalized] || 0)
        + Number(count);
    }
  }

  const enriched = {
    client: 'oha-windows-amd64-pgo',
    clientProcesses,
    backend: backend || null,
    label: label || null,
    workload: workload || null,
    path: workload || url,
    url,
    connections,
    duration,
    elapsed,
    durationSeconds,
    requests,
    requestsPerSecond,
    responseMiBPerSecond: summaries.reduce((sum, summary) =>
      sum + Number(summary.sizePerSec || 0), 0) / 1048576,
    totalDataBytes: summaries.reduce((sum, summary) =>
      sum + Number(summary.totalData || 0), 0),
    successRate: requests ? successfulRequests / requests : 0,
    errors: Math.max(0, Math.round(requests - successfulRequests)),
    latencyAverageMs,
    latencyP50Ms: finiteMedian(latencies.map(latency => latency.p50)),
    latencyP95Ms: finiteMedian(latencies.map(latency => latency.p95)),
    latencyP99Ms: finiteMedian(latencies.map(latency => latency.p99)),
    errorDistribution,
    tcpServerPort: tcpAudit ? tcpAudit.port : null,
    tcpEstablishedAtAudit: tcpAudit ? tcpAudit.established : null,
    tcpConnectionsAtAudit: tcpAudit ? tcpAudit.total : null,
    tcpConnectionAuditError: tcpAudit ? tcpAudit.error : null,
    ...nginxCpu,
    nginxWorkerCpuPercent: nginxWorkerCpu
      ? nginxWorkerCpu.cpuPercent : null,
    nginxWorkerCpuCores: nginxWorkerCpu ? nginxWorkerCpu.cpuCores : null,
    nginxMasterRouterCpuPercent: nginxMasterRouterCpu
      ? nginxMasterRouterCpu.cpuPercent : null,
    nginxMasterRouterCpuCores: nginxMasterRouterCpu
      ? nginxMasterRouterCpu.cpuCores : null,
    clientCpuPercent: clientCpu.nginxCpuPercent,
    clientCpuCores: clientCpu.nginxCpuCores,
    clientCpuPidCount: clientCpu.nginxCpuPidCount
  };

  fs.writeFileSync(outputPath, `${JSON.stringify(enriched)}\n`);
}

main().catch(error => {
  process.stderr.write(`${normalizeWindowsSocketError(error.stack || error)}\n`);
  process.exitCode = 1;
});

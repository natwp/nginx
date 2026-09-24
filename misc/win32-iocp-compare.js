'use strict';

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const bench = require('./win32-ci-benchmark');
const base = require('./benchmark-report');
const report = require('./iocp-performance-report');

const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const writeJson = (file, data) => fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n');
const append = (file, data) => fs.appendFileSync(file, JSON.stringify(data) + '\n');
const maskText = bits => '0x' + bits.toString(16);

function options(argv) {
  const result = {kind: 'static', profile: 'isolated', repeats: 6,
    connections: 32, sampleSeconds: 2, warmupSeconds: 1, measureClient: true};
  const names = {baseline: 'baseline', candidate: 'candidate', oha: 'oha',
    helper: 'helper', openssl: 'openssl', output: 'output', kind: 'kind',
    profile: 'profile', provenance: 'provenance', repeats: 'repeats'};
  for (let i = 0; i < argv.length; i++) {
    const key = names[argv[i].replace(/^--/, '')];
    assert(key && argv[i + 1], `unknown or incomplete argument ${argv[i]}`);
    result[key] = argv[++i];
  }
  for (const key of ['baseline', 'candidate', 'oha', 'helper', 'openssl', 'output', 'provenance']) {
    assert(result[key], `--${key} is required`);
    result[key] = path.resolve(result[key]);
    if (key !== 'output') assert(fs.existsSync(result[key]), `missing ${key}`);
  }
  result.repeats = Number(result.repeats);
  assert(Number.isInteger(result.repeats) && result.repeats >= 6
    && result.repeats % 6 === 0, 'repeats must be a positive multiple of six');
  assert(['static', 'proxy'].includes(result.kind), 'invalid workload family');
  assert(['isolated', 'shared'].includes(result.profile), 'invalid CPU profile');
  return result;
}

function cpuPlan(available, kind, profile) {
  const bits = [];
  for (let i = 0n; i < 64n; i++) {
    if (available & (1n << i)) bits.push(1n << i);
  }
  assert(bits.length >= 4, 'at least four available logical CPUs are required');
  if (profile === 'shared') {
    return {client: available, nginx: available, upstream: available,
      clientBudget: Math.min(2, bits.length), upstreamBudget: bits.length, multiWorkers: 4};
  }
  // Reserve the same two server CPUs in both workload families. Static
  // needs no upstream, so its client can use both remaining CPUs.
  return {client: kind === 'static' ? bits[0] | bits[1] : bits[0],
    nginx: bits[2] | bits[3], upstream: bits[1],
    clientBudget: kind === 'static' ? 2 : 1, upstreamBudget: 1, multiWorkers: 2};
}

async function pin(settings, pids, mask, identities) {
  const before = identities || (await bench.snapshot(settings, pids)).processes;
  for (const pid of pids) {
    assert(before[pid], `missing process to pin: ${pid}`);
    await bench.checked(settings.helper, ['affinity', String(pid), mask, before[pid].created]);
  }
  const after = await bench.snapshot(settings, pids);
  for (const pid of pids) {
    assert(after.processes[pid] && after.processes[pid].created === before[pid].created
      && after.processes[pid].affinity === mask, `affinity was not applied to ${pid}`);
  }
  return after;
}

function verifyAffinity(counters, pids, mask) {
  for (const pid of pids) {
    assert(counters.processes[pid] && counters.processes[pid].affinity === mask,
      `affinity changed for process ${pid}`);
  }
}

async function run(settings) {
  assert(process.platform === 'win32', 'native Windows Node.js is required');
  assert(process.env.GITHUB_ACTIONS === 'true', 'performance trials must run in CI');
  assert(!fs.existsSync(settings.output) || !fs.readdirSync(settings.output).length,
    'output directory must be empty');
  fs.mkdirSync(path.join(settings.output, 'data'), {recursive: true});
  const rawDirectory = path.join(settings.output, 'raw');
  fs.mkdirSync(rawDirectory);
  for (const size of base.sizes) {
    fs.writeFileSync(path.join(settings.output, 'data', size.file), Buffer.alloc(size.bytes, 0x61));
  }
  fs.writeFileSync(path.join(settings.output, 'openssl.cnf'), '[req]\ndistinguished_name=dn\n[dn]\n');
  await bench.checked(settings.openssl, ['req', '-config', path.join(settings.output, 'openssl.cnf'),
    '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
    '-keyout', path.join(settings.output, 'key.pem'), '-out', path.join(settings.output, 'cert.pem')]);
  const provenance = JSON.parse(fs.readFileSync(settings.provenance, 'utf8'));
  assert(provenance.candidate === process.env.GITHUB_SHA, 'candidate source does not match the run');
  const binaries = {};
  for (const variant of report.variants) {
    const binary = variant === 'candidate' ? settings.candidate : settings.baseline;
    const version = (await bench.checked(binary, ['-V'])).stderr.trim();
    assert(!version.includes('--with-debug'), 'benchmark binaries must be optimized');
    binaries[variant] = {sha256: hash(fs.readFileSync(binary)), nginxVersion: version,
      sourceCommit: variant === 'candidate' ? provenance.candidate : provenance.baseline};
  }
  const original = await bench.snapshot(settings, [process.pid]);
  assert(original.processes[process.pid], 'missing controller identity');
  const cpus = cpuPlan(BigInt(original.processes[process.pid].affinity), settings.kind, settings.profile);
  settings.multiWorkers = cpus.multiWorkers;
  settings.clientAffinity = maskText(cpus.client);
  const nginxAffinity = maskText(cpus.nginx), upstreamAffinity = maskText(cpus.upstream);
  const manifest = {schemaVersion: 1, kind: settings.kind, profile: settings.profile,
    repeats: settings.repeats, sampleSeconds: settings.sampleSeconds,
    warmupSeconds: settings.warmupSeconds, connections: settings.connections, clientThreads: 2,
    multiWorkers: settings.multiWorkers, binaries, provenance,
    clientCpuBudget: cpus.clientBudget, upstreamCpuBudget: cpus.upstreamBudget,
    affinity: {client: settings.clientAffinity, nginx: nginxAffinity,
      upstream: settings.kind === 'proxy' ? upstreamAffinity : null},
    runUrl: `https://github.com/${process.env.GITHUB_REPOSITORY}/actions/runs/${process.env.GITHUB_RUN_ID}`,
    runAttempt: process.env.GITHUB_RUN_ATTEMPT, startedAt: new Date().toISOString(),
    runner: {os: os.type(), release: os.release(), arch: os.arch(),
      logicalProcessors: os.cpus().length, cpu: os.cpus()[0].model,
      totalMemoryBytes: os.totalmem(), node: process.version},
    ohaSha256: hash(fs.readFileSync(settings.oha)),
    ohaVersion: (await bench.checked(settings.oha, ['--version'])).stdout.trim(),
    helperSha256: hash(fs.readFileSync(settings.helper)),
    tls: 'TLSv1.3 / TLS_AES_128_GCM_SHA256; client-facing only',
    proxy: {transport: 'HTTP/1.1', buffering: false, body: 'memory', slowDelayMs: 20},
    policy: base.policy};
  writeJson(path.join(settings.output, 'manifest.json'), manifest);
  const rows = report.cases(manifest), probes = new Set();
  let ordinal = 0, upstream;
  try {
    // Children inherit the client mask from process creation. nginx and the
    // upstream are moved and verified before any warm-up or measurement.
    await pin(settings, [process.pid], settings.clientAffinity, original.processes);
    if (settings.kind === 'proxy') {
      upstream = await bench.startUpstream(settings);
      await pin(settings, [upstream.child.pid], upstreamAffinity);
    }
    for (let round = 0; round < settings.repeats; round++) {
      const workerOrder = round % 2 ? [settings.multiWorkers, 1] : [1, settings.multiWorkers];
      for (const workers of workerOrder) {
        const states = new Map();
        try {
          for (const variant of report.order(round, 0)) {
            const arm = {...settings, instance: variant,
              binary: variant === 'candidate' ? settings.candidate : settings.baseline};
            const nginx = await bench.startNginx(arm, {backend: 'iocp', workers},
              round, upstream && upstream.port);
            states.set(variant, {arm, nginx});
            await pin(settings, nginx.pids, nginxAffinity, nginx.identities);
          }
          const group = rows.filter(row => row.workers === workers);
          for (const row of base.workloadOrder(group, round, workerOrder.indexOf(workers))) {
            for (const variant of report.order(round, rows.indexOf(row))) {
              const {arm, nginx} = states.get(variant);
              const url = `${row.tls ? 'https' : 'http'}://127.0.0.1:${nginx.ports[row.tls ? 1 : 0]}/${row.mode}/${row.file}`;
              const stem = path.join(rawDirectory, `r${round + 1}-${row.id}-${variant}`);
              const probeId = `${row.id}/${variant}`;
              if (!probes.has(probeId)) {
                const probe = await bench.request(url);
                assert(probe.status === 200 && probe.body.equals(Buffer.alloc(row.bytes, 0x61)),
                  `invalid response body: ${probeId}`);
                assert(!row.tls || probe.protocol === 'TLSv1.3', 'unexpected TLS protocol');
                append(path.join(settings.output, 'probes.jsonl'), {id: row.id, variant,
                  bytes: probe.body.length, sha256: hash(probe.body), protocol: probe.protocol});
                probes.add(probeId);
              }
              const idle = await bench.idleCpu(settings);
              await bench.load(arm, url, settings.warmupSeconds, stem + '-warmup.json', row.bytes);
              const pids = [...nginx.pids, ...(upstream ? [upstream.child.pid] : [])];
              const before = await bench.snapshot(settings, pids);
              const result = await bench.load(arm, url, settings.sampleSeconds, stem + '.json', row.bytes);
              const after = await bench.snapshot(settings, pids);
              for (const counters of [before, after]) {
                verifyAffinity(counters, nginx.pids, nginxAffinity);
                if (upstream) verifyAffinity(counters, [upstream.child.pid], upstreamAffinity);
              }
              const sample = {id: row.id, variant, round, ordinal: ordinal++, ...result,
                idleCpuPercent: idle, hostCpuPercent: bench.hostCpu(before, after),
                nginxCpuCores: bench.processCpu(before, after, nginx.pids),
                upstreamCpuCores: upstream
                  ? bench.processCpu(before, after, [upstream.child.pid]) : null,
                counters: {before, after},
                rawFile: path.relative(settings.output, stem + '.json').replace(/\\/g, '/')};
              append(path.join(settings.output, 'samples.jsonl'), sample);
              process.stdout.write(`round ${round + 1}/${settings.repeats} ${row.id} ${variant}: `
                + `${result.requestsPerSecond.toFixed(1)} req/s; client ${result.clientCpuCores.toFixed(2)} CPUs\n`);
            }
          }
        } finally {
          // Finish every started instance even if one failed its stop checks.
          const results = await Promise.allSettled([...states.values()]
            .map(({arm, nginx}) => bench.stopNginx(arm, nginx)));
          const failure = results.find(result => result.status === 'rejected');
          if (failure) throw failure.reason;
        }
      }
    }
    manifest.completedAt = new Date().toISOString();
    writeJson(path.join(settings.output, 'manifest.json'), manifest);
    const summary = report.main(settings.output);
    process.stdout.write(`${summary.totalSamples} valid samples: ${summary.decision}\n`);
  } catch (error) {
    fs.writeFileSync(path.join(settings.output, 'failure.txt'), error.stack + '\n');
    throw error;
  } finally {
    if (upstream) upstream.close();
    fs.rmSync(path.join(settings.output, 'key.pem'), {force: true});
    await pin(settings, [process.pid], original.processes[process.pid].affinity, original.processes);
  }
}

if (require.main === module) {
  run(options(process.argv.slice(2))).catch(error => {
    process.stderr.write(`${error.stack}\n`);
    process.exitCode = 1;
  });
}

module.exports = {options, cpuPlan, run};

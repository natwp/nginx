'use strict';

const assert = require('assert');
const childProcess = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const https = require('https');
const net = require('net');
const os = require('os');
const path = require('path');
const report = require('./benchmark-report');

const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const quote = value => '"' + value.replace(/\\/g, '/').replace(/"/g, '\\"') + '"';
const writeJson = (file, data) => fs.writeFileSync(file, JSON.stringify(data, null, 2) + '\n');

function options(argv) {
  const result = {suite: 'quick', multiWorkers: 4, connections: 32,
    smoke: false, kind: 'static'};
  const names = {binary: 'binary', oha: 'oha', helper: 'helper', openssl: 'openssl',
    output: 'output', kind: 'kind', suite: 'suite', 'multi-workers': 'multiWorkers'};
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--smoke') {
      result.smoke = true;
      continue;
    }
    const key = names[argv[i].replace(/^--/, '')];
    assert(key && argv[i + 1], `unknown or incomplete argument ${argv[i]}`);
    result[key] = argv[++i];
  }
  for (const key of ['binary', 'oha', 'helper', 'openssl', 'output']) {
    assert(result[key], `--${key} is required`);
    result[key] = path.resolve(result[key]);
    if (key !== 'output') assert(fs.existsSync(result[key]), `missing ${key}`);
  }
  result.multiWorkers = Number(result.multiWorkers);
  report.cases(result.kind, result.multiWorkers);
  assert(['quick', 'standard', 'comprehensive'].includes(result.suite), 'invalid suite');
  result.repeats = result.smoke ? 1 : result.suite === 'comprehensive' ? 12 : 6;
  result.sampleSeconds = result.smoke ? 0.2 : result.suite === 'quick' ? 2 : 3;
  result.warmupSeconds = result.smoke ? 0.1 : 1;
  return result;
}

function execute(file, args, timeout = 20000, cwd, watcher) {
  return new Promise((resolve, reject) => {
    const child = childProcess.spawn(file, args, {
      cwd, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']
    });
    const stdout = [], stderr = [];
    const usage = watcher && child.pid
      ? execute(watcher, ['watch', String(child.pid)], timeout + 5000)
        .catch(error => ({code: -1, stderr: error.message}))
      : Promise.resolve(null);
    let timedOut = false;
    const timer = setTimeout(() => { timedOut = true; child.kill(); }, timeout);
    child.stdout.on('data', chunk => stdout.push(chunk));
    child.stderr.on('data', chunk => stderr.push(chunk));
    child.once('error', error => { clearTimeout(timer); reject(error); });
    child.once('close', async code => {
      clearTimeout(timer);
      resolve({code, timedOut, stdout: Buffer.concat(stdout).toString(),
        stderr: Buffer.concat(stderr).toString(), usage: await usage});
    });
  });
}

async function checked(file, args, timeout, cwd) {
  const result = await execute(file, args, timeout, cwd);
  assert(result.code === 0 && !result.timedOut,
    `${path.basename(file)} failed (${result.code}): ${result.stderr}`);
  return result;
}

async function freePort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}

function request(url) {
  return new Promise((resolve, reject) => {
    const secure = url.startsWith('https:');
    const client = (secure ? https : http).get(url, {
      agent: false, rejectUnauthorized: false, timeout: 3000
    }, response => {
      const chunks = [];
      const protocol = secure ? response.socket.getProtocol() : 'HTTP/1.1';
      response.on('data', chunk => chunks.push(chunk));
      response.once('error', reject);
      response.once('end', () => resolve({status: response.statusCode,
        body: Buffer.concat(chunks), protocol}));
    });
    client.once('timeout', () => client.destroy(new Error('probe timed out')));
    client.once('error', reject);
  });
}

function configuration(settings, condition, ports, upstreamPort) {
  const tls = [
    `ssl_certificate ${quote(path.join(settings.output, 'cert.pem'))};`,
    `ssl_certificate_key ${quote(path.join(settings.output, 'key.pem'))};`,
    'ssl_protocols TLSv1.3;',
    'ssl_conf_command Ciphersuites TLS_AES_128_GCM_SHA256;',
    'ssl_session_cache shared:BENCH:1m;',
    'ssl_session_tickets off;'
  ];
  const routes = ['location = /__bench_worker { return 200 "$pid"; }'];
  if (settings.kind === 'static') {
    for (const mode of ['off', 'on']) {
      for (const size of report.sizes) {
        routes.push(`location = /${mode}/${size.file} {`,
          `    alias ${quote(path.join(settings.output, 'data', size.file))};`,
          `    sendfile ${mode};`, '}', '');
      }
    }
  } else {
    routes.push('location / {', '    proxy_pass http://bench_upstream;',
      '    proxy_http_version 1.1;', '    proxy_set_header Connection "";',
      '    proxy_buffering off;', '    proxy_request_buffering off;',
      '    proxy_cache off;', '    proxy_connect_timeout 2s;',
      '    proxy_read_timeout 10s;', '}');
  }
  return [
    `worker_processes ${condition.workers};`,
    'error_log logs/error.log notice;', 'pid nginx.pid;',
    `events { use ${condition.backend}; worker_connections 1024; }`,
    'http {', '    access_log off;', '    default_type application/octet-stream;',
    '    gzip off;', '    sendfile off;', '    tcp_nopush off;',
    '    tcp_nodelay on;', '    keepalive_timeout 60;',
    '    keepalive_requests 1000000;',
    ...(settings.kind === 'proxy' ? [
      `    upstream bench_upstream { server 127.0.0.1:${upstreamPort}; keepalive 64; }`
    ] : []),
    ...ports.flatMap((port, index) => [
      '    server {', `        listen 127.0.0.1:${port}${index ? ' ssl' : ''};`,
      '        server_name localhost;',
      ...(index ? tls.map(line => '        ' + line) : []),
      ...routes.map(line => '        ' + line), '    }'
    ]), '}', ''
  ].join('\n');
}

async function snapshot(settings, pids = []) {
  const value = await checked(settings.helper, ['snapshot', ...pids.map(String)]);
  return JSON.parse(value.stdout);
}

function hostCpu(before, after) {
  const total = after.kernel + after.user - before.kernel - before.user;
  assert(total > 0, 'CPU sampling interval is empty');
  return Math.max(0, Math.min(100, 100 * (1 - (after.idle - before.idle) / total)));
}

function processCpu(before, after, pids) {
  const elapsed = (after.at - before.at) / 1000;
  assert(elapsed > 0);
  let ticks = 0;
  for (const pid of pids) {
    const a = before.processes[pid], b = after.processes[pid];
    assert(a && b && a.created === b.created, `benchmark process ${pid} exited`);
    ticks += b.ticks - a.ticks;
  }
  return ticks / 10000000 / elapsed;
}

async function idleCpu(settings) {
  let value;
  for (let attempt = 0; attempt < (settings.smoke ? 1 : 3); attempt++) {
    const before = await snapshot(settings);
    await delay(200);
    value = hostCpu(before, await snapshot(settings));
    if (value <= report.policy.maximumIdleCpuPercent) break;
    await delay(100);
  }
  return value;
}

async function startUpstream(settings) {
  const child = childProcess.spawn(settings.helper, ['serve', '0', '20'], {
    windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']
  });
  const stdout = [], stderr = [];
  child.stderr.on('data', chunk => stderr.push(chunk));
  try {
    const port = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('upstream startup timed out')), 10000);
      child.once('error', error => { clearTimeout(timer); reject(error); });
      child.once('exit', code => { clearTimeout(timer);
        reject(new Error(`upstream exited with ${code}`)); });
      child.stdout.on('data', chunk => {
        stdout.push(chunk);
        const match = /^READY (\d+)\r?$/m.exec(Buffer.concat(stdout).toString());
        if (match) { clearTimeout(timer); resolve(Number(match[1])); }
      });
    });
    return {child, port, close() {
      child.kill();
      fs.writeFileSync(path.join(settings.output, 'upstream.stdout'), Buffer.concat(stdout));
      fs.writeFileSync(path.join(settings.output, 'upstream.stderr'), Buffer.concat(stderr));
    }};
  } catch (error) {
    child.kill();
    throw error;
  }
}

async function startNginx(settings, condition, round, upstreamPort) {
  const prefix = path.join(settings.output, 'prefixes',
    `r${round + 1}-${condition.backend}-w${condition.workers}`
      + (settings.instance ? `-${settings.instance}` : ''));
  fs.mkdirSync(path.join(prefix, 'logs'), {recursive: true});
  const ports = [await freePort(), await freePort()];
  assert(ports[0] !== ports[1]);
  const config = path.join(prefix, 'nginx.conf');
  fs.writeFileSync(config, configuration(settings, condition, ports, upstreamPort));
  const args = ['-p', prefix.replace(/\\/g, '/') + '/', '-c', config];
  const configTest = await checked(settings.binary, [...args, '-t'], 20000, prefix);
  fs.writeFileSync(path.join(prefix, 'config-test.log'), configTest.stdout + configTest.stderr);
  const out = fs.openSync(path.join(prefix, 'stdout.log'), 'w');
  const err = fs.openSync(path.join(prefix, 'stderr.log'), 'w');
  const child = childProcess.spawn(settings.binary, args, {
    cwd: prefix, windowsHide: true, stdio: ['ignore', out, err]
  });
  fs.closeSync(out); fs.closeSync(err);
  const state = {child, prefix, args, ports, identities: {}};
  child.once('error', error => { state.spawnError = error; });
  try {
    let ready = false;
    for (let attempt = 0; attempt < 100; attempt++) {
      if (state.spawnError) throw state.spawnError;
      assert(child.exitCode === null, 'nginx exited during startup');
      try {
        const log = fs.readFileSync(path.join(prefix, 'logs/error.log'), 'utf8');
        const workers = [...log.matchAll(/start worker process (\d+)/g)].map(match => Number(match[1]));
        state.pids = [child.pid, ...workers];
        const counters = await snapshot(settings, state.pids);
        Object.assign(state.identities, counters.processes);
        const response = await request(`http://127.0.0.1:${ports[0]}/__bench_worker`);
        if (response.status === 200 && workers.length === condition.workers
          && log.includes(`using the "${condition.backend}" event method`)) {
          assert(state.pids.every(pid => counters.processes[pid]), 'missing nginx process');
          ready = true;
          break;
        }
      } catch (error) { /* Retry while workers initialize. */ }
      await delay(100);
    }
    assert(ready, `nginx startup or backend/worker verification failed: ${prefix}`);
    return state;
  } catch (error) {
    await stopNginx(settings, state).catch(() => {});
    throw error;
  }
}

async function stopNginx(settings, state) {
  // All clients have exited and their complete responses were validated.
  // Fast shutdown keeps idle TLS/upstream connections out of benchmark timing.
  const stop = await execute(settings.binary, [...state.args, '-s', 'stop'], 10000, state.prefix);
  let remaining = Object.keys(state.identities);
  for (let attempt = 0; attempt < 50; attempt++) {
    const current = await snapshot(settings, remaining);
    remaining = remaining.filter(pid => current.processes[pid]
      && current.processes[pid].created === state.identities[pid].created);
    if (!remaining.length && state.child.exitCode !== null) break;
    await delay(100);
  }
  if (remaining.length || state.child.exitCode === null) {
    const failures = [];
    for (const pid of remaining) {
      try {
        await checked(settings.helper, ['terminate', pid, state.identities[pid].created]);
      } catch (error) { failures.push(error.message); }
    }
    state.child.kill();
    throw new Error(`nginx did not stop cleanly: ${state.prefix}; ${failures.join('; ')}`);
  }
  assert(stop.code === 0 && !stop.timedOut, `nginx stop command failed: ${state.prefix}`);
  assert(state.child.exitCode === 0, `nginx exited with ${state.child.exitCode}: ${state.prefix}`);
  const log = fs.readFileSync(path.join(state.prefix, 'logs/error.log'), 'utf8');
  assert(!/\[(?:emerg|alert|crit|error)\]/.test(log), `nginx logged an error: ${state.prefix}`);
}

async function load(settings, url, seconds, file, bytes) {
  const result = await execute(settings.oha, [
    '-z', `${seconds}s`, '-c', String(settings.connections),
    '--worker-threads', '2', '--http-version', '1.1', '--ipv4',
    '--wait-ongoing-requests-after-deadline', '--no-tui',
    '--disable-compression', '--output-format', 'json',
    '--insecure', '-t', '5s',
    ...(settings.rate ? ['-q', String(settings.rate), '--latency-correction'] : []),
    url
  ], Math.max(15000, (seconds + 10) * 1000), undefined,
  settings.measureClient ? settings.helper : undefined);
  fs.writeFileSync(file, result.stdout);
  fs.writeFileSync(file + '.stderr', result.stderr);
  assert(result.code === 0 && !result.timedOut, `oha failed: ${file}: ${result.stderr}`);
  const sample = report.validateSample(JSON.parse(result.stdout), bytes);
  if (settings.measureClient) {
    assert(result.usage && result.usage.code === 0 && !result.usage.timedOut,
      `missing client CPU/affinity measurement: ${file}`);
    const usage = JSON.parse(result.usage.stdout);
    assert(usage.seconds > 0 && usage.cpuSeconds >= 0);
    assert(usage.affinity === settings.clientAffinity, 'client affinity changed');
    writeJson(file + '.usage.json', usage);
    sample.clientCpuCores = usage.cpuSeconds / usage.seconds;
  }
  return sample;
}

async function run(settings) {
  assert(process.platform === 'win32', 'run this benchmark with native Windows Node.js');
  assert(!fs.existsSync(settings.output) || !fs.readdirSync(settings.output).length,
    'output directory must be empty; do not mix benchmark campaigns');
  fs.mkdirSync(path.join(settings.output, 'data'), {recursive: true});
  const rawDirectory = path.join(settings.output, 'raw');
  fs.mkdirSync(rawDirectory);
  for (const size of report.sizes) {
    fs.writeFileSync(path.join(settings.output, 'data', size.file), Buffer.alloc(size.bytes, 0x61));
  }
  fs.writeFileSync(path.join(settings.output, 'openssl.cnf'), '[req]\ndistinguished_name=dn\n[dn]\n');
  await checked(settings.openssl, ['req', '-config', path.join(settings.output, 'openssl.cnf'),
    '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
    '-keyout', path.join(settings.output, 'key.pem'), '-out', path.join(settings.output, 'cert.pem')]);
  const version = await checked(settings.binary, ['-V']);
  assert(!version.stderr.includes('--with-debug'), 'benchmark binary must be optimized');
  const manifest = {
    schemaVersion: 1, kind: settings.kind, profile: settings.smoke ? 'smoke' : 'ci',
    suite: settings.suite, repeats: settings.repeats,
    sampleSeconds: settings.sampleSeconds, warmupSeconds: settings.warmupSeconds,
    connections: settings.connections, clientThreads: 2, multiWorkers: settings.multiWorkers,
    sourceCommit: process.env.GITHUB_SHA || process.env.BENCH_SOURCE_SHA || 'local',
    runUrl: process.env.GITHUB_RUN_ID ? `https://github.com/${process.env.GITHUB_REPOSITORY}/actions/runs/${process.env.GITHUB_RUN_ID}` : null,
    runAttempt: process.env.GITHUB_RUN_ATTEMPT || null,
    startedAt: new Date().toISOString(),
    runner: {os: os.type(), release: os.release(), arch: os.arch(),
      logicalProcessors: os.cpus().length, cpu: os.cpus()[0].model,
      totalMemoryBytes: os.totalmem(), node: process.version},
    binarySha256: hash(fs.readFileSync(settings.binary)), nginxVersion: version.stderr.trim(),
    ohaSha256: hash(fs.readFileSync(settings.oha)),
    ohaVersion: (await checked(settings.oha, ['--version'])).stdout.trim(),
    helperSha256: hash(fs.readFileSync(settings.helper)),
    tls: 'TLSv1.3 / TLS_AES_128_GCM_SHA256; client-facing only',
    proxy: {transport: 'HTTP/1.1', buffering: false, body: 'memory', slowDelayMs: 20},
    policy: report.policy
  };
  writeJson(path.join(settings.output, 'manifest.json'), manifest);
  const rows = report.cases(settings.kind, settings.multiWorkers);
  const probes = new Set();
  let ordinal = 0, upstream;
  try {
    if (settings.kind === 'proxy') upstream = await startUpstream(settings);
    for (let round = 0; round < settings.repeats; round++) {
      for (const condition of report.conditionOrder(round, settings.multiWorkers)) {
        const index = [1, settings.multiWorkers].indexOf(condition.workers) * 3
          + report.backends.indexOf(condition.backend);
        const group = rows.filter(row => row.backend === condition.backend
          && row.workers === condition.workers);
        const nginx = await startNginx(settings, condition, round, upstream && upstream.port);
        try {
          for (const row of report.workloadOrder(group, round, index)) {
            const url = `${row.tls ? 'https' : 'http'}://127.0.0.1:${nginx.ports[row.tls ? 1 : 0]}/${row.mode}/${row.file}`;
            const stem = path.join(rawDirectory, `r${round + 1}-${row.id}`);
            if (!probes.has(row.id)) {
              const probe = await request(url);
              assert(probe.status === 200
                && probe.body.equals(Buffer.alloc(row.bytes, 0x61)), `invalid body: ${row.id}`);
              assert(!row.tls || probe.protocol === 'TLSv1.3', 'unexpected TLS protocol');
              fs.appendFileSync(path.join(settings.output, 'probes.jsonl'), JSON.stringify({
                id: row.id, bytes: probe.body.length, sha256: hash(probe.body), protocol: probe.protocol
              }) + '\n');
              probes.add(row.id);
            }
            const idle = await idleCpu(settings);
            await load(settings, url, settings.warmupSeconds, stem + '-warmup.json', row.bytes);
            const pids = [...nginx.pids, ...(upstream ? [upstream.child.pid] : [])];
            const before = await snapshot(settings, pids);
            const result = await load(settings, url, settings.sampleSeconds, stem + '.json', row.bytes);
            const after = await snapshot(settings, pids);
            const sample = {id: row.id, round, ordinal: ordinal++, ...result,
              idleCpuPercent: idle, hostCpuPercent: hostCpu(before, after),
              nginxCpuCores: processCpu(before, after, nginx.pids),
              upstreamCpuCores: upstream ? processCpu(before, after, [upstream.child.pid]) : null,
              counters: {before, after}, rawFile: path.relative(settings.output, stem + '.json').replace(/\\/g, '/')};
            fs.appendFileSync(path.join(settings.output, 'samples.jsonl'), JSON.stringify(sample) + '\n');
            process.stdout.write(`round ${round + 1}/${settings.repeats} ${row.id}: `
              + `${result.requestsPerSecond.toFixed(1)} req/s; idle CPU ${idle.toFixed(1)}%\n`);
          }
        } finally {
          await stopNginx(settings, nginx);
        }
      }
    }
    manifest.completedAt = new Date().toISOString();
    writeJson(path.join(settings.output, 'manifest.json'), manifest);
    const summary = report.main(settings.output);
    process.stdout.write(`Completed ${summary.totalSamples} measured samples, all HTTP 200 with correct body sizes.\n`);
  } catch (error) {
    fs.writeFileSync(path.join(settings.output, 'failure.txt'), error.stack + '\n');
    throw error;
  } finally {
    if (upstream) upstream.close();
    // The temporary TLS key is not useful as an uploaded benchmark artifact.
    fs.rmSync(path.join(settings.output, 'key.pem'), {force: true});
  }
}

if (require.main === module) {
  run(options(process.argv.slice(2))).catch(error => {
    process.stderr.write(`${error.stack}\n`);
    process.exitCode = 1;
  });
}

module.exports = {options, configuration, run, execute, checked, request,
  snapshot, hostCpu, processCpu, idleCpu, startUpstream, startNginx,
  stopNginx, load};

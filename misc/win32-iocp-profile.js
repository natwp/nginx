'use strict';

// Diagnostic call counts only. Instrumented rates are not acceptance results.
const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const os = require('os');
const path = require('path');
const bench = require('./win32-ci-benchmark');

const write = (file, value) => fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n');
const hash = data => crypto.createHash('sha256').update(data).digest('hex');

async function run() {
  assert(process.platform === 'win32' && process.env.GITHUB_ACTIONS === 'true',
    'profiles must run on native Windows in GitHub Actions');
  const output = path.resolve('profile-results');
  assert(!fs.existsSync(output), 'profile-results must not already exist');
  fs.mkdirSync(output);
  const binary = path.resolve('benchmark-bin/nginx.exe');
  const common = {binary, oha: path.resolve('benchmark-bin/oha.exe'),
    helper: path.resolve('benchmark-bin/benchmark-helper.exe'),
    openssl: path.resolve('benchmark-bin/openssl.exe'), connections: 32,
    measureClient: true};
  const original = await bench.snapshot(common, [process.pid]);
  common.clientAffinity = original.processes[process.pid].affinity;
  fs.writeFileSync(path.join(output, 'openssl.cnf'), '[req]\ndistinguished_name=dn\n[dn]\n');
  await bench.checked(common.openssl, ['req', '-config', path.join(output, 'openssl.cnf'),
    '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
    '-keyout', path.join(output, 'key.pem'), '-out', path.join(output, 'cert.pem')]);
  const cases = [
    {kind: 'static', mode: 'off', file: '1k.bin', bytes: 1024, tls: false},
    {kind: 'static', mode: 'on', file: '1k.bin', bytes: 1024, tls: false},
    {kind: 'static', mode: 'off', file: '1k.bin', bytes: 1024, tls: true},
    {kind: 'proxy', mode: 'fast', file: '1k.bin', bytes: 1024, tls: false},
    {kind: 'proxy', mode: 'fast', file: '1k.bin', bytes: 1024, tls: true},
    {kind: 'proxy', mode: 'fast', file: '1m.bin', bytes: 1048576, tls: false},
    {kind: 'proxy', mode: 'fast', file: '1m.bin', bytes: 1048576, tls: true}
  ];
  const manifest = {source: process.env.GITHUB_SHA,
    purpose: 'Instrumented call counts; not a throughput comparison or retention gate',
    binarySha256: hash(fs.readFileSync(binary)),
    nginxVersion: (await bench.checked(binary, ['-V'])).stderr,
    runner: {cpu: os.cpus()[0].model, cpus: os.cpus().length,
      release: os.release(), node: process.version},
    connections: common.connections, sampleSeconds: 5, warmupSeconds: 1,
    results: []};
  write(path.join(output, 'manifest.json'), manifest);
  try {
    for (const row of cases) {
      for (const backend of ['iocp', 'select', 'poll']) {
        const id = `${row.kind}-${row.mode}-${row.file}-${row.tls ? 'tls' : 'plain'}-${backend}`;
        const directory = path.join(output, id);
        fs.mkdirSync(path.join(directory, 'data'), {recursive: true});
        for (const [name, bytes] of [['1k.bin', 1024], ['1m.bin', 1048576]]) {
          fs.writeFileSync(path.join(directory, 'data', name), Buffer.alloc(bytes, 0x61));
        }
        for (const name of ['key.pem', 'cert.pem']) {
          fs.copyFileSync(path.join(output, name), path.join(directory, name));
        }
        const settings = {...common, output: directory, kind: row.kind};
        process.env.NGX_GPROF_DIR = path.join(directory, 'gprof');
        let nginx, upstream;
        const result = {id, backend, ...row};
        try {
          if (row.kind === 'proxy') upstream = await bench.startUpstream(settings);
          nginx = await bench.startNginx(settings, {backend, workers: 1}, 0,
            upstream && upstream.port);
          const url = `${row.tls ? 'https' : 'http'}://127.0.0.1:${nginx.ports[row.tls ? 1 : 0]}/${row.mode}/${row.file}`;
          const probe = await bench.request(url);
          assert(probe.status === 200 && probe.body.equals(Buffer.alloc(row.bytes, 0x61)));
          assert(!row.tls || probe.protocol === 'TLSv1.3');
          await bench.load(settings, url, 1, path.join(directory, 'warmup.json'), row.bytes);
          const pids = [...nginx.pids, ...(upstream ? [upstream.child.pid] : [])];
          const before = await bench.snapshot(settings, pids);
          result.sample = await bench.load(settings, url, 5,
            path.join(directory, 'measurement.json'), row.bytes);
          const after = await bench.snapshot(settings, pids);
          result.nginxCpuCores = bench.processCpu(before, after, nginx.pids);
          result.upstreamCpuCores = upstream
            ? bench.processCpu(before, after, [upstream.child.pid]) : null;
          result.counters = {before, after};
        } catch (error) {
          result.error = error.stack;
        } finally {
          if (nginx) {
            try { await bench.stopNginx(settings, nginx); }
            catch (error) { result.stopError = error.stack; }
          }
          if (upstream) upstream.close();
          fs.rmSync(path.join(directory, 'key.pem'), {force: true});
        }
        const profiles = path.join(directory, 'gprof');
        result.profiles = fs.existsSync(profiles) ? fs.readdirSync(profiles)
          .filter(name => fs.existsSync(path.join(profiles, name, 'gmon.out'))) : [];
        if (!result.profiles.some(name => name.startsWith('worker-'))) {
          result.profileError = 'missing worker gmon.out';
        }
        manifest.results.push(result);
        write(path.join(output, 'manifest.json'), manifest);
        process.stdout.write(`${id}: ${result.error || result.stopError || result.profileError || 'profile saved'}\n`);
      }
    }
  } finally {
    fs.rmSync(path.join(output, 'key.pem'), {force: true});
    delete process.env.NGX_GPROF_DIR;
  }
  assert(manifest.results.length === cases.length * 3);
  assert(manifest.results.every(result => !result.error && !result.stopError && !result.profileError),
    'one or more diagnostic profiles failed; inspect the preserved results');
}

if (require.main === module) {
  run().catch(error => { process.stderr.write(error.stack + '\n'); process.exitCode = 1; });
}

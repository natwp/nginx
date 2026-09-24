'use strict';

const assert = require('assert');
const base = require('./benchmark-report');
const report = require('./iocp-performance-report');
const runner = require('./win32-iocp-compare');

function fixture(kind = 'static', candidateRatio = 1.06) {
  const manifest = {schemaVersion: 1, completedAt: '2026-09-24T00:00:00Z',
    kind, profile: 'isolated', multiWorkers: 2, repeats: 6, sampleSeconds: 2,
    clientCpuBudget: 2, upstreamCpuBudget: 1, policy: base.policy,
    binaries: {'baseline-a': {sha256: 'same'}, 'baseline-b': {sha256: 'same'},
      candidate: {sha256: 'changed'}}};
  const samples = [];
  const rows = report.cases(manifest);
  for (let round = 0; round < manifest.repeats; round++) {
    for (const [index, row] of rows.entries()) {
      for (const variant of report.order(round, index)) {
        samples.push({id: row.id, variant, round, ordinal: samples.length,
          requests: 1000, bytes: 1000 * row.bytes, seconds: 2,
          requestsPerSecond: 500 * (1 + round / 100)
            * (variant === 'candidate' ? candidateRatio : 1),
          idleCpuPercent: 1, clientCpuCores: 0.2, nginxCpuCores: 0.5,
          upstreamCpuCores: kind === 'proxy' ? 0.1 : null});
      }
    }
  }
  return {manifest, samples};
}

function check(data) { return report.summarize(data.manifest, data.samples); }

for (const kind of ['static', 'proxy']) {
  const data = fixture(kind);
  const result = check(data);
  assert.strictEqual(result.totalSamples, 288);
  assert.strictEqual(result.decision, 'retain');
  assert(result.rows.every(row => row.decision === 'improvement'));
  assert(result.rows.every(row => row.comparisons.every(c => Math.abs(c.deltaPercent - 6) < 1e-8)));
  assert.strictEqual(check(fixture(kind, 1)).decision, 'no improvement');
  assert.strictEqual(check(fixture(kind, 0.95)).decision, 'reject');
}

// A label bias between identical controls invalidates an apparent win.
{
  const data = fixture();
  for (const sample of data.samples.filter(s => s.variant === 'baseline-b')) {
    sample.requestsPerSecond *= 1.03;
  }
  const result = check(data);
  assert.strictEqual(result.decision, 'inconclusive');
  assert(result.rows.every(row => row.reasons.some(reason => reason.includes('A/A'))));
}

// Large warm-host noise can be excluded symmetrically; it is never missing data.
{
  const data = fixture();
  for (const sample of data.samples.filter(s => s.round === 0)) sample.idleCpuPercent = 90;
  assert.strictEqual(check(data).decision, 'retain');
  for (const sample of data.samples.filter(s => s.round < 3)) sample.idleCpuPercent = 90;
  assert.strictEqual(check(data).decision, 'inconclusive');
}

// A single outlier in either direction does not change a reproducible result.
for (const factor of [0.4, 2.5]) {
  const data = fixture();
  const sample = data.samples.find(s => s.variant === 'candidate');
  sample.requestsPerSecond *= factor;
  const result = check(data);
  assert.strictEqual(result.decision, 'retain');
  assert(result.rows.find(row => row.id === sample.id).comparisons.every(c => c.retained === 5));
}

for (const key of ['clientCpuCores', 'upstreamCpuCores']) {
  const data = fixture('proxy');
  for (const sample of data.samples) sample[key] = key === 'clientCpuCores' ? 1.9 : 0.9;
  assert.strictEqual(check(data).decision, 'inconclusive');
}

// Good medians cannot hide a loss in the reverse execution order.
{
  const data = fixture();
  for (const sample of data.samples.filter(s => s.variant === 'candidate')) {
    const control = data.samples.find(s => s.id === sample.id && s.round === sample.round
      && s.variant === 'baseline-a');
    sample.requestsPerSecond = control.requestsPerSecond * (sample.ordinal < control.ordinal ? 1.1 : 0.95);
  }
  assert.strictEqual(check(data).decision, 'inconclusive');
}

for (const mutate of [
  data => data.samples.pop(),
  data => data.samples.push({...data.samples[0]}),
  data => { data.samples[0].bytes--; },
  data => { data.samples[0].ordinal = 9999; },
  data => { [data.samples[0].ordinal, data.samples[1].ordinal]
    = [data.samples[1].ordinal, data.samples[0].ordinal]; },
  data => { delete data.samples[0].clientCpuCores; },
  data => { data.manifest.binaries['baseline-b'].sha256 = 'different'; },
  data => { data.manifest.policy = {...base.policy, maximumMadPercent: 99}; },
  data => { data.manifest.repeats = 5; },
  data => { delete data.manifest.completedAt; }
]) {
  const data = fixture();
  mutate(data);
  assert.throws(() => check(data));
}

const staticCpus = runner.cpuPlan(15n, 'static', 'isolated');
const proxyCpus = runner.cpuPlan(15n, 'proxy', 'isolated');
assert.strictEqual(staticCpus.client & staticCpus.nginx, 0n);
assert.strictEqual(proxyCpus.client & proxyCpus.nginx, 0n);
assert.strictEqual(proxyCpus.upstream & (proxyCpus.client | proxyCpus.nginx), 0n);
assert.strictEqual(staticCpus.multiWorkers, 2);
assert.strictEqual(runner.cpuPlan(15n, 'static', 'shared').multiWorkers, 4);
assert.throws(() => runner.cpuPlan(7n, 'proxy', 'isolated'));

{
  const reports = ['static', 'proxy'].flatMap(kind => ['isolated', 'shared'].map(profile => {
    const data = fixture(kind);
    data.manifest.profile = profile;
    return check(data);
  }));
  assert.strictEqual(report.gate(reports), 'retain');
  assert.throws(() => report.gate(reports.slice(1)));
  reports[0].decision = 'inconclusive';
  assert.strictEqual(report.gate(reports), 'inconclusive');
  reports[1].decision = 'reject';
  assert.strictEqual(report.gate(reports), 'reject');
  for (const row of reports) row.decision = 'no improvement';
  reports[1].decision = 'retain';
  assert.strictEqual(report.gate(reports), 'no isolated improvement');
}

process.stdout.write('IOCP comparison coverage, A/A, order, resource, and retention gates passed\n');

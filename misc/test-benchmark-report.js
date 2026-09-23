'use strict';

const assert = require('assert');
const report = require('./benchmark-report');

function raw(overrides = {}) {
  return {
    summary: {requestsPerSec: 100, total: 2, successRate: 1,
      totalData: 200 * 1024, sizePerRequest: 1024, ...overrides},
    statusCodeDistribution: {'200': 200}, errorDistribution: {},
    latencyPercentiles: {p50: 0.01, p95: 0.02}
  };
}

assert.equal(report.validateSample(raw(), 1024).requests, 200);
for (const change of [
  {successRate: 0.99}, {total: 0}, {total: NaN}, {requestsPerSec: 101},
  {totalData: 200 * 1024 - 1}, {sizePerRequest: 1}, {errors: 1}
]) assert.throws(() => report.validateSample(raw(change), 1024));
assert.throws(() => report.validateSample({...raw(),
  errorDistribution: {'connection reset': 1}}, 1024));
assert.throws(() => report.validateSample({...raw(),
  statusCodeDistribution: {'500': 200}}, 1024));
assert.throws(() => report.validateSample({...raw(),
  latencyPercentiles: {p50: 0.03, p95: 0.02}}, 1024));

const conditions = report.conditionOrder(0);
const key = row => `${row.backend}-${row.workers}`;
for (let position = 0; position < 6; position++) {
  assert.equal(new Set(Array.from({length: 6}, (_, round) =>
    key(report.conditionOrder(round)[position]))).size, 6);
}
for (const a of conditions) {
  for (const b of conditions.filter(row => key(row) !== key(a))) {
    const before = Array.from({length: 6}, (_, round) => report.conditionOrder(round))
      .filter(rows => rows.findIndex(row => key(row) === key(a))
        < rows.findIndex(row => key(row) === key(b))).length;
    assert.equal(before, 3, `unbalanced condition order: ${key(a)}, ${key(b)}`);
  }
}

const logs = values => values.map((value, round) => ({round, value: Math.log(value)}));
const filtered = report.filterLogs(logs([100, 100, 100, 100, 10, 1000]), 6);
assert.equal(filtered.status, 'stable');
assert.equal(filtered.retained, 4);
assert(Math.abs(filtered.median - 100) < 1e-9);
assert.deepEqual(filtered.excluded.map(row => row.round), [4, 5]);
assert.equal(report.filterLogs(logs([60, 65, 70, 130, 135, 140]), 6).status, 'inconclusive');
assert.equal(report.filterLogs(logs([100, 100, 100]), 6).status, 'inconclusive');
assert.equal(report.filterLogs(logs([1, 1, 1, 1, 1, 1]).map(row =>
  ({...row, exclusion: 'host busy before sample'})), 6).retained, 0);
const orderSensitive = logs([1, 1, 1, 1.06, 1.06, 1.06])
  .map(row => ({...row, first: row.round < 3}));
assert(report.filterLogs(orderSensitive, 6, true).reasons
  .includes('execution-order difference exceeds 5%'));
const ratios = logs([1.09, 1.10, 1.11, 1.10, 0.1, 10])
  .map(row => ({...row, first: row.round % 2 === 0}));
const forward = report.filterLogs(ratios, 6, true);
const reverse = report.filterLogs(ratios.map(row =>
  ({...row, value: -row.value, first: !row.first})), 6, true);
assert.equal(forward.retained, reverse.retained);
assert(Math.abs(forward.median * reverse.median - 1) < 1e-10);

function fixture(kind, repeats = 6) {
  const manifest = {schemaVersion: 1, kind, multiWorkers: 4, repeats,
    profile: repeats === 1 ? 'smoke' : 'ci', sampleSeconds: 2};
  const cases = report.cases(kind);
  assert.equal(cases.length, 48);
  const samples = [];
  for (let round = 0; round < repeats; round++) {
    for (const condition of report.conditionOrder(round)) {
      const index = [1, 4].indexOf(condition.workers) * 3
        + report.backends.indexOf(condition.backend);
      const group = cases.filter(row => row.backend === condition.backend
        && row.workers === condition.workers);
      for (const row of report.workloadOrder(group, round, index)) {
        const requestsPerSecond = 100 * (row.backend === 'iocp' ? 1.1 : 1)
          * (row.workers === 4 ? 2 : 1) * (row.tls ? 0.8 : 1)
          * (row.mode === 'on' ? 1.1 : row.mode === 'slow' ? 0.5 : 1)
          * [1, 1.005, 0.995, 1.008, 1.002, 0.998][round % 6];
        const requests = Math.round(requestsPerSecond * 2);
        samples.push({id: row.id, round, ordinal: samples.length,
          requestsPerSecond, requests, bytes: requests * row.bytes,
          seconds: requests / requestsPerSecond, idleCpuPercent: 2,
          p95Milliseconds: 10, nginxCpuCores: 1});
      }
    }
  }
  return {manifest, samples};
}

for (const kind of ['static', 'proxy']) {
  const {manifest, samples} = fixture(kind);
  const summary = report.summarize(manifest, samples);
  assert.equal(summary.totalSamples, 288);
  assert.equal(summary.cells.length, 48);
  assert.equal(summary.comparisons.length, 104);
  assert(summary.cells.every(row => row.status === 'stable'));
  assert(summary.comparisons.every(row => row.status === 'stable'));
  for (const row of summary.comparisons.filter(row => row.dimension === 'workers')) {
    assert(Math.abs(row.deltaPercent - 100) < 1e-8);
  }
  for (const row of summary.comparisons.filter(row => row.dimension === 'tls')) {
    assert(Math.abs(row.deltaPercent + 20) < 1e-8);
  }
  assert.throws(() => report.summarize(manifest, samples.slice(1)), /missing samples/);
  assert.throws(() => report.summarize(manifest, [...samples, samples[0]]), /duplicate/);
  assert.throws(() => report.summarize(manifest, [
    {...samples[0], bytes: samples[0].bytes - 1}, ...samples.slice(1)
  ]), /response count/);
  const busy = samples.map(row => ({...row, idleCpuPercent: 99}));
  assert(report.summarize(manifest, busy).cells.every(row => row.status === 'inconclusive'));
  const smoke = fixture(kind, 1);
  assert.equal(report.summarize(smoke.manifest, smoke.samples).publishable, false);
}

// Common host drift can cancel in a paired ratio. Do not divide separately
// filtered cell medians or discard pairs according to the desired winner.
const drift = fixture('static');
const factors = [0.7, 1, 1.8, 0.8, 1.5, 1.1];
for (const row of drift.samples) {
  row.requestsPerSecond *= factors[row.round];
}
const driftReport = report.summarize(drift.manifest, drift.samples);
assert(driftReport.cells.some(row => row.status === 'inconclusive'));
assert(driftReport.comparisons.every(row => row.status === 'stable'));

const group = report.cases('static').filter(row => row.backend === 'iocp' && row.workers === 1);
for (const a of group) {
  for (const b of group.filter(row => row.id !== a.id)) {
    const before = Array.from({length: 6}, (_, round) => report.workloadOrder(group, round, 0))
      .filter(rows => rows.indexOf(a) < rows.indexOf(b)).length;
    assert.equal(before, 3, 'unbalanced workload order');
  }
}

assert.throws(() => report.cases('static', 1));
assert.throws(() => report.cases('unknown'));
process.stdout.write('Benchmark coverage, correctness validation, pairing, and noise-filter tests passed.\n');

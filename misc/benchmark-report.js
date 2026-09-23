'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const {validateResult} = require('./validate-oha-summary');

const backends = ['iocp', 'select', 'poll'];
const sizes = [{file: '1k.bin', bytes: 1024},
  {file: '1m.bin', bytes: 1048576}];
const policy = Object.freeze({
  floorPercent: 3,
  madMultiplier: 3,
  madScale: 1.4826,
  minimumRetainedFraction: 2 / 3,
  minimumRetained: 4,
  maximumMadPercent: 5,
  maximumOrderDifferencePercent: 5,
  maximumIdleCpuPercent: 20,
  maximumDrainFraction: 0.25
});

function caseId(row) {
  return [row.kind, row.mode, row.file, row.tls ? 'tls' : 'plain',
    `w${row.workers}`, row.backend].join('-');
}

function cases(kind, multiWorkers = 4) {
  assert(['static', 'proxy'].includes(kind), 'invalid matrix kind');
  assert(Number.isInteger(multiWorkers) && multiWorkers >= 2
    && multiWorkers <= 8, 'invalid multi-worker count');
  const result = [];
  for (const backend of backends) {
    for (const workers of [1, multiWorkers]) {
      for (const tls of [false, true]) {
        for (const mode of kind === 'static' ? ['off', 'on'] : ['fast', 'slow']) {
          for (const size of sizes) {
            const row = {kind, backend, workers, tls, mode, ...size};
            result.push({...row, id: caseId(row)});
          }
        }
      }
    }
  }
  return result;
}

// Williams order: every configuration occupies every position once, with
// each pair measured in both orders equally often over six rounds.
function conditionOrder(round, multiWorkers = 4) {
  const conditions = [1, multiWorkers].flatMap(workers =>
    backends.map(backend => ({backend, workers})));
  return [0, 1, 5, 2, 4, 3].map(index => conditions[(index + round) % 6]);
}

function workloadOrder(rows, round, conditionIndex) {
  const offset = (Math.floor(round / 2) + conditionIndex) % rows.length;
  const rotated = [...rows.slice(offset), ...rows.slice(0, offset)];
  return round % 2 ? rotated.reverse() : rotated;
}

function median(values) {
  assert(values.length, 'cannot take an empty median');
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

function validateSample(raw, bytes) {
  const summary = validateResult(raw, 200);
  const distribution = summary.statusCodeDistribution || raw.statusCodeDistribution;
  const requests = distribution['200'];
  assert(Number.isFinite(summary.total) && summary.total > 0,
    'missing elapsed time');
  assert(summary.totalData === requests * bytes
    && summary.sizePerRequest === bytes, 'wrong response body size');
  assert(Math.abs(summary.requestsPerSec - requests / summary.total)
    / summary.requestsPerSec < 0.001, 'inconsistent request rate');
  const p50 = raw.latencyPercentiles && raw.latencyPercentiles.p50;
  const p95 = raw.latencyPercentiles && raw.latencyPercentiles.p95;
  assert(Number.isFinite(p50) && p50 >= 0
    && Number.isFinite(p95) && p95 >= p50, 'invalid latency percentiles');
  return {
    requestsPerSecond: summary.requestsPerSec,
    bytesPerSecond: summary.totalData / summary.total,
    requests, bytes: summary.totalData, seconds: summary.total,
    p50Milliseconds: p50 * 1000, p95Milliseconds: p95 * 1000
  };
}

function filterLogs(entries, rawCount, paired = false) {
  const required = Math.max(policy.minimumRetained,
    Math.ceil(rawCount * policy.minimumRetainedFraction));
  const usable = entries.filter(row => !row.exclusion);
  const excluded = entries.filter(row => row.exclusion)
    .map(row => ({round: row.round, reason: row.exclusion}));
  if (!usable.length) {
    return {status: 'inconclusive', raw: rawCount, retained: 0,
      excluded, reasons: ['no eligible samples'], median: null,
      rounds: [], madPercent: null, orderDifferencePercent: null,
      minimum: null, maximum: null, filteredMedian: null};
  }
  assert(usable.every(row => Number.isFinite(row.value)), 'invalid log sample');
  const center = median(usable.map(row => row.value));
  const mad = median(usable.map(row => Math.abs(row.value - center)));
  const limit = Math.max(Math.log1p(policy.floorPercent / 100),
    policy.madMultiplier * policy.madScale * mad);
  const kept = usable.filter(row => Math.abs(row.value - center) <= limit);
  for (const row of usable.filter(row => Math.abs(row.value - center) > limit)) {
    excluded.push({round: row.round, reason: 'MAD outlier',
      value: Math.exp(row.value)});
  }
  const reasons = [];
  if (kept.length < required) reasons.push(`fewer than ${required} samples retained`);
  const estimate = kept.length ? median(kept.map(row => row.value)) : center;
  const dispersion = kept.length ? Math.expm1(policy.madScale
    * median(kept.map(row => Math.abs(row.value - estimate)))) * 100 : null;
  if (dispersion === null || dispersion > policy.maximumMadPercent) {
    reasons.push('retained dispersion exceeds 5%');
  }
  let orderDifferencePercent = null;
  if (paired) {
    const first = kept.filter(row => row.first).map(row => row.value);
    const last = kept.filter(row => !row.first).map(row => row.value);
    if (first.length < 2 || last.length < 2) {
      reasons.push('fewer than two retained pairs in each execution order');
    } else {
      orderDifferencePercent = Math.expm1(Math.abs(median(first) - median(last))) * 100;
      if (orderDifferencePercent > policy.maximumOrderDifferencePercent) {
        reasons.push('execution-order difference exceeds 5%');
      }
    }
  }
  return {
    status: reasons.length ? 'inconclusive' : 'stable',
    raw: rawCount, retained: kept.length,
    rounds: kept.map(row => row.round), excluded, reasons,
    median: reasons.length ? null : Math.exp(estimate),
    filteredMedian: Math.exp(estimate),
    madPercent: dispersion, orderDifferencePercent,
    minimum: kept.length ? Math.exp(Math.min(...kept.map(row => row.value))) : null,
    maximum: kept.length ? Math.exp(Math.max(...kept.map(row => row.value))) : null
  };
}

function sampleExclusion(row, manifest) {
  if (row.idleCpuPercent > policy.maximumIdleCpuPercent) {
    return 'host busy before sample';
  }
  if (row.seconds > manifest.sampleSeconds * (1 + policy.maximumDrainFraction) + 0.1) {
    return 'deadline drain exceeded 25%';
  }
  return null;
}

function summarize(manifest, samples) {
  assert(manifest.schemaVersion === 1, 'unsupported benchmark schema');
  const plan = cases(manifest.kind, manifest.multiWorkers);
  const expected = new Map(plan.map(row => [row.id, row]));
  const byCase = new Map(plan.map(row => [row.id, new Map()]));
  assert(Number.isInteger(manifest.repeats) && manifest.repeats > 0);
  assert(manifest.profile === 'smoke' || manifest.repeats >= 6);
  assert(Number.isFinite(manifest.sampleSeconds) && manifest.sampleSeconds > 0);
  for (const row of samples) {
    assert(expected.has(row.id), `unexpected case ${row.id}`);
    assert(Number.isInteger(row.round) && row.round >= 0
      && row.round < manifest.repeats, 'invalid round');
    assert(!byCase.get(row.id).has(row.round), `duplicate sample ${row.id}`);
    assert(Number.isFinite(row.requestsPerSecond) && row.requestsPerSecond > 0);
    assert(Number.isInteger(row.requests) && row.requests > 0
      && row.bytes === row.requests * expected.get(row.id).bytes,
    'invalid normalized response count');
    assert(Number.isFinite(row.idleCpuPercent) && row.idleCpuPercent >= 0
      && row.idleCpuPercent <= 100, 'missing host CPU sample');
    assert(Number.isFinite(row.seconds) && row.seconds > 0);
    assert(Number.isInteger(row.ordinal) && row.ordinal >= 0);
    byCase.get(row.id).set(row.round, {...row,
      exclusion: sampleExclusion(row, manifest)});
  }
  const cells = plan.map(row => {
    const samples = [...byCase.get(row.id).values()];
    assert(samples.length === manifest.repeats, `missing samples for ${row.id}`);
    const result = filterLogs(samples.map(sample => ({...sample,
      value: Math.log(sample.requestsPerSecond)})), manifest.repeats);
    const retained = samples.filter(sample => result.rounds
      && result.rounds.includes(sample.round));
    return {...row, ...result, requestsPerSecond: result.median,
      p95Milliseconds: retained.length
        ? median(retained.map(sample => sample.p95Milliseconds)) : null,
      nginxCpuCores: retained.length
        ? median(retained.map(sample => sample.nginxCpuCores)) : null};
  });
  const comparisons = [];
  const compare = (dimension, candidate, control) => {
    const left = byCase.get(candidate.id);
    const right = byCase.get(control.id);
    assert(right, `missing comparison control ${control.id}`);
    const pairs = [...left].map(([round, a]) => {
      const b = right.get(round);
      return {round, value: Math.log(a.requestsPerSecond / b.requestsPerSecond),
        first: a.ordinal < b.ordinal, exclusion: a.exclusion || b.exclusion};
    });
    const result = filterLogs(pairs, manifest.repeats, true);
    comparisons.push({dimension, candidate: candidate.id, control: control.id,
      ...result, deltaPercent: result.median === null ? null : (result.median - 1) * 100});
  };
  for (const row of plan) {
    const control = changes => {
      const value = {...row, ...changes};
      return {...value, id: caseId(value)};
    };
    if (row.backend === 'iocp') {
      for (const backend of ['select', 'poll']) compare('backend', row, control({backend}));
    }
    if (row.workers === manifest.multiWorkers) compare('workers', row, control({workers: 1}));
    if (row.tls) compare('tls', row, control({tls: false}));
    if (row.mode === 'on') compare('sendfile', row, control({mode: 'off'}));
    if (row.mode === 'slow') compare('upstream delay', row, control({mode: 'fast'}));
  }
  return {schemaVersion: 1, manifest, policy,
    publishable: manifest.profile !== 'smoke',
    totalSamples: samples.length, cells, comparisons};
}

function rateCell(row) {
  return row.status === 'stable'
    ? `${row.requestsPerSecond.toFixed(0)} (${row.retained}/${row.raw})`
    : `inconclusive (${row.retained}/${row.raw})`;
}

function table(report) {
  const rows = [
    '| Mode | Size | TLS | Workers | IOCP req/s | Select req/s | Poll req/s |',
    '|---|---|---|---:|---:|---:|---:|'
  ];
  for (const mode of report.manifest.kind === 'static' ? ['off', 'on'] : ['fast', 'slow']) {
    for (const size of sizes) {
      for (const tls of [false, true]) {
        for (const workers of [1, report.manifest.multiWorkers]) {
          const group = backends.map(backend => report.cells.find(row =>
            row.mode === mode && row.file === size.file && row.tls === tls
            && row.workers === workers && row.backend === backend));
          rows.push(`| ${mode} | ${size.bytes === 1024 ? '1 KiB' : '1 MiB'} | `
            + `${tls ? 'on' : 'off'} | ${workers} | ${group.map(rateCell).join(' | ')} |`);
        }
      }
    }
  }
  return rows.join('\n');
}

function markdown(report) {
  const {manifest} = report;
  const rows = [
    `# ${manifest.kind === 'static' ? 'Static files' : 'HTTP proxy'} CI benchmark`, '',
    `Source: \`${manifest.sourceCommit}\`. Run: ${manifest.runUrl || 'local smoke test'}.`, '',
    `${manifest.repeats} rounds; ${manifest.sampleSeconds}s measurements; `
      + `${manifest.warmupSeconds}s warm-ups; ${manifest.connections} HTTP/1.1 keepalive connections.`, '',
    'Rates are geometric medians after symmetric filtering; counts are retained/raw.',
    'Inconclusive rows do not support a throughput claim. Raw samples remain in the artifact.', '',
    table(report), '', '## Paired comparisons', '',
    'Each change is the median ratio within matched rounds, not a ratio of the table medians.', '',
    '| Dimension | Candidate | Control | Change | Pairs | MAD |',
    '|---|---|---|---:|---:|---:|'
  ];
  for (const row of report.comparisons) {
    rows.push(`| ${row.dimension} | ${row.candidate} | ${row.control} | `
      + `${row.status === 'stable' ? `${row.deltaPercent > 0 ? '+' : ''}${row.deltaPercent.toFixed(2)}%` : 'inconclusive'} | `
      + `${row.retained}/${row.raw} | ${row.madPercent === null ? 'n/a' : row.madPercent.toFixed(2) + '%'} |`);
  }
  rows.push('', '## Filter and limitations', '',
    'The policy is fixed before measurement: reject log-rate or paired log-ratio',
    'outliers beyond max(log(1.03), 3 × 1.4826 × MAD). Require at least four',
    'and two-thirds of the planned samples, with retained scaled MAD ≤ 5%.',
    'Comparisons also require two retained pairs in each execution order and',
    'an order difference ≤ 5%. Busy-host and excessive-drain exclusions are',
    'recorded separately. HTTP errors or wrong body sizes fail the benchmark.', '',
    'This is one shared Windows runner per workload family. The client, nginx,',
    'and proxy upstream share its CPUs; TLS applies to the client-facing connection.',
    'Proxy bodies are generated in memory; slow responses add 20 ms before headers.',
    'These measurements describe this workload and runner, not a general speedup.', '');
  return rows.join('\n');
}

function main(directory) {
  const manifest = JSON.parse(fs.readFileSync(path.join(directory, 'manifest.json'), 'utf8'));
  const samples = fs.readFileSync(path.join(directory, 'samples.jsonl'), 'utf8')
    .trim().split(/\r?\n/).filter(Boolean).map(line => JSON.parse(line));
  const report = summarize(manifest, samples);
  fs.writeFileSync(path.join(directory, 'summary.json'), JSON.stringify(report, null, 2) + '\n');
  fs.writeFileSync(path.join(directory, 'summary.md'), markdown(report));
  fs.writeFileSync(path.join(directory, 'readme-table.md'), table(report) + '\n');
  return report;
}

if (require.main === module) {
  try {
    assert(process.argv.length === 3, 'usage: benchmark-report.js results-directory');
    const report = main(process.argv[2]);
    process.stdout.write(`${report.totalSamples} samples; `
      + `${report.cells.filter(row => row.status === 'stable').length}/${report.cells.length} stable cells\n`);
  } catch (error) {
    process.stderr.write(`${error.stack}\n`);
    process.exitCode = 1;
  }
}

module.exports = {backends, sizes, policy, cases, caseId, conditionOrder,
  workloadOrder, median, validateSample, filterLogs, summarize, table, main};

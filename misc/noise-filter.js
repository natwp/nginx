'use strict';

const fs = require('fs');
const path = require('path');

const rawDir = process.argv[2];
const label = process.argv[3];
const output = process.argv[4];

if (!rawDir || !label || !output) {
  throw new Error('usage: noise-filter.js raw-dir label output.jsonl');
}

function median(values) {
  if (!values.length) {
    return 0;
  }

  const sorted = values.slice().sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);

  return sorted.length % 2
    ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

function mad(values, center) {
  return median(values.map(value => Math.abs(value - center)));
}

function collect(dir, out) {
  for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
    const name = path.join(dir, entry.name);

    if (entry.isDirectory()) {
      collect(name, out);
      continue;
    }

    if (!entry.name.endsWith('.jsonl') || entry.name === 'summary.jsonl') {
      continue;
    }

    const run = path.basename(path.dirname(name));
    const rows = fs.readFileSync(name, 'utf8').trim().split(/\r?\n/)
      .filter(Boolean).map(line => JSON.parse(line));

    for (const row of rows) {
      out.push({run, row});
    }
  }
}

const rows = [];
collect(rawDir, rows);

if (!rows.length) {
  throw new Error(`no JSONL samples found under ${rawDir}`);
}

const byRunPath = new Map();
for (const item of rows) {
  const key = `${item.run}\u0000${item.row.path}`;
  if (!byRunPath.has(key)) {
    byRunPath.set(key, []);
  }
  byRunPath.get(key).push(item.row);
}

const runMedians = [];
for (const [key, samples] of byRunPath) {
  const split = key.indexOf('\u0000');
  const run = key.slice(0, split);
  const workload = key.slice(split + 1);
  const fields = [
    'requestsPerSecond', 'responseMiBPerSecond', 'latencyAverageMs',
    'latencyP50Ms', 'latencyP95Ms', 'errors'
  ];
  const result = {run, workload};

  for (const field of fields) {
    result[field] = median(samples.map(sample => Number(sample[field]) || 0));
  }

  for (const field of [
    'nginxCpuPercent', 'nginxCpuCores', 'nginxCpuPidCount',
    'nginxWorkerCpuPercent', 'nginxWorkerCpuCores',
    'nginxMasterRouterCpuPercent', 'nginxMasterRouterCpuCores',
    'clientCpuPercent', 'clientCpuCores'
  ]) {
    const values = samples.map(sample => Number(sample[field]))
      .filter(Number.isFinite);

    if (values.length) {
      result[field] = median(values);
    }
  }

  runMedians.push(result);
}

const workloads = [...new Set(runMedians.map(result => result.workload))].sort();
const outputRows = [];

for (const workload of workloads) {
  const group = runMedians.filter(result => result.workload === workload);
  const throughput = group.map(result => result.requestsPerSecond);
  const bandwidth = group.map(result => result.responseMiBPerSecond);
  const throughputCenter = median(throughput);
  const bandwidthCenter = median(bandwidth);
  const throughputMad = mad(throughput, throughputCenter);
  const bandwidthMad = mad(bandwidth, bandwidthCenter);
  const throughputLimit = Math.max(throughputCenter * 0.02,
                                   throughputMad * 2);
  const bandwidthLimit = Math.max(bandwidthCenter * 0.02,
                                  bandwidthMad * 2);
  const retained = [];
  const rejected = [];

  for (const result of group) {
    const badThroughput = Math.abs(result.requestsPerSecond
                                   - throughputCenter) > throughputLimit;
    const badBandwidth = Math.abs(result.responseMiBPerSecond
                                  - bandwidthCenter) > bandwidthLimit;

    if (badThroughput || badBandwidth) {
      rejected.push({
        run: result.run,
        requestsPerSecond: result.requestsPerSecond,
        responseMiBPerSecond: result.responseMiBPerSecond
      });
    } else {
      retained.push(result);
    }
  }

  if (retained.length < 3) {
    throw new Error(`noise filter retained only ${retained.length} runs for ${workload}`);
  }

  const pick = field => median(retained.map(result => result[field]));
  const errors = retained.reduce((sum, result) => sum + result.errors, 0);

  outputRows.push({
    label,
    workload,
    rawRuns: group.length,
    retainedRuns: retained.length,
    rejectedRuns: rejected.length,
    rejected,
    requestsPerSecond: pick('requestsPerSecond'),
    responseMiBPerSecond: pick('responseMiBPerSecond'),
    latencyAverageMs: pick('latencyAverageMs'),
    latencyP50Ms: pick('latencyP50Ms'),
    latencyP95Ms: pick('latencyP95Ms'),
    errors,
    filter: 'per-run sample median; reject either throughput metric beyond max(2%, 2*MAD)'
  });

  for (const field of [
    'nginxCpuPercent', 'nginxCpuCores', 'nginxCpuPidCount',
    'nginxWorkerCpuPercent', 'nginxWorkerCpuCores',
    'nginxMasterRouterCpuPercent', 'nginxMasterRouterCpuCores',
    'clientCpuPercent', 'clientCpuCores'
  ]) {
    const values = retained.map(result => Number(result[field]))
      .filter(Number.isFinite);

    if (values.length) {
      outputRows[outputRows.length - 1][field] = median(values);
    }
  }
}

fs.writeFileSync(output,
                 outputRows.map(row => JSON.stringify(row)).join('\n') + '\n');

for (const row of outputRows) {
  process.stdout.write(`${JSON.stringify(row)}\n`);
}

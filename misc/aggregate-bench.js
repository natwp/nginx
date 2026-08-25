'use strict';

const fs = require('fs');

const output = process.argv[2];
const inputs = process.argv.slice(3);

if (!output || inputs.length < 2) {
  throw new Error(
    'usage: aggregate-bench.js output.jsonl input1.jsonl input2.jsonl [...]'
  );
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);

  return sorted.length % 2
    ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

function finiteValues(rows, field) {
  return rows.map(row => Number(row[field])).filter(Number.isFinite);
}

const groups = new Map();

for (const input of inputs) {
  const rows = fs.readFileSync(input, 'utf8').trim().split(/\r?\n/)
    .filter(Boolean).map(line => JSON.parse(line));

  for (const row of rows) {
    const key = `${row.path}\u0000${row.sample}`;
    if (!groups.has(key)) {
      groups.set(key, []);
    }
    groups.get(key).push(row);
  }
}

const outputRows = [];

for (const rows of groups.values()) {
  if (rows.length !== inputs.length) {
    throw new Error(
      `sample ${rows[0].sample} for ${rows[0].path} has ${rows.length} of `
      + `${inputs.length} client rows`
    );
  }

  const requests = rows.reduce((sum, row) => sum + Number(row.requests), 0);
  const elapsed = median(rows.map(row => Number(row.seconds)));
  const weightedLatency = requests
    ? rows.reduce((sum, row) =>
      sum + Number(row.latencyAverageMs) * Number(row.requests), 0) / requests
    : 0;
  const result = {
    ...rows[0],
    seconds: elapsed,
    requests,
    requestsPerSecond: rows.reduce((sum, row) =>
      sum + Number(row.requestsPerSecond), 0),
    responseMiBPerSecond: rows.reduce((sum, row) =>
      sum + Number(row.responseMiBPerSecond), 0),
    latencyAverageMs: weightedLatency,
    latencyP50Ms: median(rows.map(row => Number(row.latencyP50Ms))),
    latencyP95Ms: median(rows.map(row => Number(row.latencyP95Ms))),
    errors: rows.reduce((sum, row) => sum + Number(row.errors), 0),
    clientProcesses: inputs.length
  };

  for (const field of ['nginxCpuPercent', 'nginxCpuCores']) {
    const values = finiteValues(rows, field);
    result[field] = values.length ? median(values) : null;
  }

  const pidCounts = finiteValues(rows, 'nginxCpuPidCount');
  result.nginxCpuPidCount = pidCounts.length ? Math.max(...pidCounts) : 0;

  for (const field of ['clientCpuPercent', 'clientCpuCores']) {
    const values = finiteValues(rows, field);
    result[field] = values.length
      ? values.reduce((sum, value) => sum + value, 0)
      : null;
  }

  outputRows.push(result);
}

outputRows.sort((left, right) => {
  const path = left.path.localeCompare(right.path);
  return path || left.sample - right.sample;
});

fs.writeFileSync(output,
                 outputRows.map(row => JSON.stringify(row)).join('\n') + '\n');

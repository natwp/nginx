'use strict';

const fs = require('fs');
const path = require('path');

const candidateDir = process.argv[2];
const controlDir = process.argv[3];
const candidateLabel = process.argv[4];
const controlLabel = process.argv[5];
const output = process.argv[6];

if (!candidateDir || !controlDir || !candidateLabel || !controlLabel || !output) {
  throw new Error(
    'usage: noise-compare.js candidate-raw control-raw candidate-label control-label output.jsonl'
  );
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

function runNumber(name) {
  const match = /^repeat(\d+)-/.exec(name);
  return match ? Number(match[1]) : NaN;
}

function collect(dir) {
  const result = new Map();

  for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
    if (!entry.isDirectory()) {
      continue;
    }

    const number = runNumber(entry.name);
    if (!Number.isInteger(number)) {
      continue;
    }

    const runDir = path.join(dir, entry.name);
    for (const file of fs.readdirSync(runDir)) {
      if (!file.endsWith('.jsonl')) {
        continue;
      }

      const filePath = path.join(runDir, file);
      const rows = fs.readFileSync(filePath, 'utf8').trim().split(/\r?\n/)
        .filter(Boolean).map(line => JSON.parse(line));

      if (!rows.length) {
        continue;
      }

      const fields = [
        'requestsPerSecond', 'responseMiBPerSecond', 'latencyAverageMs',
        'latencyP50Ms', 'latencyP95Ms', 'errors'
      ];
      const summary = {run: number, workload: rows[0].path};

      for (const field of fields) {
        summary[field] = median(rows.map(row => Number(row[field]) || 0));
      }

      for (const field of [
        'nginxCpuPercent', 'nginxCpuCores', 'nginxCpuPidCount',
        'nginxWorkerCpuPercent', 'nginxWorkerCpuCores',
        'nginxMasterRouterCpuPercent', 'nginxMasterRouterCpuCores',
        'clientCpuPercent', 'clientCpuCores'
      ]) {
        const values = rows.map(row => Number(row[field]))
          .filter(Number.isFinite);

        if (values.length) {
          summary[field] = median(values);
        }
      }

      result.set(`${number}\u0000${summary.workload}`, summary);
    }
  }

  return result;
}

const candidate = collect(candidateDir);
const control = collect(controlDir);
const keys = [...new Set([...candidate.keys(), ...control.keys()])].sort();
const byWorkload = new Map();

for (const key of keys) {
  const split = key.indexOf('\u0000');
  const workload = key.slice(split + 1);
  if (!byWorkload.has(workload)) {
    byWorkload.set(workload, {
      pairs: [],
      candidateOnly: [],
      controlOnly: []
    });
  }

  const left = candidate.get(key);
  const right = control.get(key);
  const group = byWorkload.get(workload);

  if (left && right) {
    group.pairs.push({candidate: left, control: right});
  } else if (left) {
    group.candidateOnly.push(left.run);
  } else {
    group.controlOnly.push(right.run);
  }
}

const outputRows = [];

for (const [workload, group] of byWorkload) {
  const pairs = group.pairs;

  if (pairs.length < 3) {
    throw new Error(`only ${pairs.length} paired runs for ${workload}`);
  }

  const ratios = pairs.map(pair =>
    pair.candidate.requestsPerSecond / pair.control.requestsPerSecond);
  const bandwidthRatios = pairs.map(pair =>
    pair.candidate.responseMiBPerSecond / pair.control.responseMiBPerSecond);
  const ratioCenter = median(ratios);
  const bandwidthCenter = median(bandwidthRatios);
  const ratioLimit = Math.max(ratioCenter * 0.02, mad(ratios, ratioCenter) * 2);
  const bandwidthLimit = Math.max(bandwidthCenter * 0.02,
                                  mad(bandwidthRatios, bandwidthCenter) * 2);
  const retained = [];
  const rejected = [];

  pairs.forEach((pair, index) => {
    const ratio = ratios[index];
    const bandwidthRatio = bandwidthRatios[index];
    const outlier = Math.abs(ratio - ratioCenter) > ratioLimit
      || Math.abs(bandwidthRatio - bandwidthCenter) > bandwidthLimit;

    if (outlier) {
      rejected.push({
        run: pair.candidate.run,
        requestsPerSecondRatio: ratio,
        responseMiBPerSecondRatio: bandwidthRatio
      });
    } else {
      retained.push(pair);
    }
  });

  if (retained.length < 3) {
    throw new Error(`paired filter retained only ${retained.length} runs for ${workload}`);
  }

  const retainedRatios = retained.map(pair =>
    pair.candidate.requestsPerSecond / pair.control.requestsPerSecond);
  const retainedBandwidthRatios = retained.map(pair =>
    pair.candidate.responseMiBPerSecond / pair.control.responseMiBPerSecond);
  const medianField = (side, field) =>
    median(retained.map(pair => pair[side][field]));
  const ratio = median(retainedRatios);
  const bandwidthRatio = median(retainedBandwidthRatios);

  outputRows.push({
    candidate: candidateLabel,
    control: controlLabel,
    workload,
    candidateRuns: pairs.length + group.candidateOnly.length,
    controlRuns: pairs.length + group.controlOnly.length,
    rawPairs: pairs.length,
    unpairedCandidateRuns: group.candidateOnly.sort((a, b) => a - b),
    unpairedControlRuns: group.controlOnly.sort((a, b) => a - b),
    retainedPairs: retained.length,
    rejectedPairs: rejected.length,
    rejected,
    candidateRequestsPerSecond: medianField('candidate', 'requestsPerSecond'),
    controlRequestsPerSecond: medianField('control', 'requestsPerSecond'),
    requestsPerSecondRatio: ratio,
    requestsPerSecondDeltaPercent: (ratio - 1) * 100,
    candidateResponseMiBPerSecond:
      medianField('candidate', 'responseMiBPerSecond'),
    controlResponseMiBPerSecond:
      medianField('control', 'responseMiBPerSecond'),
    responseMiBPerSecondRatio: bandwidthRatio,
    responseMiBPerSecondDeltaPercent: (bandwidthRatio - 1) * 100,
    candidateLatencyP50Ms: medianField('candidate', 'latencyP50Ms'),
    controlLatencyP50Ms: medianField('control', 'latencyP50Ms'),
    candidateLatencyP95Ms: medianField('candidate', 'latencyP95Ms'),
    controlLatencyP95Ms: medianField('control', 'latencyP95Ms'),
    errors: retained.reduce((sum, pair) =>
      sum + pair.candidate.errors + pair.control.errors, 0),
    filter: 'paired per-round sample medians; reject either ratio beyond max(2%, 2*MAD)'
  });

  const outputRow = outputRows[outputRows.length - 1];
  for (const field of ['nginxCpuPercent', 'nginxCpuCores',
                       'nginxCpuPidCount', 'nginxWorkerCpuPercent',
                       'nginxWorkerCpuCores',
                       'nginxMasterRouterCpuPercent',
                       'nginxMasterRouterCpuCores', 'clientCpuPercent',
                       'clientCpuCores']) {
    const candidateValues = retained
      .map(pair => Number(pair.candidate[field]))
      .filter(Number.isFinite);
    const controlValues = retained
      .map(pair => Number(pair.control[field]))
      .filter(Number.isFinite);

    if (candidateValues.length) {
      outputRow[`candidate${field[0].toUpperCase()}${field.slice(1)}`] =
        median(candidateValues);
    }
    if (controlValues.length) {
      outputRow[`control${field[0].toUpperCase()}${field.slice(1)}`] =
        median(controlValues);
    }
    if (candidateValues.length && controlValues.length
        && field !== 'nginxCpuPidCount') {
      const candidate = median(candidateValues);
      const control = median(controlValues);
      outputRow[`${field}Delta`] = candidate - control;
    }
  }
}

fs.writeFileSync(output,
                 outputRows.map(row => JSON.stringify(row)).join('\n') + '\n');

for (const row of outputRows) {
  process.stdout.write(`${JSON.stringify(row)}\n`);
}

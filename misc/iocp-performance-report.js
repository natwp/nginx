'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const base = require('./benchmark-report');

const variants = ['baseline-a', 'baseline-b', 'candidate'];
const permutations = [[0, 1, 2], [1, 2, 0], [2, 0, 1],
  [0, 2, 1], [2, 1, 0], [1, 0, 2]];

function order(round, index) {
  return permutations[(round + index) % permutations.length].map(i => variants[i]);
}

function cases(manifest) {
  return base.cases(manifest.kind, manifest.multiWorkers).filter(c => c.backend === 'iocp');
}

function summarize(manifest, samples) {
  assert(manifest.schemaVersion === 1 && manifest.completedAt, 'incomplete trial');
  assert(Number.isInteger(manifest.repeats) && manifest.repeats >= 6
    && manifest.repeats % 6 === 0, 'unbalanced rounds');
  for (const key of ['sampleSeconds', 'clientCpuBudget', 'upstreamCpuBudget']) {
    assert(Number.isFinite(manifest[key]) && manifest[key] > 0, `invalid ${key}`);
  }
  assert.deepStrictEqual(manifest.policy, base.policy, 'changed noise policy');
  assert(manifest.binaries['baseline-a'].sha256 === manifest.binaries['baseline-b'].sha256,
    'A/A controls must use identical executables');
  const plan = cases(manifest);
  const ids = new Set(plan.map(c => c.id));
  const groups = new Map();
  const ordinals = new Set();
  for (const sample of samples) {
    assert(ids.has(sample.id) && variants.includes(sample.variant), 'unknown case/variant');
    assert(Number.isInteger(sample.round) && sample.round >= 0 && sample.round < manifest.repeats);
    assert(Number.isInteger(sample.ordinal) && sample.ordinal >= 0
      && !ordinals.has(sample.ordinal), 'invalid or duplicate ordinal');
    ordinals.add(sample.ordinal);
    const row = plan.find(c => c.id === sample.id);
    assert(Number.isInteger(sample.requests) && sample.requests > 0
      && sample.bytes === sample.requests * row.bytes);
    for (const key of ['requestsPerSecond', 'seconds', 'idleCpuPercent', 'clientCpuCores', 'nginxCpuCores']) {
      assert(Number.isFinite(sample[key]) && sample[key] >= 0, `invalid ${key}`);
    }
    assert(sample.requestsPerSecond > 0 && sample.seconds > 0);
    assert(sample.idleCpuPercent <= 100, 'invalid idle CPU');
    if (manifest.kind === 'proxy') {
      assert(Number.isFinite(sample.upstreamCpuCores) && sample.upstreamCpuCores >= 0,
        'invalid upstream CPU');
    }
    const key = `${sample.id}/${sample.variant}`;
    if (!groups.has(key)) groups.set(key, new Map());
    assert(!groups.get(key).has(sample.round), 'duplicate round');
    groups.get(key).set(sample.round, sample);
  }
  assert(samples.length === plan.length * variants.length * manifest.repeats, 'missing samples');
  assert([...ordinals].every(value => value < samples.length), 'noncontiguous ordinals');
  const rows = plan.map(row => {
    const byVariant = Object.fromEntries(variants.map(variant => {
      const group = groups.get(`${row.id}/${variant}`);
      assert(group && group.size === manifest.repeats, 'missing variant/round');
      return [variant, [...group.values()].sort((a, b) => a.round - b.round)];
    }));
    for (let round = 0; round < manifest.repeats; round++) {
      const actual = [...variants].sort((a, b) => byVariant[a][round].ordinal
        - byVariant[b][round].ordinal);
      assert.deepStrictEqual(actual, order(round, plan.indexOf(row)), 'incorrect arm order');
      assert(byVariant[actual[2]][round].ordinal - byVariant[actual[0]][round].ordinal === 2,
        'matched triplet was not measured consecutively');
    }
    const rates = Object.fromEntries(variants.map(variant => [variant,
      base.filterLogs(byVariant[variant].map(s => ({round: s.round,
        value: Math.log(s.requestsPerSecond), exclusion: base.sampleExclusion(s, manifest)})),
      manifest.repeats)]));
    const compare = (candidate, control) => {
      const pairs = byVariant[candidate].map((a, i) => {
        const b = byVariant[control][i];
        return {round: a.round, value: Math.log(a.requestsPerSecond / b.requestsPerSecond),
          first: a.ordinal < b.ordinal,
          exclusion: base.sampleExclusion(a, manifest) || base.sampleExclusion(b, manifest)};
      });
      const result = base.filterLogs(pairs, manifest.repeats, true);
      const retained = pairs.filter(p => result.rounds.includes(p.round));
      const byOrder = [true, false].map(first => retained.filter(p => p.first === first));
      return {...result, deltaPercent: result.median === null ? null : 100 * (result.median - 1),
        orderDeltas: byOrder.map(rows => rows.length ? 100 * Math.expm1(base.median(rows.map(p => p.value))) : null)};
    };
    const aa = compare('baseline-b', 'baseline-a');
    const comparisons = ['baseline-a', 'baseline-b'].map(control => compare('candidate', control));
    const reasons = [];
    if (aa.status !== 'stable' || Math.abs(aa.deltaPercent) > 2
      || aa.orderDeltas.some(delta => delta === null || Math.abs(delta) > 2)) {
      reasons.push('A/A check is not within 2% in both orders');
    }
    if (comparisons.some(c => c.status !== 'stable')) reasons.push('candidate comparison is inconclusive');
    const clientCpu = Object.fromEntries(variants.map(v => [v,
      base.median(byVariant[v].map(s => s.clientCpuCores))]));
    if (Object.values(clientCpu).some(cpu => cpu > 0.85 * manifest.clientCpuBudget)) {
      reasons.push('load generator has less than 15% CPU headroom');
    }
    const upstreamCpu = manifest.kind === 'proxy'
      ? Object.fromEntries(variants.map(v => [v, base.median(byVariant[v].map(s => s.upstreamCpuCores))]))
      : null;
    if (upstreamCpu && Object.values(upstreamCpu).some(cpu => cpu > 0.85 * manifest.upstreamCpuBudget)) {
      reasons.push('upstream has less than 15% CPU headroom');
    }
    let decision = 'inconclusive';
    if (!reasons.length) {
      if (comparisons.every(c => c.deltaPercent < -2 && c.orderDeltas.every(d => d < -2))) {
        decision = 'regression';
      } else if (comparisons.every(c => c.deltaPercent >= 2 && c.orderDeltas.every(d => d >= 2))) {
        decision = 'improvement';
      } else if (comparisons.every(c => c.deltaPercent >= -2 && c.orderDeltas.every(d => d >= -2))) {
        decision = 'unchanged';
      } else {
        reasons.push('execution orders do not establish a 2% non-regression');
      }
    }
    return {...row, rates, aa, comparisons, clientCpu, upstreamCpu, decision, reasons};
  });
  const decision = rows.some(r => r.decision === 'regression') ? 'reject'
    : rows.some(r => r.decision === 'inconclusive') ? 'inconclusive'
      : rows.some(r => r.decision === 'improvement') ? 'retain' : 'no improvement';
  return {manifest, totalSamples: samples.length, decision, rows};
}

function markdown(report) {
  const rate = r => r.status === 'stable' ? `${r.median.toFixed(0)} (${r.retained}/${r.raw})` : 'inconclusive';
  const delta = r => r.status === 'stable' ? `${r.deltaPercent.toFixed(2)}% (${r.retained}/${r.raw})` : 'inconclusive';
  const lines = [`# IOCP ${report.manifest.kind}: ${report.manifest.profile}`, '',
    `Run: ${report.manifest.runUrl}. Retention decision: **${report.decision}**.`, '',
    'Three interleaved arms: identical baseline A/B executables and the candidate.',
    'Percentages use matched-round ratios; counts are retained/planned pairs.', '',
    '| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A delta | Candidate/A | Candidate/B | Decision |',
    '| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |'];
  for (const row of report.rows) {
    lines.push(`| ${row.id} | ${variants.map(v => rate(row.rates[v])).join(' | ')} | `
      + `${delta(row.aa)} | ${row.comparisons.map(delta).join(' | ')} | ${row.decision} |`);
  }
  lines.push('', '## Limits and gate reasons', '',
    'Isolated runs reserve separate CPU masks for clients, nginx, and upstream.',
    'Shared runs measure contention on the whole runner; they do not establish',
    'dedicated-server scaling. CPU headroom does not prove network headroom.',
    'These fixed-concurrency runs measure throughput, not open-loop tail latency.', '');
  for (const row of report.rows.filter(r => r.reasons.length)) {
    lines.push(`- ${row.id}: ${row.reasons.join('; ')}.`);
  }
  return lines.join('\n') + '\n';
}

function main(directory) {
  const manifest = JSON.parse(fs.readFileSync(path.join(directory, 'manifest.json')));
  const samples = fs.readFileSync(path.join(directory, 'samples.jsonl'), 'utf8')
    .trim().split(/\r?\n/).filter(Boolean).map(JSON.parse);
  const result = summarize(manifest, samples);
  fs.writeFileSync(path.join(directory, 'summary.json'), JSON.stringify(result, null, 2) + '\n');
  fs.writeFileSync(path.join(directory, 'summary.md'), markdown(result));
  return result;
}

function gate(reports) {
  const expected = ['static/isolated', 'static/shared', 'proxy/isolated', 'proxy/shared'];
  assert.deepStrictEqual(reports.map(r => `${r.manifest.kind}/${r.manifest.profile}`).sort(),
    expected.sort(), 'all four workload/CPU profiles are required');
  for (const report of reports) {
    assert.deepStrictEqual(report.manifest.binaries, reports[0].manifest.binaries,
      'profiles used different binaries');
  }
  if (reports.some(r => r.decision === 'reject')) return 'reject';
  if (reports.some(r => r.decision === 'inconclusive')) return 'inconclusive';
  if (!reports.some(r => r.manifest.profile === 'isolated' && r.decision === 'retain')) {
    return 'no isolated improvement';
  }
  return 'retain';
}

if (require.main === module) {
  if (process.argv[2] === '--gate') {
    const reports = process.argv.slice(3).map(main);
    const decision = gate(reports);
    process.stdout.write(`IOCP performance gate: ${decision}\n`);
    if (decision !== 'retain') process.exitCode = 1;
  } else {
    const result = main(process.argv[2]);
    process.stdout.write(`${result.totalSamples} samples: ${result.decision}\n`);
  }
}

module.exports = {variants, order, cases, summarize, markdown, main, gate};

'use strict';

const fs = require('fs');
const path = require('path');

const root = process.argv[2];

if (!root) {
  throw new Error('usage: validate-oha-results.js result-directory');
}

function collect(directory, output) {
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    const name = path.join(directory, entry.name);

    if (entry.isDirectory()) {
      collect(name, output);
    } else if (entry.name.endsWith('.jsonl')) {
      output.push(name);
    }
  }
}

const files = [];
collect(root, files);

if (!files.length) {
  throw new Error(`no JSONL benchmark output under ${root}`);
}

for (const file of files) {
  const rows = fs.readFileSync(file, 'utf8').trim().split(/\r?\n/)
    .filter(Boolean).map(line => JSON.parse(line));

  if (!rows.length) {
    throw new Error(`empty benchmark output: ${file}`);
  }

  for (const row of rows) {
    if (!(Number(row.requestsPerSecond) > 0)
        || Number(row.errors) !== 0
        || Number(row.successRate) !== 1) {
      throw new Error(
        `invalid or non-zero-error sample in ${file}: ${JSON.stringify(row)}`
      );
    }

    if (!(Number(row.nginxCpuCores) > 0)
        || !(Number(row.clientCpuCores) > 0)
        || !(Number(row.nginxCpuPidCount) > 0)
        || Number(row.clientCpuPidCount) !== Number(row.clientProcesses)) {
      throw new Error(`missing CPU sample in ${file}`);
    }
  }
}

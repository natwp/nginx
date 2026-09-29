'use strict';

// MinGW64 gmon.out stores relocated PCs and padded 32-bit arc counts.
// Decode copies so both the original evidence and executable stay intact.
const assert = require('assert');
const fs = require('fs');
const path = require('path');

function textRange(symbols) {
  const entries = symbols.split(/\r?\n/).map(line =>
    /^([0-9a-fA-F]+)\s+[tT]\s+(\S+)$/.exec(line)).filter(Boolean);
  assert(entries.length, 'missing text symbols');
  const addresses = entries.map(entry => BigInt('0x' + entry[1]));
  const end = entries.find(entry => entry[2] === 'etext');
  assert(end, 'missing MinGW etext symbol');
  return {start: addresses.reduce((a, b) => a < b ? a : b),
    end: BigInt('0x' + end[1])};
}

function normalize(raw, range) {
  assert(raw.length >= 40, 'truncated MinGW64 profile header');
  const low = raw.readBigUInt64LE(0), high = raw.readBigUInt64LE(8);
  const histogramEnd = raw.readUInt32LE(16);
  assert(raw.readUInt32LE(20) === 0x51879, 'unsupported MinGW profile format');
  assert(raw.readUInt32LE(24) > 0, 'invalid profile sample rate');
  assert(high > low && histogramEnd >= 40 && histogramEnd <= raw.length
    && (histogramEnd - 40) % 2 === 0 && (raw.length - histogramEnd) % 24 === 0,
  'invalid histogram or arc length');
  assert(high - low === BigInt(histogramEnd - 40) * 2n,
    'unexpected MinGW64 histogram scale');
  const relocation = high - range.end;
  assert(relocation % 65536n === 0n && low - relocation >= range.start
    && low - relocation < range.end, 'profile does not match the text range');
  const output = Buffer.from(raw);
  output.writeBigUInt64LE(low - relocation, 0);
  output.writeBigUInt64LE(high - relocation, 8);
  output.fill(0, 28, 40);
  let samples = 0, calls = 0n;
  for (let offset = 40; offset < histogramEnd; offset += 2) {
    samples += raw.readUInt16LE(offset);
  }
  for (let offset = histogramEnd; offset < raw.length; offset += 24) {
    for (const field of [offset, offset + 8]) {
      const pc = raw.readBigUInt64LE(field);
      assert(pc >= low && pc <= high, 'arc address outside the profiled image');
      output.writeBigUInt64LE(pc - relocation, field);
    }
    calls += BigInt(raw.readUInt32LE(offset + 16));
    // Windows long is 32 bits; clear padding also read by Unix gprof.
    output.fill(0, offset + 20, offset + 24);
  }
  assert(calls > 0n, 'profile has no recorded calls');
  return {output, metadata: {relocation: relocation.toString(), samples,
    arcRecords: (raw.length - histogramEnd) / 24, calls: calls.toString()}};
}

function profiles(root) {
  const files = [];
  for (const row of fs.readdirSync(root, {withFileTypes: true})) {
    if (!row.isDirectory()) continue;
    const directory = path.join(root, row.name, 'gprof');
    if (!fs.existsSync(directory)) continue;
    for (const process of fs.readdirSync(directory, {withFileTypes: true})) {
      if (!process.isDirectory()) continue;
      const file = path.join(directory, process.name, 'gmon.out');
      if (fs.existsSync(file)) files.push(file);
    }
  }
  assert(files.length, 'no worker or router profiles found');
  return files;
}

function verify(file, text) {
  const flat = text.split('Call graph')[0];
  const counts = new Map();
  for (const line of flat.split(/\r?\n/)) {
    const match = /^\s*[\d.]+\s+[\d.]+\s+[\d.]+\s+(\d+)\s+\S+\s+\S+\s+(\S+)/.exec(line);
    if (match) counts.set(match[2], BigInt(match[1]));
  }
  assert([...counts.values()].some(value => value > 0n),
    'decoded profile has no symbolized calls: ' + file);
  if (path.basename(path.dirname(file)).startsWith('worker-')) {
    for (const name of ['ngx_http_create_request', 'ngx_http_keepalive_handler']) {
      assert(counts.get(name) > 0n, 'missing worker function ' + name + ': ' + file);
    }
  }
}

function main(args) {
  assert(args.length === 2, 'usage: win32-gprof.js <nm-symbols|--verify> <profile-root>');
  const files = profiles(args[1]);
  if (args[0] === '--verify') {
    for (const file of files) verify(file, fs.readFileSync(file + '.txt', 'utf8'));
  } else {
    const range = textRange(fs.readFileSync(args[0], 'utf8'));
    for (const file of files) {
      const {output, metadata} = normalize(fs.readFileSync(file), range);
      fs.writeFileSync(file + '.linked', output);
      fs.writeFileSync(file + '.normalization.json', JSON.stringify(metadata, null, 2) + '\n');
    }
  }
  process.stdout.write(files.length + ' profiles ' + (args[0] === '--verify'
    ? 'contain symbolized calls\n' : 'normalized without changing raw evidence\n'));
}

if (require.main === module) main(process.argv.slice(2));
module.exports = {textRange, normalize, verify};

'use strict';

const assert = require('assert');
const {textRange, normalize, verify} = require('./win32-gprof');

const symbols = '0000000140001000 t .text\n0000000140001100 t local_worker\n'
  + '0000000140002000 T etext\n0000000140003000 D data\n';
const range = textRange(symbols);
assert.deepStrictEqual(range, {start: 0x140001000n, end: 0x140002000n});
assert.throws(() => textRange(''), /missing text/);
assert.throws(() => textRange('0000000140001000 T function'), /etext/);

const relocation = 0x7ff700000000n - 0x140000000n;
const histogramEnd = 40 + Number((range.end - range.start) / 2n);
const raw = Buffer.alloc(histogramEnd + 24, 0);
raw.writeBigUInt64LE(range.start + relocation, 0);
raw.writeBigUInt64LE(range.end + relocation, 8);
raw.writeUInt32LE(histogramEnd, 16);
raw.writeUInt32LE(0x51879, 20);
raw.writeUInt32LE(100, 24);
raw.fill(0xff, 28, 40);
raw.writeUInt16LE(7, 40);
raw.writeBigUInt64LE(range.start + relocation + 0x40n, histogramEnd);
raw.writeBigUInt64LE(range.start + relocation + 0x110n, histogramEnd + 8);
raw.writeUInt32LE(123456, histogramEnd + 16);
raw.fill(0xff, histogramEnd + 20);
const original = Buffer.from(raw);
const {output, metadata} = normalize(raw, range);
assert.deepStrictEqual(raw, original, 'raw profile must remain unchanged');
assert.strictEqual(output.readBigUInt64LE(0), range.start);
assert.strictEqual(output.readBigUInt64LE(8), range.end);
assert.strictEqual(output.readBigUInt64LE(histogramEnd), range.start + 0x40n);
assert.strictEqual(output.readBigUInt64LE(histogramEnd + 8), range.start + 0x110n);
assert.strictEqual(output.readBigUInt64LE(histogramEnd + 16), 123456n);
assert.deepStrictEqual(output.subarray(40, histogramEnd), raw.subarray(40, histogramEnd));
assert.deepStrictEqual(metadata, {relocation: relocation.toString(), samples: 7,
  arcRecords: 1, calls: '123456'});
assert.deepStrictEqual(normalize(output, range).output, output);
assert.throws(() => normalize(raw.subarray(0, 39), range), /truncated/);
assert.throws(() => normalize(raw.subarray(0, -1), range), /length/);
assert.throws(() => normalize(raw, {...range, end: range.end + 1n}), /text range/);
const bad = Buffer.from(raw);
bad.writeBigUInt64LE(range.end + relocation + 1n, histogramEnd + 8);
assert.throws(() => normalize(bad, range), /arc address/);
bad.set(raw);
bad.writeUInt32LE(0, histogramEnd + 16);
assert.throws(() => normalize(bad, range), /no recorded calls/);

const worker = 'case/gprof/worker-123-0/gmon.out';
const flat = ' 0.00 0.00 0.00 300 0.00 0.00 ngx_http_create_request\n'
  + ' 0.00 0.00 0.00 299 0.00 0.00 ngx_http_keepalive_handler\n';
verify(worker, flat);
assert.throws(() => verify(worker, 'Flat profile:\nCall graph\n' + flat), /no symbolized calls/);
assert.throws(() => verify(worker, flat.split('\n')[0]), /keepalive_handler/);
console.log('MinGW relocated profile, arc counts, and symbol validation passed');

'use strict';

const assert = require('assert');
const {validateResult} = require('./validate-oha-summary');

function valid(overrides = {}) {
  return {
    statusCodeDistribution: {'200': 100},
    summary: {
      requestsPerSec: 100,
      errors: 0,
      successRate: 1,
      ...overrides
    }
  };
}

validateResult(valid(), 200);
assert.throws(() => validateResult({summary: valid().summary}, 200));
assert.throws(() => validateResult(valid({errors: 1}), 200));
assert.throws(() => validateResult(valid({successRate: 0.5}), 200));
assert.throws(() => validateResult(valid({requestsPerSec: 0}), 200));
assert.throws(() => validateResult(valid({requestsPerSec: Infinity}), 200));
assert.throws(() => validateResult({...valid(),
  errorDistribution: {'connection refused': 1}}, 200));
assert.throws(() => validateResult({summary: {
  ...valid().summary, errorDistribution: {'connection refused': 1}
}}, 200));
assert.throws(() => validateResult({summary: valid().summary,
  statusCodeDistribution: {'500': 1}}, 200));
validateResult({summary: valid().summary,
  statusCodeDistribution: {'200': 3}}, 200);

process.stdout.write('oha summary validator tests passed\n');

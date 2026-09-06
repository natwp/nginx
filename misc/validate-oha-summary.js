'use strict';

const fs = require('fs');

function validateResult(result, expectedStatus) {
  if (!result || typeof result !== 'object') {
    throw new Error('oha output is not an object');
  }

  const summary = result.summary;
  if (!summary || typeof summary !== 'object') {
    throw new Error('oha output has no summary');
  }

  const errorDistribution = summary.errorDistribution
    || result.errorDistribution;
  const errorCount = errorDistribution && typeof errorDistribution === 'object'
    ? Object.values(errorDistribution).reduce((sum, value) =>
      sum + Number(value || 0), 0)
    : 0;

  if (!Number.isFinite(summary.requestsPerSec) || summary.requestsPerSec <= 0
      || (summary.errors !== undefined && Number(summary.errors) !== 0)
      || errorCount !== 0
      || Number(summary.successRate) !== 1)
  {
    throw new Error(`invalid oha summary: ${JSON.stringify(summary)}`);
  }

  const distribution = summary.statusCodeDistribution
    || result.statusCodeDistribution;

  if (!distribution || typeof distribution !== 'object'
      || !Object.keys(distribution).length
      || Object.entries(distribution).some(([status, count]) =>
        Number(status) !== expectedStatus || !Number.isInteger(count)
        || count <= 0)) {
    throw new Error(
      `unexpected HTTP status distribution: ${JSON.stringify(distribution)}`
    );
  }

  return summary;
}

function main() {
  const file = process.argv[2];
  const expectedStatus = Number(process.argv[3] || 200);

  if (!file || !Number.isInteger(expectedStatus) || expectedStatus < 100
      || expectedStatus > 599)
  {
    throw new Error('usage: validate-oha-summary.js result.json [status]');
  }

  validateResult(JSON.parse(fs.readFileSync(file, 'utf8')), expectedStatus);
}

if (require.main === module) {
  try {
    main();
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}

module.exports = {validateResult};

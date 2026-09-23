# Windows CI benchmark matrix

The [extended benchmark workflow](../.github/workflows/benchmarks-extended.yml)
compares IOCP, select, and poll using the same optimized nginx binary. Static
files and HTTP proxy traffic run in separate Windows 2022 jobs; all comparisons
within a workload family use the same runner. The latest completed measurements
are summarized in the [README](../README.md#performance).

## Coverage and duration

Every suite covers the same 96 cases:

| Dimension | Values |
| --- | --- |
| Event backend | IOCP, select, poll |
| Worker processes | 1 and 4 by default; the multi-worker count can be 2, 4, or 8 |
| Response size | 1 KiB and 1 MiB |
| Client transport | HTTP/1.1 over plain TCP and TLS 1.3 |
| Static files | `sendfile off` and `sendfile on` |
| HTTP proxy | Fast upstream and an upstream delaying each response by 20 ms |

The proxy upstream serves memory-backed bodies over HTTP/1.1 with keepalive;
proxy buffering and caching are disabled. TLS applies to the connection between
the client and nginx. The upstream connection remains plaintext. The delayed
upstream waits before sending headers; it does not throttle the response body.

`sendfile on` describes the configured directive. OpenSSL TLS uses nginx's SSL
output path, so enabling this directive does not imply zero-copy TLS transfer.

| Suite | Warm-up before each sample | Measured duration | Samples per case |
| --- | ---: | ---: | ---: |
| quick (default) | 1 s | 2 s | 6 |
| standard | 1 s | 3 s | 6 |
| comprehensive | 1 s | 3 s | 12 |

Each sample uses native oha 1.16.0 with 32 keepalive connections and two client
threads. The quick suite records 576 measurements plus a warm-up for each.
The static and proxy jobs run concurrently, on separate runners. The build job
first exercises all cases with short smoke samples at the same connection
count; smoke results are never eligible for performance claims.

## Order, validation, and noise filtering

Each round visits all backend/worker configurations in a balanced order. Over
six rounds each configuration occupies every position once, and every pair
appears in both orders three times. Workload order also rotates and reverses.
nginx restarts between configurations. All clients finish before nginx is
stopped; setup, shutdown, and warm-ups are outside the measured request rate.

The runner checks the selected backend and worker count in the nginx startup
log. It verifies each response body and the TLS protocol before measurement.
Every warm-up and measured sample must contain only HTTP 200 responses, zero
errors, the expected body size, and consistent request/byte counts. Invalid
responses, missing samples, process exits, or nginx error logs fail the job.
Outstanding requests drain after the measurement deadline; a five-second
request timeout bounds failures.

The filter policy is fixed before samples are collected:

- Exclude a sample if host CPU utilization before its warm-up remains above
  20% after up to three idle checks, or elapsed time exceeds the requested
  duration by more than 25% plus 100 ms.
- Filter absolute log request rates and paired log rate ratios separately.
  Reject values farther from the median than
  `max(log(1.03), 3 * 1.4826 * MAD)`, where MAD is the median absolute
  deviation. The same rule applies to positive and negative differences.
- Retain at least four samples and at least two-thirds of the planned samples.
  Retained scaled MAD must be at most 5%.
- For comparisons, also require at least two retained pairs in each execution
  order. The two order-specific medians must differ by at most 5%.

An estimate failing any gate is **inconclusive**. The report keeps all raw
samples, exclusions, counts, and reasons. A comparison uses the median ratio
of matched rounds, independently of the absolute-rate filter. Dividing two
separately filtered table medians does not reproduce the paired comparison.
The log-space median of an even sample count is the geometric mean of the two
central rates or ratios.

Stable means the observed samples passed these dispersion and ordering gates;
it is not a statistical significance test or a promise about another machine.
The client, nginx, and proxy upstream share runner CPUs. Multi-worker results
therefore also reflect client/upstream contention and the available CPU count.
Small percentage differences should not be treated as universal speedups.

## Running and reading the workflow

Open [Performance Benchmarks (Extended)](https://github.com/natwp/nginx-iocp/actions/workflows/benchmarks-extended.yml)
and select **Run workflow**, `test_suite`, and `worker_count`. The workflow also
runs monthly and on benchmark changes pushed to `fix/benchmark-*` branches.

Each workload job publishes a Markdown summary and a 90-day artifact named
`benchmark-results-static` or `benchmark-results-proxy` containing:

- `manifest.json`: source commit, run/attempt, binary and tool hashes, build
  options, CPU/OS, durations, connection count, and filter policy.
- `samples.jsonl` and `raw/`: normalized observations, Windows CPU counters,
  all raw oha measurements, and warm-ups.
- `summary.json` and `summary.md`: filtered rates, all paired comparisons,
  exclusions, and inconclusive reasons.
- `readme-table.md`, `probes.jsonl`, and `prefixes/`: generated rate table,
  response checks, nginx configurations, and logs.

Recompute a report without running the workload:

```sh
node misc/benchmark-report.js path/to/extracted/artifact
```

The build also uploads `benchmark-binaries` for seven days and
`benchmark-smoke-results` for 14 days. A failed run preserves diagnostic
artifacts but does not provide a completed performance comparison.

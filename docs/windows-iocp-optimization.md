# Windows IOCP optimization and measurement

The optimization goal is lower overhead in ordinary one-worker workloads and
better scaling with multiple workers, while retaining nginx configuration and
protocol behavior. An asynchronous API removes some readiness polling, but
does not make allocation, copying, completion processing, TLS, or scheduling
free. IOCP's maturity alone cannot establish which implementation is faster.
Changes need correctness coverage and measurements against the previous binary.

The [September 24–25 trials](windows-iocp-trials-2026-09-25.md) evaluated two
independent notification candidates. Both passed correctness CI; neither met
the performance retention requirements. Neither runtime change is retained.

The [published matrix](windows-ci-benchmarks-2026-09-23.md) is a starting point.
It used one four-vCPU Windows runner per workload family, with nginx, oha, and
the proxy upstream sharing those CPUs. Its four-worker results include that
contention and cannot establish scaling on four dedicated server CPUs.

## Lessons from production server benchmarks

These primary sources inform the measurement plan:

- The .NET team's [Crank](https://github.com/dotnet/crank) runs continuous
  server benchmarks. Its [getting-started guide](https://github.com/dotnet/crank/blob/main/docs/getting_started.md)
  explicitly recommends separate machines for the application and generator.
  It reports application and generator CPU use along with throughput, errors,
  and latency. Its [architecture](https://github.com/dotnet/crank/blob/main/docs/reference/architecture.md)
  separates scenario jobs and executes each agent's jobs serially.
- [TechEmpower Framework Benchmarks](https://github.com/TechEmpower/FrameworkBenchmarks)
  uses several workloads, including plaintext, serialization, database access,
  and templates. A single plaintext response test does not represent all of
  those workloads or a reverse proxy serving large bodies.
- [wrk2](https://github.com/giltene/wrk2) explains coordinated omission:
  a client that waits for each response before issuing its next request reduces
  offered traffic precisely when the server stalls. Its constant-rate model
  measures latency from the intended request time. It also warns that its
  ten-second calibration makes runs shorter than 10–20 seconds uninformative.
- [oha 1.16.0](https://github.com/hatoo/oha/tree/v1.16.0) supplies fixed-rate
  `-q` and `--latency-correction` options. Those are distinct from its
  fixed-concurrency throughput mode used by this repository's matrix.

For a production capacity claim, use separate generator/server/upstream hosts,
verify generator and network headroom, sweep concurrency and offered rates,
and report throughput, byte rate, CPU cost, errors, and latency together.
Hold CPU budgets, builds, TLS settings, keepalive, caching, logging, response
bodies, and configuration constant. Include overload and recovery, slow peers,
connection churn, and realistic request mixes. These are complementary
experiments; a short saturated loopback run cannot answer all of them.

Fixed-rate latency trials should use common offered rates below, near, and
above the measured capacity of both versions, with sufficient duration and
request count for the claimed percentiles. Compare the same offered rate,
record scheduled and completed requests, and retain errors and deadline drain.
The throughput trials below make no open-loop tail-latency claim.

## CI comparison protocol

[IOCP Performance Trial](../.github/workflows/iocp-performance.yml) builds the
baseline and candidate in one job using the same MinGW toolchain, dependencies,
and optimized compiler flags. The default baseline is
`11d9f5fe1fc9e8be1cb0c505dd168c0d494f57c3`. The manifest records resolved source
commits, executable hashes, versions, runner information, and tool hashes;
the binary artifact also records compiler and package versions.

Each measurement job runs three arms on the same runner: baseline A, baseline
B using the identical executable, and the candidate. Each case has all six
arm permutations over six rounds. Its three measurements are consecutive;
workloads rotate and reverse, worker-count order alternates, and instance
startup order rotates. Instances restart each round. Only one client load is
active at a time. Every sample has a one-second warm-up and a two-second
measurement with 32 HTTP/1.1 keepalive connections. Twelve rounds are also
available without changing the filter policy.

Four jobs cover static/proxy workloads and isolated/shared CPU profiles:

| Profile on a four-vCPU runner | Client CPUs | Upstream CPUs | nginx CPUs | Workers |
| --- | ---: | ---: | ---: | --- |
| Isolated static | 2 | none | 2 | 1, 2 |
| Isolated proxy | 1 | 1 | 2 | 1, 2 |
| Shared static/proxy | whole runner | whole runner when used | whole runner | 1, 4 |

Affinity is applied to the nginx master and every worker, and to the upstream.
The client inherits its mask from process creation. Native helpers verify masks
and process creation identities, and collect CPU time for each client lifetime
and for nginx/upstream over the measured interval. The isolated profile uses
the same two server CPUs for one and two workers, including the master's work.
Shared results remain useful contention tests, with that limitation explicit.

Every family includes small/large bodies, plain HTTP/TLS, and static sendfile
off/on or fast/20 ms delayed proxy responses. All warm-ups and measurements
must have zero errors, only HTTP 200 responses, and consistent body and request
counts. Initial full-body probes also verify content and TLS version. Missing
samples, incorrect order, process exits, or nginx error logs fail the job.
The TLS backpressure lifecycle test runs in the normal Windows correctness
workflow, independently of performance measurements.

## Retention gate

The trial uses the existing [symmetric log/MAD filter](windows-ci-benchmarks.md#order-validation-and-noise-filtering)
without changing its thresholds. Each case also requires:

- A stable A/A comparison within 2% in both execution orders.
- Stable candidate/A and candidate/B matched-round comparisons.
- At least 15% client CPU headroom and, for proxy tests, upstream CPU headroom.
  Client capacity is bounded by both its CPU mask and its two worker threads.
- At least a 2% improvement against both baselines in both orders to establish
  an improvement. Establishing non-regression requires neither comparison nor
  either order to lose more than 2%.

A reproducible loss greater than 2% against both baselines in both orders is a
regression. Unstable controls, resource saturation, or conflicting orders are
inconclusive. The overall gate requires a supported improvement in an isolated
profile and no regression or inconclusive case in any profile. Thresholds are
fixed before measurement. Passing these gates is evidence for the observed
workload, not a statistical significance test or a universal speed ranking.

CPU headroom does not establish network or memory-bandwidth headroom. Affinity
also does not remove shared caches, kernel work, virtualization noise, or the
loopback transport. A failing or inconclusive trial provides diagnostics; it
does not justify a speedup claim or retaining a speculative optimization.

All benchmarks run on CI. Local work may compile affected objects, exercise
focused correctness checks, or recompute reports from downloaded artifacts.
The comparison runner refuses to start outside GitHub Actions. Raw results,
CPU counters, warm-ups, configurations, logs, and gate reasons remain in the
90-day measurement artifacts. Recompute a downloaded result without executing
the workload:

```sh
node misc/iocp-performance-report.js path/to/extracted/artifact
```

## Focused diagnostic profiles

[IOCP Diagnostic Profiles](../.github/workflows/iocp-profile.yml) runs one
selected workload and one event backend per manual dispatch. The default is
a 1 KiB plaintext static response with IOCP and sendfile disabled. Each run
uses one nginx worker, a one-second warm-up, and five seconds of profiling.
Other workloads and backends require separate dispatches; heavy workflows
share a concurrency group and execute serially.

Use saved profiles to choose the next workload. Normalize call counts by
the requests recorded in the same worker profile: the profile includes
startup probes, warm-up, measurement, and shutdown. The measurement-only
request count covers a different interval. Counts can identify repeated
allocation or I/O work, but do not measure its cost. Calls crossing shared
library boundaries and kernel activity are not fully visible in gprof.

Compare a candidate's operation, allocation, and buffer-lifetime behavior
on that path before requesting wider measurements. Instrumented rates do
not satisfy or change the correctness and performance retention gates.

## Evaluated write-notification candidate

OpenSSL's socket BIO sends directly through Winsock. When it returns
`SSL_ERROR_WANT_WRITE`, the IOCP bridge waits for FD_WRITE and posts a completion.
The original bridge creates and destroys a WSA event, thread-pool wait, and
subscription for every blocked write. The candidate retains those resources
for the connection lifetime, while individual operations keep their existing
pool references, cancellation, and completion ownership.

Microsoft's [WSAEventSelect documentation](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect)
specifies that a send returning `WSAEWOULDBLOCK` reenables FD_WRITE notification
when buffer space becomes available. `WSAEnumNetworkEvents` atomically reads
and clears recorded events. Retiring an operation drains callbacks before its
memory is released; cleanup of an old operation cannot disarm a newer one
posted by its completion handler. Connection finalization releases the cache.

The retained correctness test blocks multi-MiB TLS responses, reuses one
keepalive connection, validates complete binary bodies, reloads with a response
in flight, and resets a blocked connection. The trial did not establish the
isolated-profile improvement and complete non-regression evidence needed by
the retention gate. The resource cache is absent from the implementation.

## Evaluated read-notification candidate

The read-readiness bridge posts a zero-byte `WSARecv`, then calls the normal
consumer when its completion arrives. The candidate retained at most one
fully retired operation per connection, holding no pool references while idle.
Only retirement made an operation reusable; a handler could arm a different
operation before the old one finished. Successful zero-byte reads also used
the status in the completed
[`OVERLAPPED`](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-overlapped)
instead of querying Winsock again, while preserving the existing error path.

The retained correctness tests exercise fragmented requests, ordered pipelines
with distinct response bodies, keepalive reuse, and reset with a read pending,
over plaintext and TLS. This independent candidate also failed to meet the
performance retention requirements and is absent from the implementation.

The dated report preserves complete profiles and failed attempts separately.
Client saturation and noisy A/A controls stay inconclusive. The available
four-CPU hosted runners remain useful for finding correctness failures and
detecting substantial changes, but cannot resolve every throughput comparison.

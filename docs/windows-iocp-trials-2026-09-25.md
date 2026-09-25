# Windows IOCP trials, September 24–25, 2026

Neither candidate passed the predeclared CI retention gate. Neither runtime
change is retained. The stronger correctness tests, comparison workflow,
and reporting tools remain. These trials do not establish that IOCP now
beats select or poll; the [last complete backend comparison](windows-ci-benchmarks-2026-09-23.md)
remains the applicable snapshot.

Both candidates use source baseline `11d9f5fe1fc9e8be1cb0c505dd168c0d494f57c3`.
Each trial builds its baseline and candidate together, then measures two
identical baseline copies and the candidate in six balanced rounds.
All benchmarks ran on GitHub-hosted Windows 2022 runners. Local work only
compiled affected objects, checked syntax, and audited downloaded evidence.

## Protocol and limits

The [predeclared protocol](windows-iocp-optimization.md#ci-comparison-protocol)
uses 32 keepalive connections, one-second warm-ups, two-second samples,
matched-round ratios, both execution orders, fixed noise filters, A/A
controls within 2%, and at least 15% client/upstream CPU headroom.
The whole candidate needs an isolated-profile improvement of at least 2%
against both baseline arms in both orders, with no regression or inconclusive
case in any profile. Thresholds were not relaxed after seeing these results.

Each job has four logical CPUs. Isolated static jobs assign two to nginx
and two to the client. Isolated proxy jobs assign two to nginx, one to
the client, and one to the upstream. Shared jobs use the whole runner for
all processes. Isolated multi-worker cases use two workers; shared cases
use four. These are different resource layouts, not measurements on the
same dedicated server. CPU models can also differ between jobs.

Large static sendfile responses saturated the two-thread client in several
profiles. Some isolated proxy loads exhausted their one client CPU.
The headroom gate correctly makes such throughput comparisons inconclusive.
A timeout fails the job even when other requests succeed; it is never removed
as noise. CPU saturation is an observed limitation, not proof of the cause
of an individual timeout. No production capacity or open-loop tail-latency
claim follows from these short loopback measurements.

Tables preserve all complete cases. Rates and deltas have independently
filtered medians; percentages are matched-round ratios, not ratios of the
displayed rates. Parentheses give retained/planned observations. A numeric
delta alone does not establish improvement when the row decision is
inconclusive. Raw and filtered evidence remains in the linked run artifacts.

## Write-notification resources

Run: CI run 35998617079 (historical)

Candidate source: `285db545043e166ea01b9f85d50b5a4c876b5f20`.

Baseline executable SHA256: `393183dfa96af9eef5c82d3087a134e80d24e865e7e27d480009d85a6f155ae9`.

Candidate executable SHA256: `eef701ab81a637e8bd15f101c51f48c2bc2c4f6a262f1e811169e2eaffc2ca78`.

This candidate retains the WSA event, thread-pool wait, and FD_WRITE
subscription across repeated TLS write notifications. Windows
Release/Debug correctness CI
passed, as did integration, smoke, feature, and code-quality workflows.
Performance acceptance failed: no isolated improvement was established.
The complete shared proxy profile has one qualifying row, large TLS
responses with four workers (+6.61%/+7.58% against A/B), but that does
not satisfy the whole-candidate gate.

The isolated proxy job stopped after 142 valid samples because baseline
A had one timeout in a round-three warm-up. Its rerun stopped after
26 valid samples because baseline B had 14 timeouts in a round-one
measurement. Both errors occurred with fast, plaintext, 1 MiB responses
and two nginx workers. Neither partial job supplies a complete comparison.

| Profile | Valid samples | Measured requests | Outcome |
| --- | ---: | ---: | --- |
| proxy-isolated | 142/288 | 1,951,509 | failed; ineligible |
| proxy-shared | 288/288 | 2,819,638 | inconclusive; 1 improvement, 8 unchanged, 7 inconclusive |
| static-isolated | 288/288 | 2,004,011 | inconclusive; 12 unchanged, 4 inconclusive |
| static-shared | 288/288 | 4,080,572 | inconclusive; 8 unchanged, 8 inconclusive |

### Static / isolated

CPU: AMD EPYC 7763 64-Core Processor; 4 logical processors.

| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A | Candidate/A | Candidate/B | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| static-off-1k.bin-plain-w1-iocp | 7,007 (6/6) | 6,998 (6/6) | 6,998 (6/6) | -1.48% (5/6) | +0.03% (6/6) | +0.64% (6/6) | unchanged |
| static-off-1m.bin-plain-w1-iocp | 2,167 (6/6) | 2,180 (6/6) | 2,189 (6/6) | -0.77% (5/6) | +0.46% (6/6) | +1.01% (6/6) | unchanged |
| static-on-1k.bin-plain-w1-iocp | 6,863 (6/6) | 6,808 (6/6) | 6,869 (6/6) | -1.53% (5/6) | +0.63% (6/6) | +1.05% (6/6) | unchanged |
| static-on-1m.bin-plain-w1-iocp | 2,479 (6/6) | 2,431 (6/6) | 2,406 (5/6) | inconclusive (6/6) | -2.42% (5/6) | inconclusive (6/6) | inconclusive |
| static-off-1k.bin-tls-w1-iocp | 5,803 (6/6) | 5,849 (6/6) | 5,811 (6/6) | +0.08% (6/6) | +0.26% (6/6) | +0.20% (5/6) | unchanged |
| static-off-1m.bin-tls-w1-iocp | 877 (6/6) | 874 (6/6) | 876 (6/6) | -0.10% (6/6) | +0.35% (6/6) | -0.16% (6/6) | unchanged |
| static-on-1k.bin-tls-w1-iocp | 5,732 (4/6) | 5,801 (6/6) | 5,831 (6/6) | +0.93% (5/6) | -0.04% (5/6) | +0.37% (6/6) | unchanged |
| static-on-1m.bin-tls-w1-iocp | 878 (6/6) | 872 (6/6) | 885 (6/6) | -0.36% (6/6) | +0.89% (6/6) | +1.34% (6/6) | unchanged |
| static-off-1k.bin-plain-w2-iocp | 4,305 (6/6) | 4,274 (5/6) | 4,239 (6/6) | +0.40% (6/6) | -0.55% (6/6) | -0.66% (6/6) | unchanged |
| static-off-1m.bin-plain-w2-iocp | 2,518 (6/6) | 2,530 (6/6) | 2,512 (6/6) | +0.47% (6/6) | -0.45% (6/6) | -0.88% (6/6) | unchanged |
| static-on-1k.bin-plain-w2-iocp | 4,761 (6/6) | 4,816 (6/6) | 4,757 (6/6) | inconclusive (6/6) | -0.99% (6/6) | -2.45% (6/6) | inconclusive |
| static-on-1m.bin-plain-w2-iocp | 2,423 (6/6) | 2,432 (5/6) | 2,428 (6/6) | +0.12% (6/6) | -0.04% (6/6) | +0.39% (6/6) | inconclusive |
| static-off-1k.bin-tls-w2-iocp | 3,775 (6/6) | 3,777 (6/6) | 3,771 (6/6) | +0.66% (6/6) | -0.12% (6/6) | -0.65% (6/6) | unchanged |
| static-off-1m.bin-tls-w2-iocp | 921 (6/6) | 932 (5/6) | 930 (6/6) | +0.12% (6/6) | +1.32% (6/6) | +0.56% (6/6) | unchanged |
| static-on-1k.bin-tls-w2-iocp | 3,757 (6/6) | 3,766 (6/6) | 3,774 (6/6) | -1.30% (5/6) | +0.36% (6/6) | +0.08% (6/6) | unchanged |
| static-on-1m.bin-tls-w2-iocp | 926 (6/6) | 928 (5/6) | 906 (6/6) | +0.14% (5/6) | -1.51% (6/6) | -1.67% (5/6) | inconclusive |

Gate reasons:

- `static-on-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive; load generator has less than 15% CPU headroom.
- `static-on-1k.bin-plain-w2-iocp`: A/A check is not within 2% in both orders.
- `static-on-1m.bin-plain-w2-iocp`: A/A check is not within 2% in both orders; load generator has less than 15% CPU headroom.
- `static-on-1m.bin-tls-w2-iocp`: execution orders do not establish a 2% non-regression.

### Static / shared

CPU: INTEL(R) XEON(R) PLATINUM 8573C; 4 logical processors.

| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A | Candidate/A | Candidate/B | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| static-off-1k.bin-plain-w1-iocp | 13,484 (6/6) | 13,681 (6/6) | 13,645 (6/6) | +1.34% (6/6) | +0.66% (5/6) | +0.08% (6/6) | unchanged |
| static-off-1m.bin-plain-w1-iocp | inconclusive (3/6) | 2,454 (4/6) | 2,441 (5/6) | inconclusive (4/6) | +0.18% (4/6) | inconclusive (4/6) | inconclusive |
| static-on-1k.bin-plain-w1-iocp | 12,788 (6/6) | 12,749 (6/6) | 12,885 (6/6) | -0.39% (6/6) | +1.51% (5/6) | +0.76% (5/6) | unchanged |
| static-on-1m.bin-plain-w1-iocp | 2,804 (4/6) | 2,759 (4/6) | inconclusive (6/6) | -0.66% (6/6) | +0.21% (6/6) | -0.19% (5/6) | inconclusive |
| static-off-1k.bin-tls-w1-iocp | 12,254 (6/6) | 12,200 (6/6) | 12,020 (6/6) | -0.85% (6/6) | -2.19% (6/6) | -1.12% (6/6) | inconclusive |
| static-off-1m.bin-tls-w1-iocp | 1,446 (6/6) | 1,433 (6/6) | 1,439 (6/6) | -0.92% (6/6) | -0.52% (6/6) | +0.33% (6/6) | unchanged |
| static-on-1k.bin-tls-w1-iocp | 12,290 (6/6) | 12,079 (6/6) | 12,016 (6/6) | -1.59% (6/6) | -2.64% (6/6) | -1.08% (5/6) | inconclusive |
| static-on-1m.bin-tls-w1-iocp | 1,438 (6/6) | 1,446 (6/6) | 1,442 (6/6) | +0.64% (6/6) | +0.19% (6/6) | -0.13% (6/6) | unchanged |
| static-off-1k.bin-plain-w4-iocp | 12,136 (6/6) | 12,161 (6/6) | 12,102 (6/6) | +0.82% (6/6) | -0.28% (6/6) | -0.32% (6/6) | unchanged |
| static-off-1m.bin-plain-w4-iocp | inconclusive (6/6) | inconclusive (5/6) | 2,605 (4/6) | inconclusive (6/6) | +1.27% (6/6) | +2.52% (6/6) | inconclusive |
| static-on-1k.bin-plain-w4-iocp | 11,659 (6/6) | 11,732 (6/6) | 11,680 (5/6) | +0.54% (6/6) | -0.17% (5/6) | -0.75% (5/6) | unchanged |
| static-on-1m.bin-plain-w4-iocp | 2,526 (4/6) | 2,470 (6/6) | inconclusive (6/6) | -0.96% (6/6) | -0.38% (6/6) | +0.18% (6/6) | inconclusive |
| static-off-1k.bin-tls-w4-iocp | 11,422 (6/6) | 11,438 (6/6) | 11,404 (6/6) | -0.44% (6/6) | +0.11% (6/6) | +0.15% (6/6) | unchanged |
| static-off-1m.bin-tls-w4-iocp | inconclusive (6/6) | 1,883 (4/6) | 797 (4/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive |
| static-on-1k.bin-tls-w4-iocp | 11,385 (6/6) | 11,422 (6/6) | 11,484 (6/6) | +0.49% (6/6) | +0.76% (6/6) | +0.10% (6/6) | unchanged |
| static-on-1m.bin-tls-w4-iocp | 1,900 (6/6) | 791 (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive |

Gate reasons:

- `static-off-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `static-on-1m.bin-plain-w1-iocp`: load generator has less than 15% CPU headroom.
- `static-off-1k.bin-tls-w1-iocp`: A/A check is not within 2% in both orders.
- `static-on-1k.bin-tls-w1-iocp`: execution orders do not establish a 2% non-regression.
- `static-off-1m.bin-plain-w4-iocp`: A/A check is not within 2% in both orders.
- `static-on-1m.bin-plain-w4-iocp`: load generator has less than 15% CPU headroom.
- `static-off-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `static-on-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.

### Proxy / isolated

CPU: AMD EPYC 7763 64-Core Processor; 4 logical processors.

The job failed before completing the planned matrix. No row is
eligible for the retention decision.

- `r3-proxy-fast-1m.bin-plain-w2-iocp-baseline-a-warmup.json`: `{"timeout": 1}`.

### Proxy / shared

CPU: AMD EPYC 7763 64-Core Processor; 4 logical processors.

| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A | Candidate/A | Candidate/B | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| proxy-fast-1k.bin-plain-w1-iocp | 13,104 (6/6) | 13,015 (5/6) | 12,997 (6/6) | +0.04% (5/6) | +0.51% (6/6) | inconclusive (6/6) | inconclusive |
| proxy-fast-1m.bin-plain-w1-iocp | 430 (6/6) | 435 (6/6) | 432 (5/6) | +1.56% (6/6) | +0.83% (5/6) | -1.33% (6/6) | inconclusive |
| proxy-slow-1k.bin-plain-w1-iocp | 1,009 (6/6) | 1,009 (6/6) | 1,007 (6/6) | -0.10% (6/6) | -0.18% (6/6) | -0.11% (6/6) | unchanged |
| proxy-slow-1m.bin-plain-w1-iocp | 340 (5/6) | 347 (5/6) | 348 (5/6) | inconclusive (6/6) | inconclusive (6/6) | -2.27% (6/6) | inconclusive |
| proxy-fast-1k.bin-tls-w1-iocp | 10,646 (6/6) | 10,525 (6/6) | 10,576 (6/6) | -1.48% (6/6) | -0.78% (6/6) | +0.17% (6/6) | inconclusive |
| proxy-fast-1m.bin-tls-w1-iocp | 255 (4/6) | 255 (5/6) | 259 (5/6) | +1.08% (6/6) | +2.44% (5/6) | +1.29% (6/6) | unchanged |
| proxy-slow-1k.bin-tls-w1-iocp | 981 (6/6) | 979 (6/6) | 978 (6/6) | -0.12% (6/6) | -0.21% (6/6) | +0.02% (6/6) | unchanged |
| proxy-slow-1m.bin-tls-w1-iocp | 227 (6/6) | inconclusive (6/6) | 233 (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive |
| proxy-fast-1k.bin-plain-w4-iocp | 25,638 (6/6) | 25,929 (6/6) | 25,770 (6/6) | +0.83% (6/6) | +0.64% (6/6) | -0.28% (6/6) | unchanged |
| proxy-fast-1m.bin-plain-w4-iocp | 859 (5/6) | 822 (6/6) | 829 (6/6) | inconclusive (6/6) | -3.00% (6/6) | +0.74% (6/6) | inconclusive |
| proxy-slow-1k.bin-plain-w4-iocp | 1,019 (6/6) | 1,019 (6/6) | 1,020 (6/6) | +0.08% (6/6) | +0.15% (6/6) | +0.15% (6/6) | unchanged |
| proxy-slow-1m.bin-plain-w4-iocp | 769 (5/6) | 766 (6/6) | 773 (5/6) | +0.19% (5/6) | +1.11% (5/6) | +2.14% (5/6) | unchanged |
| proxy-fast-1k.bin-tls-w4-iocp | 20,586 (5/6) | 20,589 (6/6) | 20,673 (6/6) | -0.61% (5/6) | -0.12% (5/6) | +0.47% (6/6) | unchanged |
| proxy-fast-1m.bin-tls-w4-iocp | 321 (6/6) | 319 (6/6) | 343 (6/6) | +0.14% (5/6) | +6.61% (6/6) | +7.58% (5/6) | improvement |
| proxy-slow-1k.bin-tls-w4-iocp | 989 (6/6) | 988 (6/6) | 987 (6/6) | +0.06% (6/6) | -0.27% (6/6) | -0.03% (6/6) | unchanged |
| proxy-slow-1m.bin-tls-w4-iocp | 471 (5/6) | 482 (5/6) | 476 (6/6) | +2.67% (5/6) | +1.80% (5/6) | -0.62% (6/6) | inconclusive |

Gate reasons:

- `proxy-fast-1k.bin-plain-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `proxy-fast-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders.
- `proxy-slow-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `proxy-fast-1k.bin-tls-w1-iocp`: execution orders do not establish a 2% non-regression.
- `proxy-slow-1m.bin-tls-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `proxy-fast-1m.bin-plain-w4-iocp`: A/A check is not within 2% in both orders.
- `proxy-slow-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders.

### Artifact provenance

| Artifact | ID | GitHub-verified ZIP SHA256 |
| --- | ---: | --- |
| iocp-results-proxy-isolated | 10807862443 | `4c6c6991e8c75ae88d96b6d808f0ba1b9830c6c495792e763d7ca51fb4c3ce53` |
| iocp-results-proxy-shared | 10807104926 | `696a7c15ada8355f5c70891de1ae7e55d3527297343a97f8d61b2423103f06c1` |
| iocp-results-static-isolated | 10807903793 | `4fd499620c9a9000ae84a3b0d86279a529b80c7cfaa9bf611325b9eeda6565b7` |
| iocp-results-static-shared | 10808126920 | `3f4bc21a7210f49bf1ba7fb2b3d790d5df498847388acf70d4244ac0cd2f254b` |
| proxy-isolated rerun, attempt 2 | 10843427477 | `8de9d5260e5027734db3194664503c99511b9afa401442d2c2a46465f50feaf8` |

## Retired read notifications

Run: CI run 36085078310 (historical)

Candidate source: `8fdbd1befff6852b9e25e5c91e6d72c7229fd285`.

Baseline executable SHA256: `210fc9c1360f7c91e2c0f4287bcba7c2468b3468a640e05e8903daf72b1ce67c`.

Candidate executable SHA256: `327e3fb73489744fe5c4dae7a2631e6d75c597cea33754066513d4e71a4a5177`.

This independent candidate caches at most one fully retired read
notification per connection and skips a redundant Winsock result query
only for a successful zero-byte receive. It does not include the TLS
resource-cache candidate. Windows
Release/Debug correctness CI
passed, including fragmented and pipelined requests, distinct response
bodies, keepalive reuse, reset with a read pending, TLS backpressure,
and reload. Integration, smoke, feature, and code-quality also passed.

No complete profile produced a qualifying improvement. The isolated
proxy job stopped after 38 valid samples when baseline A had 16
timeouts in the first round, with fast, TLS, 1 MiB responses and two
nginx workers. The partial job is ineligible for comparison.

| Profile | Valid samples | Measured requests | Outcome |
| --- | ---: | ---: | --- |
| proxy-isolated | 38/288 | 780,299 | failed; ineligible |
| proxy-shared | 288/288 | 2,815,833 | inconclusive; 8 unchanged, 8 inconclusive |
| static-isolated | 288/288 | 2,074,306 | inconclusive; 12 unchanged, 4 inconclusive |
| static-shared | 288/288 | 2,647,191 | inconclusive; 10 unchanged, 6 inconclusive |

### Static / isolated

CPU: AMD EPYC 7763 64-Core Processor; 4 logical processors.

| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A | Candidate/A | Candidate/B | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| static-off-1k.bin-plain-w1-iocp | 7,186 (6/6) | 7,125 (6/6) | 7,243 (6/6) | -0.76% (6/6) | +1.31% (5/6) | +1.48% (6/6) | unchanged |
| static-off-1m.bin-plain-w1-iocp | 2,288 (4/6) | 2,290 (6/6) | 2,289 (6/6) | +0.43% (5/6) | +0.09% (4/6) | -0.00% (6/6) | unchanged |
| static-on-1k.bin-plain-w1-iocp | 7,196 (6/6) | 7,251 (6/6) | 7,219 (6/6) | +1.00% (5/6) | +0.26% (6/6) | -0.40% (6/6) | unchanged |
| static-on-1m.bin-plain-w1-iocp | 2,733 (6/6) | 2,706 (6/6) | 2,704 (6/6) | -0.47% (6/6) | -0.76% (6/6) | -0.59% (6/6) | inconclusive |
| static-off-1k.bin-tls-w1-iocp | 6,045 (6/6) | 6,077 (6/6) | 6,070 (6/6) | +0.41% (6/6) | +0.16% (6/6) | -0.42% (6/6) | unchanged |
| static-off-1m.bin-tls-w1-iocp | 905 (6/6) | 913 (6/6) | 914 (6/6) | +0.79% (6/6) | +1.13% (6/6) | +0.27% (6/6) | unchanged |
| static-on-1k.bin-tls-w1-iocp | 6,050 (6/6) | 6,024 (6/6) | 6,043 (6/6) | -0.20% (6/6) | -0.33% (6/6) | -0.34% (6/6) | unchanged |
| static-on-1m.bin-tls-w1-iocp | 905 (6/6) | 909 (6/6) | 912 (6/6) | +0.53% (6/6) | +0.69% (6/6) | +0.22% (6/6) | unchanged |
| static-off-1k.bin-plain-w2-iocp | 4,285 (6/6) | 4,303 (6/6) | 4,284 (6/6) | +0.50% (6/6) | +0.13% (6/6) | -0.32% (6/6) | unchanged |
| static-off-1m.bin-plain-w2-iocp | 2,556 (6/6) | 2,572 (6/6) | 2,562 (6/6) | +0.81% (6/6) | +0.08% (6/6) | -0.25% (6/6) | unchanged |
| static-on-1k.bin-plain-w2-iocp | 4,615 (5/6) | 4,764 (6/6) | 4,518 (5/6) | +1.36% (6/6) | -1.99% (4/6) | -4.91% (6/6) | inconclusive |
| static-on-1m.bin-plain-w2-iocp | 2,682 (4/6) | 2,687 (5/6) | 2,677 (6/6) | +0.20% (5/6) | -0.78% (6/6) | +0.24% (5/6) | inconclusive |
| static-off-1k.bin-tls-w2-iocp | 3,858 (6/6) | 3,831 (6/6) | 3,881 (6/6) | -1.07% (6/6) | -0.49% (5/6) | +0.71% (5/6) | unchanged |
| static-off-1m.bin-tls-w2-iocp | 958 (6/6) | 956 (6/6) | 953 (6/6) | -0.04% (6/6) | +0.63% (6/6) | -0.09% (6/6) | unchanged |
| static-on-1k.bin-tls-w2-iocp | 3,854 (6/6) | 3,852 (6/6) | 3,872 (5/6) | +0.05% (6/6) | +0.20% (5/6) | -0.39% (5/6) | unchanged |
| static-on-1m.bin-tls-w2-iocp | 969 (5/6) | 964 (5/6) | 936 (5/6) | -0.87% (5/6) | inconclusive (3/6) | -2.09% (5/6) | inconclusive |

Gate reasons:

- `static-on-1m.bin-plain-w1-iocp`: load generator has less than 15% CPU headroom.
- `static-on-1k.bin-plain-w2-iocp`: execution orders do not establish a 2% non-regression.
- `static-on-1m.bin-plain-w2-iocp`: load generator has less than 15% CPU headroom.
- `static-on-1m.bin-tls-w2-iocp`: candidate comparison is inconclusive.

### Static / shared

CPU: AMD EPYC 7763 64-Core Processor; 4 logical processors.

| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A | Candidate/A | Candidate/B | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| static-off-1k.bin-plain-w1-iocp | 7,316 (6/6) | 7,370 (6/6) | 7,353 (6/6) | +0.70% (6/6) | +0.43% (6/6) | -0.23% (6/6) | unchanged |
| static-off-1m.bin-plain-w1-iocp | 2,121 (6/6) | 2,131 (6/6) | 2,155 (5/6) | +1.10% (6/6) | +1.25% (6/6) | inconclusive (6/6) | inconclusive |
| static-on-1k.bin-plain-w1-iocp | 7,068 (6/6) | 7,041 (6/6) | 7,054 (6/6) | -0.53% (6/6) | +0.27% (6/6) | +0.17% (6/6) | unchanged |
| static-on-1m.bin-plain-w1-iocp | 2,507 (6/6) | 2,518 (6/6) | 2,545 (6/6) | +1.05% (6/6) | +2.57% (5/6) | +0.15% (6/6) | inconclusive |
| static-off-1k.bin-tls-w1-iocp | 6,178 (6/6) | 6,147 (6/6) | 6,142 (6/6) | -0.18% (5/6) | -0.36% (6/6) | -0.59% (5/6) | unchanged |
| static-off-1m.bin-tls-w1-iocp | 906 (6/6) | 903 (6/6) | 903 (6/6) | +0.03% (6/6) | -0.20% (6/6) | -0.02% (6/6) | unchanged |
| static-on-1k.bin-tls-w1-iocp | 6,114 (6/6) | 6,186 (6/6) | 6,139 (6/6) | +0.01% (6/6) | -0.43% (6/6) | -0.57% (6/6) | unchanged |
| static-on-1m.bin-tls-w1-iocp | 900 (6/6) | 905 (6/6) | 906 (6/6) | +0.01% (6/6) | +0.49% (6/6) | +0.28% (6/6) | unchanged |
| static-off-1k.bin-plain-w4-iocp | 9,002 (6/6) | 8,954 (6/6) | 8,927 (6/6) | -0.32% (6/6) | -0.38% (6/6) | -0.25% (6/6) | unchanged |
| static-off-1m.bin-plain-w4-iocp | inconclusive (6/6) | 2,298 (5/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive |
| static-on-1k.bin-plain-w4-iocp | 8,371 (6/6) | 8,376 (6/6) | 8,395 (6/6) | -0.08% (6/6) | +0.69% (6/6) | +0.39% (6/6) | unchanged |
| static-on-1m.bin-plain-w4-iocp | 2,340 (6/6) | 2,319 (6/6) | 2,346 (6/6) | -0.08% (6/6) | +1.01% (6/6) | +0.38% (6/6) | inconclusive |
| static-off-1k.bin-tls-w4-iocp | 8,026 (6/6) | 8,058 (6/6) | 8,048 (6/6) | +0.29% (6/6) | +0.53% (6/6) | -0.02% (6/6) | unchanged |
| static-off-1m.bin-tls-w4-iocp | inconclusive (6/6) | 575 (4/6) | 583 (5/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive |
| static-on-1k.bin-tls-w4-iocp | 8,024 (6/6) | 8,033 (6/6) | 8,025 (6/6) | -0.29% (6/6) | -0.06% (6/6) | +0.45% (6/6) | unchanged |
| static-on-1m.bin-tls-w4-iocp | 1,069 (4/6) | 1,064 (4/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (6/6) | inconclusive (5/6) | inconclusive |

Gate reasons:

- `static-off-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `static-on-1m.bin-plain-w1-iocp`: load generator has less than 15% CPU headroom.
- `static-off-1m.bin-plain-w4-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `static-on-1m.bin-plain-w4-iocp`: load generator has less than 15% CPU headroom.
- `static-off-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `static-on-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.

### Proxy / isolated

CPU: AMD EPYC 9V74 80-Core Processor; 4 logical processors.

The job failed before completing the planned matrix. No row is
eligible for the retention decision.

- `r1-proxy-fast-1m.bin-tls-w2-iocp-baseline-a.json`: `{"timeout": 16}`.

### Proxy / shared

CPU: AMD EPYC 7763 64-Core Processor; 4 logical processors.

| Case | Baseline A req/s | Baseline B req/s | Candidate req/s | A/A | Candidate/A | Candidate/B | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| proxy-fast-1k.bin-plain-w1-iocp | 12,773 (6/6) | 12,818 (6/6) | 12,979 (6/6) | -2.52% (5/6) | +1.30% (5/6) | +1.88% (6/6) | inconclusive |
| proxy-fast-1m.bin-plain-w1-iocp | 427 (5/6) | 418 (6/6) | 430 (6/6) | -2.58% (6/6) | +1.38% (6/6) | +2.90% (6/6) | inconclusive |
| proxy-slow-1k.bin-plain-w1-iocp | 1,007 (6/6) | 1,007 (6/6) | 1,009 (6/6) | -0.02% (6/6) | +0.11% (6/6) | +0.13% (6/6) | unchanged |
| proxy-slow-1m.bin-plain-w1-iocp | 336 (5/6) | 340 (6/6) | 343 (5/6) | inconclusive (6/6) | +1.58% (4/6) | +1.26% (5/6) | inconclusive |
| proxy-fast-1k.bin-tls-w1-iocp | 10,564 (6/6) | 10,548 (6/6) | 10,577 (6/6) | -0.05% (6/6) | -0.07% (5/6) | +0.84% (6/6) | unchanged |
| proxy-fast-1m.bin-tls-w1-iocp | 266 (6/6) | 266 (6/6) | 262 (6/6) | -0.69% (6/6) | -1.27% (6/6) | -0.76% (6/6) | inconclusive |
| proxy-slow-1k.bin-tls-w1-iocp | 977 (6/6) | 977 (6/6) | 980 (6/6) | +0.11% (6/6) | +0.21% (6/6) | +0.11% (6/6) | unchanged |
| proxy-slow-1m.bin-tls-w1-iocp | inconclusive (6/6) | 232 (6/6) | 234 (6/6) | inconclusive (6/6) | inconclusive (6/6) | -0.62% (5/6) | inconclusive |
| proxy-fast-1k.bin-plain-w4-iocp | 25,508 (5/6) | 25,686 (5/6) | 25,731 (5/6) | +0.30% (5/6) | +0.12% (5/6) | +0.09% (5/6) | unchanged |
| proxy-fast-1m.bin-plain-w4-iocp | 856 (5/6) | 834 (4/6) | 839 (6/6) | +0.75% (5/6) | inconclusive (6/6) | inconclusive (5/6) | inconclusive |
| proxy-slow-1k.bin-plain-w4-iocp | 1,019 (6/6) | 1,021 (6/6) | 1,020 (6/6) | +0.04% (6/6) | +0.15% (6/6) | +0.00% (6/6) | unchanged |
| proxy-slow-1m.bin-plain-w4-iocp | 762 (6/6) | 756 (6/6) | 764 (6/6) | -1.13% (6/6) | +0.66% (6/6) | +1.38% (6/6) | unchanged |
| proxy-fast-1k.bin-tls-w4-iocp | 20,724 (6/6) | 20,622 (6/6) | 20,766 (6/6) | -0.48% (6/6) | -0.23% (5/6) | +1.04% (6/6) | unchanged |
| proxy-fast-1m.bin-tls-w4-iocp | 324 (6/6) | 314 (5/6) | 318 (6/6) | -2.58% (6/6) | -1.07% (6/6) | +0.31% (6/6) | inconclusive |
| proxy-slow-1k.bin-tls-w4-iocp | 991 (6/6) | 988 (6/6) | 989 (6/6) | -0.23% (6/6) | -0.21% (6/6) | +0.18% (6/6) | unchanged |
| proxy-slow-1m.bin-tls-w4-iocp | 479 (6/6) | 463 (5/6) | 476 (6/6) | inconclusive (4/6) | inconclusive (6/6) | inconclusive (4/6) | inconclusive |

Gate reasons:

- `proxy-fast-1k.bin-plain-w1-iocp`: A/A check is not within 2% in both orders.
- `proxy-fast-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders.
- `proxy-slow-1m.bin-plain-w1-iocp`: A/A check is not within 2% in both orders.
- `proxy-fast-1m.bin-tls-w1-iocp`: execution orders do not establish a 2% non-regression.
- `proxy-slow-1m.bin-tls-w1-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.
- `proxy-fast-1m.bin-plain-w4-iocp`: candidate comparison is inconclusive.
- `proxy-fast-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders.
- `proxy-slow-1m.bin-tls-w4-iocp`: A/A check is not within 2% in both orders; candidate comparison is inconclusive.

### Artifact provenance

| Artifact | ID | GitHub-verified ZIP SHA256 |
| --- | ---: | --- |
| iocp-results-proxy-isolated | 10842559791 | `f9a9b7a273ddf10a1cf8e875976f47c2c67febad71f8b19bba63e0f7ad1a3f17` |
| iocp-results-proxy-shared | 10843757840 | `d7e8c7fab50568df3085c1cbfaa86c2d04e7963ba068b4f43abb11cba0ab0f05` |
| iocp-results-static-isolated | 10843902379 | `675887d9c7d08b0a8f6b89a2cf981d0f00b99c723e33063b16e3a5033027b509` |
| iocp-results-static-shared | 10843807670 | `a9c0434140726c4a55b6eda883f975f393d2e9156dcd672d60889b7e216456d5` |

## Verification and retained changes

Downloaded ZIP digests match the GitHub artifact metadata. For each complete
profile, recalculating the report reproduces both summary files exactly.
Normalized samples match their raw oha counts, rates, byte totals, and latency
fields. Warm-ups, full-body probes, TLS versions, client affinity, and CPU
counters were also checked. Failed jobs and their raw errors remain recorded.

The retained changes provide the same-host A/A/candidate workflow, documented
CPU limits and production-benchmark references, normal CI coverage of the
report gates, and the stronger HTTP/TLS lifecycle tests. Failed measurements
now also preserve the client watcher output before validating responses.
The performance thresholds and zero-error requirement remain unchanged.

Similar allocation and status-query ideas were tested in earlier local
[historical experiments](win32-iocp-performance.md#august-21-2026-reference-driven-receive-work).
Those historical measurements did not decide acceptance here: these trials
used the current source, hosted CI, independent baseline copies, and the
new resource and ordering controls.

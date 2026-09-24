# Windows CI benchmark results (September 23, 2026)

Run 35859988384,
at source 42b2436f0e9dab23d4b182faf0894f2c48a7f057,
completed all 96 cases. The measured source is nginx 1.31.6 with the
shared-listener accept guard added after the v1.31.6 release. All six source
workflows passed, including Windows Release/Debug and lifecycle regression
coverage. The benchmark build also passed all 96 smoke cases at 32 connections.

The quick suite used six two-second measurements per case, each after a
one-second warm-up, with 32 HTTP/1.1 keepalive connections and two oha 1.16.0
threads. All 576 measurements and 576 warm-ups returned only HTTP 200, with
zero errors and the expected response size/count. Measured requests totaled
6,421,311. Raw response validation, backend/worker startup logs, body hashes,
TLS probes, and recomputed summaries were checked against the artifacts.

| Workload job | CPU reported by runner | Logical CPUs | UTC measurements |
| --- | --- | ---: | --- |
| Static | AMD EPYC 9V74 | 4 | 12:26:19 to 12:43:24 |
| Proxy | AMD EPYC 7763 | 4 | 12:26:21 to 12:43:42 |

Both Windows 2022 jobs had about 16 GiB RAM and ran Windows build 20348 and
Node v22.23.2. They used the same nginx executable, built with GCC 16.2.0,
OpenSSL 3.6.4, and `-O2 -march=x86-64 -mtune=generic -DFD_SETSIZE=1024`.
Static and proxy rates come from separate hosts; comparisons stay within a
workload family. The client, nginx, and proxy upstream share the runner CPUs.

## Reading the results

Rates and paired ratios use medians in log space. Each rate is rounded to a
whole request/second; its count is retained/raw samples. Paired changes are
`100 * (median matched-round ratio - 1)`, filtered independently of the rates.
A positive change means a higher request rate for the candidate. A stable
comparison can coexist with an inconclusive absolute rate because ratios and
rates have separate dispersion gates. Do not derive deltas from rounded or
separately filtered rate medians.

The [method and fixed noise policy](windows-ci-benchmarks.md) require at
least four of six samples, scaled MAD at most 5%, and, for paired comparisons,
at least two pairs in each execution order with order medians within 5%.
Log-space outlier distance is `max(log(1.03), 3 * 1.4826 * MAD)`.
Busy-host and excessive-drain gates also apply. No thresholds were changed
after collection. HTTP failures are test failures and cannot be filtered as noise.

| Family | Measurements | Stable rates | Absolute-rate outliers | Stable comparisons |
| --- | ---: | ---: | ---: | ---: |
| Static | 288 | 45/48 | 13 | 90/104 |
| Proxy | 288 | 47/48 | 13 | 99/104 |
| Total | 576 | 92/96 | 26 | 189/208 |

No sample was excluded by the busy-host or drain gates in this run. The 26
absolute-rate outliers and the independent paired exclusions remain recorded
in `summary.json`; all raw samples remain in the artifacts. Four rates and
19 comparisons are inconclusive. Their reasons are listed below. Passing
these stability gates is not a statistical significance test.

Static mode means the configured `sendfile` directive. TLS uses the OpenSSL
output path, so this directive does not imply zero-copy TLS. Proxy mode is a
fast upstream or a 20 ms delay before response headers; bodies are generated
in memory and are not rate limited. Proxy buffering and caching are disabled.
TLS 1.3 (`TLS_AES_128_GCM_SHA256`) applies to the client-facing connection;
the upstream connection uses plaintext HTTP/1.1.

The full tables below preserve every rate and comparison from the generated
artifact summaries. Case IDs encode family, sendfile/upstream mode, file size,
transport, worker count, and backend. `1k.bin` is 1 KiB and `1m.bin` is 1 MiB;
`plain` is HTTP and `tls` is HTTPS. Comparisons cover backend, worker count,
TLS, and sendfile or upstream delay at each response size. See the
[README](../README.md#performance) for selected observations.

## Static files

| sendfile | Size | TLS | Workers | IOCP req/s | Select req/s | Poll req/s |
|---|---|---|---:|---:|---:|---:|
| off | 1 KiB | off | 1 | 8740 (6/6) | 10116 (5/6) | 10110 (6/6) |
| off | 1 KiB | off | 4 | 10787 (6/6) | 12234 (6/6) | 12284 (6/6) |
| off | 1 KiB | on | 1 | 7167 (6/6) | 8129 (6/6) | 8077 (6/6) |
| off | 1 KiB | on | 4 | 9574 (6/6) | 10917 (6/6) | 10849 (6/6) |
| off | 1 MiB | off | 1 | 1909 (6/6) | 1993 (6/6) | 1952 (5/6) |
| off | 1 MiB | off | 4 | inconclusive (5/6) | 3258 (6/6) | 3251 (6/6) |
| off | 1 MiB | on | 1 | 851 (5/6) | 867 (6/6) | 870 (6/6) |
| off | 1 MiB | on | 4 | 710 (4/6) | inconclusive (6/6) | inconclusive (6/6) |
| on | 1 KiB | off | 1 | 8366 (5/6) | 10095 (6/6) | 10108 (6/6) |
| on | 1 KiB | off | 4 | 10096 (6/6) | 12290 (6/6) | 12287 (6/6) |
| on | 1 KiB | on | 1 | 7082 (5/6) | 8085 (6/6) | 8052 (6/6) |
| on | 1 KiB | on | 4 | 9593 (6/6) | 10855 (6/6) | 10846 (6/6) |
| on | 1 MiB | off | 1 | 2915 (6/6) | 1984 (6/6) | 1957 (6/6) |
| on | 1 MiB | off | 4 | 2641 (6/6) | 3249 (4/6) | 3276 (6/6) |
| on | 1 MiB | on | 1 | 850 (6/6) | 868 (6/6) | 864 (6/6) |
| on | 1 MiB | on | 4 | 699 (4/6) | 971 (5/6) | 1022 (6/6) |

### Paired comparisons

Each change is the median ratio within matched rounds, not a ratio of the table medians.

| Dimension | Candidate | Control | Change | Pairs | MAD |
|---|---|---|---:|---:|---:|
| backend | static-off-1k.bin-plain-w1-iocp | static-off-1k.bin-plain-w1-select | -12.96% | 6/6 | 1.57% |
| backend | static-off-1k.bin-plain-w1-iocp | static-off-1k.bin-plain-w1-poll | -13.21% | 6/6 | 1.57% |
| backend | static-off-1m.bin-plain-w1-iocp | static-off-1m.bin-plain-w1-select | -3.68% | 6/6 | 1.14% |
| backend | static-off-1m.bin-plain-w1-iocp | static-off-1m.bin-plain-w1-poll | -2.59% | 5/6 | 1.34% |
| backend | static-on-1k.bin-plain-w1-iocp | static-on-1k.bin-plain-w1-select | -17.35% | 6/6 | 2.07% |
| backend | static-on-1k.bin-plain-w1-iocp | static-on-1k.bin-plain-w1-poll | -17.68% | 6/6 | 1.54% |
| sendfile | static-on-1k.bin-plain-w1-iocp | static-off-1k.bin-plain-w1-iocp | -4.67% | 6/6 | 2.73% |
| backend | static-on-1m.bin-plain-w1-iocp | static-on-1m.bin-plain-w1-select | +46.38% | 6/6 | 1.82% |
| backend | static-on-1m.bin-plain-w1-iocp | static-on-1m.bin-plain-w1-poll | +48.29% | 6/6 | 2.12% |
| sendfile | static-on-1m.bin-plain-w1-iocp | static-off-1m.bin-plain-w1-iocp | +52.01% | 6/6 | 1.83% |
| backend | static-off-1k.bin-tls-w1-iocp | static-off-1k.bin-tls-w1-select | -11.64% | 6/6 | 1.35% |
| backend | static-off-1k.bin-tls-w1-iocp | static-off-1k.bin-tls-w1-poll | -11.39% | 5/6 | 0.53% |
| tls | static-off-1k.bin-tls-w1-iocp | static-off-1k.bin-plain-w1-iocp | -18.12% | 6/6 | 1.78% |
| backend | static-off-1m.bin-tls-w1-iocp | static-off-1m.bin-tls-w1-select | -2.62% | 5/6 | 0.41% |
| backend | static-off-1m.bin-tls-w1-iocp | static-off-1m.bin-tls-w1-poll | -2.14% | 5/6 | 0.22% |
| tls | static-off-1m.bin-tls-w1-iocp | static-off-1m.bin-plain-w1-iocp | -55.71% | 5/6 | 0.70% |
| backend | static-on-1k.bin-tls-w1-iocp | static-on-1k.bin-tls-w1-select | -12.43% | 5/6 | 1.11% |
| backend | static-on-1k.bin-tls-w1-iocp | static-on-1k.bin-tls-w1-poll | -11.53% | 5/6 | 1.35% |
| tls | static-on-1k.bin-tls-w1-iocp | static-on-1k.bin-plain-w1-iocp | -15.30% | 6/6 | 3.68% |
| sendfile | static-on-1k.bin-tls-w1-iocp | static-off-1k.bin-tls-w1-iocp | -0.62% | 5/6 | 1.92% |
| backend | static-on-1m.bin-tls-w1-iocp | static-on-1m.bin-tls-w1-select | -2.46% | 6/6 | 0.52% |
| backend | static-on-1m.bin-tls-w1-iocp | static-on-1m.bin-tls-w1-poll | -1.86% | 6/6 | 0.95% |
| tls | static-on-1m.bin-tls-w1-iocp | static-on-1m.bin-plain-w1-iocp | -70.77% | 6/6 | 1.85% |
| sendfile | static-on-1m.bin-tls-w1-iocp | static-off-1m.bin-tls-w1-iocp | -0.14% | 5/6 | 0.35% |
| backend | static-off-1k.bin-plain-w4-iocp | static-off-1k.bin-plain-w4-select | -11.75% | 6/6 | 0.40% |
| backend | static-off-1k.bin-plain-w4-iocp | static-off-1k.bin-plain-w4-poll | -11.98% | 6/6 | 0.33% |
| workers | static-off-1k.bin-plain-w4-iocp | static-off-1k.bin-plain-w1-iocp | +23.42% | 6/6 | 1.45% |
| backend | static-off-1m.bin-plain-w4-iocp | static-off-1m.bin-plain-w4-select | -26.58% | 5/6 | 4.81% |
| backend | static-off-1m.bin-plain-w4-iocp | static-off-1m.bin-plain-w4-poll | -28.20% | 5/6 | 3.83% |
| workers | static-off-1m.bin-plain-w4-iocp | static-off-1m.bin-plain-w1-iocp | +21.78% | 5/6 | 4.21% |
| backend | static-on-1k.bin-plain-w4-iocp | static-on-1k.bin-plain-w4-select | -17.87% | 6/6 | 0.66% |
| backend | static-on-1k.bin-plain-w4-iocp | static-on-1k.bin-plain-w4-poll | -17.65% | 6/6 | 0.45% |
| workers | static-on-1k.bin-plain-w4-iocp | static-on-1k.bin-plain-w1-iocp | +21.35% | 6/6 | 1.84% |
| sendfile | static-on-1k.bin-plain-w4-iocp | static-off-1k.bin-plain-w4-iocp | -6.26% | 6/6 | 1.03% |
| backend | static-on-1m.bin-plain-w4-iocp | static-on-1m.bin-plain-w4-select | -19.78% | 6/6 | 2.76% |
| backend | static-on-1m.bin-plain-w4-iocp | static-on-1m.bin-plain-w4-poll | -18.08% | 5/6 | 1.30% |
| workers | static-on-1m.bin-plain-w4-iocp | static-on-1m.bin-plain-w1-iocp | -9.11% | 6/6 | 3.07% |
| sendfile | static-on-1m.bin-plain-w4-iocp | static-off-1m.bin-plain-w4-iocp | inconclusive | 5/6 | 5.16% |
| backend | static-off-1k.bin-tls-w4-iocp | static-off-1k.bin-tls-w4-select | -12.09% | 6/6 | 1.01% |
| backend | static-off-1k.bin-tls-w4-iocp | static-off-1k.bin-tls-w4-poll | -11.85% | 6/6 | 0.91% |
| workers | static-off-1k.bin-tls-w4-iocp | static-off-1k.bin-tls-w1-iocp | +33.37% | 6/6 | 0.34% |
| tls | static-off-1k.bin-tls-w4-iocp | static-off-1k.bin-plain-w4-iocp | -10.91% | 6/6 | 1.16% |
| backend | static-off-1m.bin-tls-w4-iocp | static-off-1m.bin-tls-w4-select | inconclusive | 6/6 | 22.82% |
| backend | static-off-1m.bin-tls-w4-iocp | static-off-1m.bin-tls-w4-poll | inconclusive | 6/6 | 28.41% |
| workers | static-off-1m.bin-tls-w4-iocp | static-off-1m.bin-tls-w1-iocp | -14.33% | 4/6 | 1.67% |
| tls | static-off-1m.bin-tls-w4-iocp | static-off-1m.bin-plain-w4-iocp | inconclusive | 6/6 | 21.93% |
| backend | static-on-1k.bin-tls-w4-iocp | static-on-1k.bin-tls-w4-select | -11.78% | 6/6 | 0.76% |
| backend | static-on-1k.bin-tls-w4-iocp | static-on-1k.bin-tls-w4-poll | -11.64% | 6/6 | 0.79% |
| workers | static-on-1k.bin-tls-w4-iocp | static-on-1k.bin-tls-w1-iocp | +35.74% | 6/6 | 2.01% |
| tls | static-on-1k.bin-tls-w4-iocp | static-on-1k.bin-plain-w4-iocp | -5.10% | 6/6 | 1.12% |
| sendfile | static-on-1k.bin-tls-w4-iocp | static-off-1k.bin-tls-w4-iocp | +0.22% | 6/6 | 1.29% |
| backend | static-on-1m.bin-tls-w4-iocp | static-on-1m.bin-tls-w4-select | inconclusive | 6/6 | 19.50% |
| backend | static-on-1m.bin-tls-w4-iocp | static-on-1m.bin-tls-w4-poll | -32.74% | 4/6 | 4.49% |
| workers | static-on-1m.bin-tls-w4-iocp | static-on-1m.bin-tls-w1-iocp | inconclusive | 4/6 | 1.09% |
| tls | static-on-1m.bin-tls-w4-iocp | static-on-1m.bin-plain-w4-iocp | inconclusive | 4/6 | 1.20% |
| sendfile | static-on-1m.bin-tls-w4-iocp | static-off-1m.bin-tls-w4-iocp | -0.81% | 4/6 | 1.79% |
| sendfile | static-on-1k.bin-plain-w1-select | static-off-1k.bin-plain-w1-select | -0.27% | 5/6 | 0.41% |
| sendfile | static-on-1m.bin-plain-w1-select | static-off-1m.bin-plain-w1-select | +0.15% | 6/6 | 0.93% |
| tls | static-off-1k.bin-tls-w1-select | static-off-1k.bin-plain-w1-select | -19.49% | 5/6 | 0.36% |
| tls | static-off-1m.bin-tls-w1-select | static-off-1m.bin-plain-w1-select | -56.27% | 6/6 | 0.48% |
| tls | static-on-1k.bin-tls-w1-select | static-on-1k.bin-plain-w1-select | -19.71% | 6/6 | 0.60% |
| sendfile | static-on-1k.bin-tls-w1-select | static-off-1k.bin-tls-w1-select | -0.25% | 6/6 | 0.89% |
| tls | static-on-1m.bin-tls-w1-select | static-on-1m.bin-plain-w1-select | -56.36% | 6/6 | 0.41% |
| sendfile | static-on-1m.bin-tls-w1-select | static-off-1m.bin-tls-w1-select | -0.17% | 6/6 | 0.51% |
| workers | static-off-1k.bin-plain-w4-select | static-off-1k.bin-plain-w1-select | +21.09% | 5/6 | 0.34% |
| workers | static-off-1m.bin-plain-w4-select | static-off-1m.bin-plain-w1-select | +64.61% | 5/6 | 0.64% |
| workers | static-on-1k.bin-plain-w4-select | static-on-1k.bin-plain-w1-select | +21.58% | 6/6 | 0.57% |
| sendfile | static-on-1k.bin-plain-w4-select | static-off-1k.bin-plain-w4-select | +0.36% | 6/6 | 0.46% |
| workers | static-on-1m.bin-plain-w4-select | static-on-1m.bin-plain-w1-select | +64.49% | 6/6 | 1.52% |
| sendfile | static-on-1m.bin-plain-w4-select | static-off-1m.bin-plain-w4-select | -0.31% | 5/6 | 1.32% |
| workers | static-off-1k.bin-tls-w4-select | static-off-1k.bin-tls-w1-select | +34.22% | 6/6 | 1.28% |
| tls | static-off-1k.bin-tls-w4-select | static-off-1k.bin-plain-w4-select | -10.99% | 6/6 | 0.55% |
| workers | static-off-1m.bin-tls-w4-select | static-off-1m.bin-tls-w1-select | inconclusive | 6/6 | 6.75% |
| tls | static-off-1m.bin-tls-w4-select | static-off-1m.bin-plain-w4-select | inconclusive | 6/6 | 6.54% |
| workers | static-on-1k.bin-tls-w4-select | static-on-1k.bin-tls-w1-select | +34.18% | 6/6 | 0.30% |
| tls | static-on-1k.bin-tls-w4-select | static-on-1k.bin-plain-w4-select | -11.63% | 6/6 | 0.37% |
| sendfile | static-on-1k.bin-tls-w4-select | static-off-1k.bin-tls-w4-select | -0.69% | 6/6 | 0.50% |
| workers | static-on-1m.bin-tls-w4-select | static-on-1m.bin-tls-w1-select | +12.95% | 5/6 | 2.98% |
| tls | static-on-1m.bin-tls-w4-select | static-on-1m.bin-plain-w4-select | -70.73% | 5/6 | 3.58% |
| sendfile | static-on-1m.bin-tls-w4-select | static-off-1m.bin-tls-w4-select | inconclusive | 5/6 | 8.63% |
| sendfile | static-on-1k.bin-plain-w1-poll | static-off-1k.bin-plain-w1-poll | +0.12% | 6/6 | 0.66% |
| sendfile | static-on-1m.bin-plain-w1-poll | static-off-1m.bin-plain-w1-poll | +0.25% | 5/6 | 0.91% |
| tls | static-off-1k.bin-tls-w1-poll | static-off-1k.bin-plain-w1-poll | -19.80% | 6/6 | 1.86% |
| tls | static-off-1m.bin-tls-w1-poll | static-off-1m.bin-plain-w1-poll | -55.72% | 5/6 | 1.35% |
| tls | static-on-1k.bin-tls-w1-poll | static-on-1k.bin-plain-w1-poll | -20.30% | 6/6 | 0.44% |
| sendfile | static-on-1k.bin-tls-w1-poll | static-off-1k.bin-tls-w1-poll | -0.44% | 6/6 | 1.47% |
| tls | static-on-1m.bin-tls-w1-poll | static-on-1m.bin-plain-w1-poll | -55.90% | 6/6 | 0.50% |
| sendfile | static-on-1m.bin-tls-w1-poll | static-off-1m.bin-tls-w1-poll | -0.43% | 6/6 | 0.77% |
| workers | static-off-1k.bin-plain-w4-poll | static-off-1k.bin-plain-w1-poll | +21.74% | 6/6 | 0.94% |
| workers | static-off-1m.bin-plain-w4-poll | static-off-1m.bin-plain-w1-poll | +65.25% | 5/6 | 0.90% |
| workers | static-on-1k.bin-plain-w4-poll | static-on-1k.bin-plain-w1-poll | +21.88% | 6/6 | 0.74% |
| sendfile | static-on-1k.bin-plain-w4-poll | static-off-1k.bin-plain-w4-poll | -0.03% | 6/6 | 1.05% |
| workers | static-on-1m.bin-plain-w4-poll | static-on-1m.bin-plain-w1-poll | +68.07% | 6/6 | 3.01% |
| sendfile | static-on-1m.bin-plain-w4-poll | static-off-1m.bin-plain-w4-poll | +1.15% | 5/6 | 0.77% |
| workers | static-off-1k.bin-tls-w4-poll | static-off-1k.bin-tls-w1-poll | +34.52% | 6/6 | 2.06% |
| tls | static-off-1k.bin-tls-w4-poll | static-off-1k.bin-plain-w4-poll | -11.81% | 6/6 | 0.74% |
| workers | static-off-1m.bin-tls-w4-poll | static-off-1m.bin-tls-w1-poll | inconclusive | 6/6 | 8.05% |
| tls | static-off-1m.bin-tls-w4-poll | static-off-1m.bin-plain-w4-poll | inconclusive | 6/6 | 7.98% |
| workers | static-on-1k.bin-tls-w4-poll | static-on-1k.bin-tls-w1-poll | +34.48% | 6/6 | 0.90% |
| tls | static-on-1k.bin-tls-w4-poll | static-on-1k.bin-plain-w4-poll | -11.68% | 6/6 | 0.79% |
| sendfile | static-on-1k.bin-tls-w4-poll | static-off-1k.bin-tls-w4-poll | -0.17% | 6/6 | 1.18% |
| workers | static-on-1m.bin-tls-w4-poll | static-on-1m.bin-tls-w1-poll | +17.85% | 6/6 | 4.27% |
| tls | static-on-1m.bin-tls-w4-poll | static-on-1m.bin-plain-w4-poll | inconclusive | 6/6 | 5.62% |
| sendfile | static-on-1m.bin-tls-w4-poll | static-off-1m.bin-tls-w4-poll | inconclusive | 6/6 | 6.35% |

## HTTP proxy

| Upstream | Size | TLS | Workers | IOCP req/s | Select req/s | Poll req/s |
|---|---|---|---:|---:|---:|---:|
| fast | 1 KiB | off | 1 | 12987 (6/6) | 18066 (5/6) | 17954 (6/6) |
| fast | 1 KiB | off | 4 | 25768 (5/6) | 27083 (6/6) | 27040 (5/6) |
| fast | 1 KiB | on | 1 | 10825 (6/6) | 12974 (6/6) | 13154 (5/6) |
| fast | 1 KiB | on | 4 | 20653 (6/6) | 21748 (6/6) | 21656 (5/6) |
| fast | 1 MiB | off | 1 | 430 (6/6) | 436 (6/6) | 433 (6/6) |
| fast | 1 MiB | off | 4 | 825 (6/6) | 903 (6/6) | 896 (5/6) |
| fast | 1 MiB | on | 1 | 258 (5/6) | 264 (6/6) | 263 (6/6) |
| fast | 1 MiB | on | 4 | 326 (6/6) | inconclusive (6/6) | 545 (4/6) |
| slow | 1 KiB | off | 1 | 1013 (6/6) | 1007 (6/6) | 1008 (6/6) |
| slow | 1 KiB | off | 4 | 1017 (5/6) | 1018 (6/6) | 1017 (6/6) |
| slow | 1 KiB | on | 1 | 973 (6/6) | 974 (6/6) | 983 (6/6) |
| slow | 1 KiB | on | 4 | 986 (6/6) | 987 (6/6) | 987 (6/6) |
| slow | 1 MiB | off | 1 | 342 (6/6) | 340 (6/6) | 339 (5/6) |
| slow | 1 MiB | off | 4 | 763 (6/6) | 758 (6/6) | 771 (6/6) |
| slow | 1 MiB | on | 1 | 228 (5/6) | 226 (6/6) | 230 (6/6) |
| slow | 1 MiB | on | 4 | 471 (5/6) | 490 (6/6) | 483 (6/6) |

### Paired comparisons

Each change is the median ratio within matched rounds, not a ratio of the table medians.

| Dimension | Candidate | Control | Change | Pairs | MAD |
|---|---|---|---:|---:|---:|
| backend | proxy-fast-1k.bin-plain-w1-iocp | proxy-fast-1k.bin-plain-w1-select | -28.17% | 5/6 | 2.63% |
| backend | proxy-fast-1k.bin-plain-w1-iocp | proxy-fast-1k.bin-plain-w1-poll | inconclusive | 6/6 | 4.74% |
| backend | proxy-fast-1m.bin-plain-w1-iocp | proxy-fast-1m.bin-plain-w1-select | -1.14% | 6/6 | 2.18% |
| backend | proxy-fast-1m.bin-plain-w1-iocp | proxy-fast-1m.bin-plain-w1-poll | -0.61% | 6/6 | 2.80% |
| backend | proxy-slow-1k.bin-plain-w1-iocp | proxy-slow-1k.bin-plain-w1-select | +0.49% | 6/6 | 0.38% |
| backend | proxy-slow-1k.bin-plain-w1-iocp | proxy-slow-1k.bin-plain-w1-poll | +0.48% | 6/6 | 1.02% |
| upstream delay | proxy-slow-1k.bin-plain-w1-iocp | proxy-fast-1k.bin-plain-w1-iocp | -92.20% | 6/6 | 2.11% |
| backend | proxy-slow-1m.bin-plain-w1-iocp | proxy-slow-1m.bin-plain-w1-select | +0.63% | 6/6 | 4.04% |
| backend | proxy-slow-1m.bin-plain-w1-iocp | proxy-slow-1m.bin-plain-w1-poll | +0.64% | 6/6 | 3.90% |
| upstream delay | proxy-slow-1m.bin-plain-w1-iocp | proxy-fast-1m.bin-plain-w1-iocp | inconclusive | 4/6 | 0.26% |
| backend | proxy-fast-1k.bin-tls-w1-iocp | proxy-fast-1k.bin-tls-w1-select | -15.89% | 5/6 | 0.29% |
| backend | proxy-fast-1k.bin-tls-w1-iocp | proxy-fast-1k.bin-tls-w1-poll | -18.60% | 6/6 | 3.87% |
| tls | proxy-fast-1k.bin-tls-w1-iocp | proxy-fast-1k.bin-plain-w1-iocp | -17.18% | 6/6 | 3.30% |
| backend | proxy-fast-1m.bin-tls-w1-iocp | proxy-fast-1m.bin-tls-w1-select | -2.82% | 6/6 | 1.59% |
| backend | proxy-fast-1m.bin-tls-w1-iocp | proxy-fast-1m.bin-tls-w1-poll | -2.67% | 6/6 | 0.79% |
| tls | proxy-fast-1m.bin-tls-w1-iocp | proxy-fast-1m.bin-plain-w1-iocp | -40.57% | 6/6 | 2.64% |
| backend | proxy-slow-1k.bin-tls-w1-iocp | proxy-slow-1k.bin-tls-w1-select | -0.31% | 6/6 | 0.58% |
| backend | proxy-slow-1k.bin-tls-w1-iocp | proxy-slow-1k.bin-tls-w1-poll | -0.89% | 6/6 | 0.76% |
| tls | proxy-slow-1k.bin-tls-w1-iocp | proxy-slow-1k.bin-plain-w1-iocp | -3.96% | 6/6 | 0.41% |
| upstream delay | proxy-slow-1k.bin-tls-w1-iocp | proxy-fast-1k.bin-tls-w1-iocp | -91.02% | 6/6 | 3.28% |
| backend | proxy-slow-1m.bin-tls-w1-iocp | proxy-slow-1m.bin-tls-w1-select | +1.14% | 5/6 | 3.22% |
| backend | proxy-slow-1m.bin-tls-w1-iocp | proxy-slow-1m.bin-tls-w1-poll | -1.88% | 5/6 | 2.39% |
| tls | proxy-slow-1m.bin-tls-w1-iocp | proxy-slow-1m.bin-plain-w1-iocp | -32.82% | 5/6 | 2.60% |
| upstream delay | proxy-slow-1m.bin-tls-w1-iocp | proxy-fast-1m.bin-tls-w1-iocp | -9.89% | 5/6 | 1.84% |
| backend | proxy-fast-1k.bin-plain-w4-iocp | proxy-fast-1k.bin-plain-w4-select | -4.68% | 5/6 | 0.76% |
| backend | proxy-fast-1k.bin-plain-w4-iocp | proxy-fast-1k.bin-plain-w4-poll | -6.27% | 6/6 | 2.21% |
| workers | proxy-fast-1k.bin-plain-w4-iocp | proxy-fast-1k.bin-plain-w1-iocp | +95.09% | 6/6 | 3.35% |
| backend | proxy-fast-1m.bin-plain-w4-iocp | proxy-fast-1m.bin-plain-w4-select | -8.95% | 6/6 | 3.48% |
| backend | proxy-fast-1m.bin-plain-w4-iocp | proxy-fast-1m.bin-plain-w4-poll | inconclusive | 6/6 | 7.13% |
| workers | proxy-fast-1m.bin-plain-w4-iocp | proxy-fast-1m.bin-plain-w1-iocp | inconclusive | 6/6 | 5.26% |
| backend | proxy-slow-1k.bin-plain-w4-iocp | proxy-slow-1k.bin-plain-w4-select | -0.15% | 5/6 | 0.11% |
| backend | proxy-slow-1k.bin-plain-w4-iocp | proxy-slow-1k.bin-plain-w4-poll | -0.06% | 5/6 | 0.08% |
| workers | proxy-slow-1k.bin-plain-w4-iocp | proxy-slow-1k.bin-plain-w1-iocp | +0.44% | 5/6 | 0.37% |
| upstream delay | proxy-slow-1k.bin-plain-w4-iocp | proxy-fast-1k.bin-plain-w4-iocp | -96.05% | 5/6 | 0.70% |
| backend | proxy-slow-1m.bin-plain-w4-iocp | proxy-slow-1m.bin-plain-w4-select | +1.15% | 6/6 | 1.52% |
| backend | proxy-slow-1m.bin-plain-w4-iocp | proxy-slow-1m.bin-plain-w4-poll | -1.25% | 6/6 | 0.78% |
| workers | proxy-slow-1m.bin-plain-w4-iocp | proxy-slow-1m.bin-plain-w1-iocp | +121.83% | 6/6 | 3.29% |
| upstream delay | proxy-slow-1m.bin-plain-w4-iocp | proxy-fast-1m.bin-plain-w4-iocp | inconclusive | 6/6 | 4.20% |
| backend | proxy-fast-1k.bin-tls-w4-iocp | proxy-fast-1k.bin-tls-w4-select | -4.78% | 6/6 | 1.77% |
| backend | proxy-fast-1k.bin-tls-w4-iocp | proxy-fast-1k.bin-tls-w4-poll | -4.21% | 5/6 | 1.54% |
| workers | proxy-fast-1k.bin-tls-w4-iocp | proxy-fast-1k.bin-tls-w1-iocp | +92.53% | 6/6 | 1.33% |
| tls | proxy-fast-1k.bin-tls-w4-iocp | proxy-fast-1k.bin-plain-w4-iocp | -19.52% | 5/6 | 1.27% |
| backend | proxy-fast-1m.bin-tls-w4-iocp | proxy-fast-1m.bin-tls-w4-select | -40.75% | 5/6 | 2.13% |
| backend | proxy-fast-1m.bin-tls-w4-iocp | proxy-fast-1m.bin-tls-w4-poll | -41.35% | 5/6 | 0.81% |
| workers | proxy-fast-1m.bin-tls-w4-iocp | proxy-fast-1m.bin-tls-w1-iocp | +27.63% | 6/6 | 1.05% |
| tls | proxy-fast-1m.bin-tls-w4-iocp | proxy-fast-1m.bin-plain-w4-iocp | -60.57% | 5/6 | 1.04% |
| backend | proxy-slow-1k.bin-tls-w4-iocp | proxy-slow-1k.bin-tls-w4-select | -0.01% | 6/6 | 0.11% |
| backend | proxy-slow-1k.bin-tls-w4-iocp | proxy-slow-1k.bin-tls-w4-poll | -0.13% | 6/6 | 0.12% |
| workers | proxy-slow-1k.bin-tls-w4-iocp | proxy-slow-1k.bin-tls-w1-iocp | +1.36% | 6/6 | 0.21% |
| tls | proxy-slow-1k.bin-tls-w4-iocp | proxy-slow-1k.bin-plain-w4-iocp | -3.00% | 5/6 | 0.05% |
| upstream delay | proxy-slow-1k.bin-tls-w4-iocp | proxy-fast-1k.bin-tls-w4-iocp | -95.23% | 6/6 | 0.93% |
| backend | proxy-slow-1m.bin-tls-w4-iocp | proxy-slow-1m.bin-tls-w4-select | -4.36% | 5/6 | 1.00% |
| backend | proxy-slow-1m.bin-tls-w4-iocp | proxy-slow-1m.bin-tls-w4-poll | -4.27% | 6/6 | 2.82% |
| workers | proxy-slow-1m.bin-tls-w4-iocp | proxy-slow-1m.bin-tls-w1-iocp | +105.35% | 5/6 | 3.03% |
| tls | proxy-slow-1m.bin-tls-w4-iocp | proxy-slow-1m.bin-plain-w4-iocp | -38.00% | 5/6 | 1.06% |
| upstream delay | proxy-slow-1m.bin-tls-w4-iocp | proxy-fast-1m.bin-tls-w4-iocp | +43.12% | 5/6 | 0.74% |
| upstream delay | proxy-slow-1k.bin-plain-w1-select | proxy-fast-1k.bin-plain-w1-select | -94.43% | 5/6 | 2.25% |
| upstream delay | proxy-slow-1m.bin-plain-w1-select | proxy-fast-1m.bin-plain-w1-select | -21.76% | 6/6 | 1.23% |
| tls | proxy-fast-1k.bin-tls-w1-select | proxy-fast-1k.bin-plain-w1-select | -26.38% | 4/6 | 0.60% |
| tls | proxy-fast-1m.bin-tls-w1-select | proxy-fast-1m.bin-plain-w1-select | -39.54% | 6/6 | 1.37% |
| tls | proxy-slow-1k.bin-tls-w1-select | proxy-slow-1k.bin-plain-w1-select | -3.19% | 6/6 | 0.41% |
| upstream delay | proxy-slow-1k.bin-tls-w1-select | proxy-fast-1k.bin-tls-w1-select | -92.49% | 6/6 | 3.26% |
| tls | proxy-slow-1m.bin-tls-w1-select | proxy-slow-1m.bin-plain-w1-select | -33.31% | 6/6 | 3.11% |
| upstream delay | proxy-slow-1m.bin-tls-w1-select | proxy-fast-1m.bin-tls-w1-select | -13.83% | 6/6 | 1.72% |
| workers | proxy-fast-1k.bin-plain-w4-select | proxy-fast-1k.bin-plain-w1-select | +49.75% | 5/6 | 2.80% |
| workers | proxy-fast-1m.bin-plain-w4-select | proxy-fast-1m.bin-plain-w1-select | +106.14% | 6/6 | 2.37% |
| workers | proxy-slow-1k.bin-plain-w4-select | proxy-slow-1k.bin-plain-w1-select | +1.11% | 6/6 | 0.14% |
| upstream delay | proxy-slow-1k.bin-plain-w4-select | proxy-fast-1k.bin-plain-w4-select | -96.24% | 6/6 | 0.83% |
| workers | proxy-slow-1m.bin-plain-w4-select | proxy-slow-1m.bin-plain-w1-select | +122.63% | 6/6 | 0.50% |
| upstream delay | proxy-slow-1m.bin-plain-w4-select | proxy-fast-1m.bin-plain-w4-select | -15.28% | 6/6 | 4.52% |
| workers | proxy-fast-1k.bin-tls-w4-select | proxy-fast-1k.bin-tls-w1-select | +69.62% | 6/6 | 2.60% |
| tls | proxy-fast-1k.bin-tls-w4-select | proxy-fast-1k.bin-plain-w4-select | -19.19% | 6/6 | 0.87% |
| workers | proxy-fast-1m.bin-tls-w4-select | proxy-fast-1m.bin-tls-w1-select | +105.43% | 6/6 | 3.78% |
| tls | proxy-fast-1m.bin-tls-w4-select | proxy-fast-1m.bin-plain-w4-select | -39.51% | 6/6 | 4.92% |
| workers | proxy-slow-1k.bin-tls-w4-select | proxy-slow-1k.bin-tls-w1-select | +0.97% | 6/6 | 0.85% |
| tls | proxy-slow-1k.bin-tls-w4-select | proxy-slow-1k.bin-plain-w4-select | -3.09% | 6/6 | 0.11% |
| upstream delay | proxy-slow-1k.bin-tls-w4-select | proxy-fast-1k.bin-tls-w4-select | -95.46% | 6/6 | 1.28% |
| workers | proxy-slow-1m.bin-tls-w4-select | proxy-slow-1m.bin-tls-w1-select | +117.06% | 6/6 | 3.23% |
| tls | proxy-slow-1m.bin-tls-w4-select | proxy-slow-1m.bin-plain-w4-select | -35.78% | 6/6 | 3.52% |
| upstream delay | proxy-slow-1m.bin-tls-w4-select | proxy-fast-1m.bin-tls-w4-select | -8.91% | 6/6 | 3.84% |
| upstream delay | proxy-slow-1k.bin-plain-w1-poll | proxy-fast-1k.bin-plain-w1-poll | -94.38% | 6/6 | 2.08% |
| upstream delay | proxy-slow-1m.bin-plain-w1-poll | proxy-fast-1m.bin-plain-w1-poll | -21.57% | 6/6 | 1.68% |
| tls | proxy-fast-1k.bin-tls-w1-poll | proxy-fast-1k.bin-plain-w1-poll | -26.05% | 6/6 | 3.76% |
| tls | proxy-fast-1m.bin-tls-w1-poll | proxy-fast-1m.bin-plain-w1-poll | -39.42% | 6/6 | 0.76% |
| tls | proxy-slow-1k.bin-tls-w1-poll | proxy-slow-1k.bin-plain-w1-poll | -2.56% | 6/6 | 0.76% |
| upstream delay | proxy-slow-1k.bin-tls-w1-poll | proxy-fast-1k.bin-tls-w1-poll | -92.50% | 5/6 | 0.85% |
| tls | proxy-slow-1m.bin-tls-w1-poll | proxy-slow-1m.bin-plain-w1-poll | -32.07% | 6/6 | 3.63% |
| upstream delay | proxy-slow-1m.bin-tls-w1-poll | proxy-fast-1m.bin-tls-w1-poll | -12.55% | 6/6 | 3.17% |
| workers | proxy-fast-1k.bin-plain-w4-poll | proxy-fast-1k.bin-plain-w1-poll | +51.31% | 6/6 | 1.73% |
| workers | proxy-fast-1m.bin-plain-w4-poll | proxy-fast-1m.bin-plain-w1-poll | +108.02% | 6/6 | 1.78% |
| workers | proxy-slow-1k.bin-plain-w4-poll | proxy-slow-1k.bin-plain-w1-poll | +0.86% | 6/6 | 1.03% |
| upstream delay | proxy-slow-1k.bin-plain-w4-poll | proxy-fast-1k.bin-plain-w4-poll | -96.24% | 6/6 | 0.69% |
| workers | proxy-slow-1m.bin-plain-w4-poll | proxy-slow-1m.bin-plain-w1-poll | +126.17% | 6/6 | 1.55% |
| upstream delay | proxy-slow-1m.bin-plain-w4-poll | proxy-fast-1m.bin-plain-w4-poll | -14.63% | 6/6 | 3.85% |
| workers | proxy-fast-1k.bin-tls-w4-poll | proxy-fast-1k.bin-tls-w1-poll | +64.40% | 6/6 | 3.27% |
| tls | proxy-fast-1k.bin-tls-w4-poll | proxy-fast-1k.bin-plain-w4-poll | -19.41% | 6/6 | 2.64% |
| workers | proxy-fast-1m.bin-tls-w4-poll | proxy-fast-1m.bin-tls-w1-poll | +108.13% | 6/6 | 1.58% |
| tls | proxy-fast-1m.bin-tls-w4-poll | proxy-fast-1m.bin-plain-w4-poll | -39.05% | 6/6 | 3.14% |
| workers | proxy-slow-1k.bin-tls-w4-poll | proxy-slow-1k.bin-tls-w1-poll | +0.41% | 6/6 | 0.98% |
| tls | proxy-slow-1k.bin-tls-w4-poll | proxy-slow-1k.bin-plain-w4-poll | -2.87% | 6/6 | 0.29% |
| upstream delay | proxy-slow-1k.bin-tls-w4-poll | proxy-fast-1k.bin-tls-w4-poll | -95.44% | 5/6 | 0.66% |
| workers | proxy-slow-1m.bin-tls-w4-poll | proxy-slow-1m.bin-tls-w1-poll | +110.79% | 6/6 | 4.89% |
| tls | proxy-slow-1m.bin-tls-w4-poll | proxy-slow-1m.bin-plain-w4-poll | -36.66% | 6/6 | 1.62% |
| upstream delay | proxy-slow-1m.bin-tls-w4-poll | proxy-fast-1m.bin-tls-w4-poll | -12.69% | 6/6 | 2.78% |

## Inconclusive estimates

These entries retain their counts and diagnostics without publishing a rate
or percentage change. MAD is scaled dispersion of retained log values,
converted to percent. Order difference compares the two execution-order
medians. `n/a` means fewer than two retained pairs in one order.

### Absolute rates

| Case | Retained | MAD | Reason |
| --- | ---: | ---: | --- |
| `static-off-1m.bin-plain-w4-iocp` | 5/6 | 6.09% | retained dispersion exceeds 5% |
| `static-off-1m.bin-tls-w4-select` | 6/6 | 5.25% | retained dispersion exceeds 5% |
| `static-off-1m.bin-tls-w4-poll` | 6/6 | 8.75% | retained dispersion exceeds 5% |
| `proxy-fast-1m.bin-tls-w4-select` | 6/6 | 5.43% | retained dispersion exceeds 5% |

### Paired comparisons

| Dimension | Candidate | Control | Pairs | MAD | Order difference | Reason |
| --- | --- | --- | ---: | ---: | ---: | --- |
| sendfile | `static-on-1m.bin-plain-w4-iocp` | `static-off-1m.bin-plain-w4-iocp` | 5/6 | 5.16% | 3.23% | retained dispersion exceeds 5% |
| backend | `static-off-1m.bin-tls-w4-iocp` | `static-off-1m.bin-tls-w4-select` | 6/6 | 22.82% | 59.76% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| backend | `static-off-1m.bin-tls-w4-iocp` | `static-off-1m.bin-tls-w4-poll` | 6/6 | 28.41% | 44.53% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| tls | `static-off-1m.bin-tls-w4-iocp` | `static-off-1m.bin-plain-w4-iocp` | 6/6 | 21.93% | 2.76% | retained dispersion exceeds 5% |
| backend | `static-on-1m.bin-tls-w4-iocp` | `static-on-1m.bin-tls-w4-select` | 6/6 | 19.50% | 6.24% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| workers | `static-on-1m.bin-tls-w4-iocp` | `static-on-1m.bin-tls-w1-iocp` | 4/6 | 1.09% | n/a | fewer than two retained pairs in each execution order |
| tls | `static-on-1m.bin-tls-w4-iocp` | `static-on-1m.bin-plain-w4-iocp` | 4/6 | 1.20% | n/a | fewer than two retained pairs in each execution order |
| workers | `static-off-1m.bin-tls-w4-select` | `static-off-1m.bin-tls-w1-select` | 6/6 | 6.75% | 12.06% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| tls | `static-off-1m.bin-tls-w4-select` | `static-off-1m.bin-plain-w4-select` | 6/6 | 6.54% | 7.13% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| sendfile | `static-on-1m.bin-tls-w4-select` | `static-off-1m.bin-tls-w4-select` | 5/6 | 8.63% | 5.77% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| workers | `static-off-1m.bin-tls-w4-poll` | `static-off-1m.bin-tls-w1-poll` | 6/6 | 8.05% | 12.70% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| tls | `static-off-1m.bin-tls-w4-poll` | `static-off-1m.bin-plain-w4-poll` | 6/6 | 7.98% | 10.74% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| tls | `static-on-1m.bin-tls-w4-poll` | `static-on-1m.bin-plain-w4-poll` | 6/6 | 5.62% | 7.66% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| sendfile | `static-on-1m.bin-tls-w4-poll` | `static-off-1m.bin-tls-w4-poll` | 6/6 | 6.35% | 4.40% | retained dispersion exceeds 5% |
| backend | `proxy-fast-1k.bin-plain-w1-iocp` | `proxy-fast-1k.bin-plain-w1-poll` | 6/6 | 4.74% | 7.27% | execution-order difference exceeds 5% |
| upstream delay | `proxy-slow-1m.bin-plain-w1-iocp` | `proxy-fast-1m.bin-plain-w1-iocp` | 4/6 | 0.26% | n/a | fewer than two retained pairs in each execution order |
| backend | `proxy-fast-1m.bin-plain-w4-iocp` | `proxy-fast-1m.bin-plain-w4-poll` | 6/6 | 7.13% | 6.42% | retained dispersion exceeds 5%; execution-order difference exceeds 5% |
| workers | `proxy-fast-1m.bin-plain-w4-iocp` | `proxy-fast-1m.bin-plain-w1-iocp` | 6/6 | 5.26% | 2.06% | retained dispersion exceeds 5% |
| upstream delay | `proxy-slow-1m.bin-plain-w4-iocp` | `proxy-fast-1m.bin-plain-w4-iocp` | 6/6 | 4.20% | 5.71% | execution-order difference exceeds 5% |

## Artifacts and reproduction

The measured nginx executable has SHA256
`ccb427b5f911107e0281177f7a71310d80d5cc093c3bc84675048809f36fa810`
in both manifests. The downloaded result archives passed ZIP integrity checks,
and their SHA256 values match GitHub's recorded artifact digests:

| Artifact | ID | ZIP SHA256 |
| --- | --- | --- |
| benchmark-results-static | 10751210620 | `26cb153ee7a58e74be1905566d119a98e27fbf9c91b0535501ad67618dd05b12` |
| benchmark-results-proxy | 10751235814 | `d45eaaedc824ad3fa54e99a72a68fface16f8cdeee5980c6488e5b0de3d1fbf9` |

Each 90-day result artifact includes `manifest.json`, `samples.jsonl`, raw
measurements and warm-ups, probes, configurations, logs, and complete
`summary.json`/`summary.md` reports. To recompute either downloaded report:

```sh
node misc/benchmark-report.js path/to/extracted/artifact
```

Use the reporting script at the measured source commit to reproduce the exact
snapshot. Its recomputed JSON and rate tables matched the published artifacts.
This dated report preserves all reported rates, comparisons, and inconclusive
reasons after artifact retention expires.
The [workflow guide](windows-ci-benchmarks.md#running-and-reading-the-workflow)
describes how to run a new campaign. The
[September 7 snapshot](win32-iocp-performance.md#historical-hosted-ci-snapshot-september-7-2026)
used a different workload, build, and sampling method and is retained as history.

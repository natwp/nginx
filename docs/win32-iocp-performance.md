# Windows IOCP Performance Validation

This document records performance experiments used to decide whether an IOCP
optimization remains in the tree.  It is intentionally limited to completed,
repeatable measurements; unfinished experiments are marked pending.

## Method

Measurements were collected on August 17–27, 2026, on a shared Windows
workstation with the benchmark driver running from WSL.  Candidate and control
binaries were built from the same source revision with only the tested switch
changed.  The historical isolated experiments used three two-second workload
samples; the long Windows Node validation below used three five-second samples.
Each paired run alternated candidate/control order.  CPU and available-memory
gates delayed a run while either Windows or WSL was busy.

Windows CPU and memory telemetry came from cmd.exe/wmic; free ports and
process startup used MSYS2/POSIX helpers.

The scripts first take the median of each run.  The paired comparison then
rejects a pair when either its request-rate or bandwidth ratio lies farther
than `max(2%, 2 * MAD)` from the median ratio.  At least three pairs must
remain.  The median ratio of retained, same-round pairs is authoritative;
positive deltas favor the candidate.  All measurements below completed with
zero request errors.

The repeatable drivers and filters are:

- `misc/win32-iocp-repeat.sh` and `misc/win32-iocp-bench.sh`
- `misc/http-loopback-bench.js` for keepalive traffic and
  `misc/http-connect-bench.js` for TCP connection churn
- `misc/win32-udp-repeat.sh` and `misc/win32-udp-bench.sh`
- `misc/noise-compare.js` and `misc/noise-filter.js`

The long validation runs additionally used
`node.exe` (Node.js v22.16.0).  The WSL
wrapper converts script paths and writes each Windows Node result to a native
file before the WSL filter reads it; this avoids the `EISDIR` stdout-handle
behavior of Windows Node when a WSL pipe is inherited.

## September 1-2, 2026 correctness and router work

Three changes landed after the August campaign.  All were built with MinGW
`-O2 -Werror`, `FD_SETSIZE=1024`, and `NGX_IOCP_DIRECT_SEND_MIN_SIZE=4096`,
and all benchmarks below ran from a ramdisk under the existing CPU gate.

`8877cd2d0` stops `ngx_iocp_process_events()` from abandoning a completion
batch.  `GetQueuedCompletionStatusEx()` has already removed every entry from
the port, so returning `NGX_ERROR` on one unusable entry silently discarded
up to 63 dequeued completions, each keeping its owner and `ngx_iocp_pending`
references until shutdown; nothing observed the failure because
`ngx_process_events_and_timers()` discards the return value.  Entries are now
reported and skipped individually.  Validation: a two-worker lifecycle served
129754 requests at 12978 req/s over 128 connections with reload under load and
graceful quit, at a 100% success rate and with no alert, error,
stale-operation, or shutdown drain message.

`f1f4bcef4` makes routed UDP flow placement independent of backpressure.
`ngx_win32_router_select_worker()` reduced the flow hash modulo the count of
*eligible* workers, so one worker reaching its queue limit changed the divisor
and remapped unrelated flows.  The hash is now reduced modulo the whole ready
set, probing forward past backlogged workers.  An exhaustive check over 100000
hashes with four ready workers showed the two forms agree on every hash while
all workers are eligible, and that backlogging one worker remapped 50% of the
unrelated flows before the change and none after it.

`7ef90485d` recycles routed UDP receive operations.  Each datagram previously
paid an `ngx_calloc()`, the zeroing, and the free of a ~66 KB operation, since
the structure embeds a 65535-byte datagram buffer and a 512-byte control
buffer.  The completion handler now consumes the datagram before posting the
replacement receive and hands the same operation back, resetting only the
overlapped structure, message header, peer address, flags, and recvmsg
selector.  Measured at **+13.75%** request rate, 5/6 pairs retained, zero
errors; reversing the candidate and control roles reproduced the win in the
opposite direction at -6.09%.

### Ordering bias in the paired UDP harness

`misc/win32-udp-repeat.sh` alternates run order by repeat parity, so an odd
`REPEATS` value leaves the candidate running second more often than first.
That bias is worth roughly 3-4% on this workstation: the routing change above
first measured -3.821% and -3.073% with the default `REPEATS=5`, even though
an exhaustive check proves it cannot change placement in that workload.  With
roles swapped and `REPEATS=6`, the same comparison returned +0.791%.  Use an
even `REPEATS`, and confirm any accepted result by swapping the candidate and
control roles.  Single-digit deltas recorded elsewhere in this document that
used an odd repeat count may carry the same artifact.

### TCP regression check for the September changes

The three changes above touch the IOCP dispatch loop and the routed UDP
paths, so the TCP workloads were re-measured against the August control
`.build-iocp-try-send-fd1024-prefix-final-20260827/nginx.exe`.  All runs used
`misc/win32-oha-compare.sh` with `REPEATS=6`, four workers, the native
`oha-windows-amd64-pgo.exe`, and the CPU gate.

| Workload | Connections | Pairs retained | Request-rate delta | Errors |
| --- | ---: | ---: | ---: | ---: |
| Static 64 KiB | 64 | 4/6 | -2.153% | 0 |
| Static 64 KiB, roles swapped | 64 | 5/6 | +1.186% | 0 |
| Fast 64 KiB proxy, buffering off | 64 | 4/6 | +2.412% | 0 |
| Slow 64 KiB proxy, 25 ms delay | 128 | 5/6 | +4.293% | 0 |

No workload regressed.  The static row is the only negative number and it
does not survive a role swap: reversing candidate and control moved it to
+1.186%, so both directions sit inside +-2.2% with opposite signs, which is
noise rather than a deficit.  The per-round static rates make the same point
directly: excluding one rejected 21% outlier, the remaining rounds scatter
-0.5%, -3.8%, +6.6%, -0.5%, and -6.6% around zero.  Aggregate nginx CPU was
also slightly *lower* for the candidate on the static workload (3.523 versus
3.573 occupied cores).  HTTP/2 was not measured because these builds are
configured without `ngx_http_v2_module`.

### Connection churn is not measurable on this workstation

An initial attempt to use `--disable-keepalive` churn for the regression
check produced 13-96% success rates that looked like a candidate regression.
It was not one: the control degraded identically when re-run, because churn
drives TIME_WAIT past the 16384-entry Windows ephemeral port range
(`netsh int ipv4 show dynamicport tcp` reports `Start 49152, Number 16384`),
and the pool needs roughly 150 seconds to drain.  Any churn measurement taken
before that drain characterizes port exhaustion in the client rather than the
server.  Use keepalive workloads here, or drain the pool between runs.

### Rejected September optimization candidates

Four further candidates were built and measured; none is in the tree.

The router pipe **write batching** candidate coalesced several queued
channel messages into one `WriteFile()`.  The channel is a byte-mode pipe
carrying length-prefixed messages and the worker reads to an exact byte
count, so this needed no reader change.  It was rejected on engagement
rather than on throughput: an instrumented build counted 1467 batched
writes out of 412239 total (0.36%, averaging 2.6 messages per batch) at 64
byte datagrams, falling to 0.06% at 1200 bytes.  The pipe drains faster
than datagrams arrive, so the queue almost never holds a second message and
the extra copy plus multi-message retirement logic buys nothing.

A **128-entry worker completion batch** (`NGX_IOCP_BATCH=128`) was retried
because the original rollback was driven by a connection-churn reversal
that this workstation can no longer measure.  Six gated keepalive pairs
returned -0.851% with 4/6 retained and CPU neutral at -0.007 cores, so the
64-entry batch stands on its own merits, not only on the churn result.

**Flow-key narrowing** hashed and compared only the populated key prefix,
56 of 76 bytes for a tuple flow, since `cid[]` is zero for tuple flows and
the remote address is zero for QUIC flows.  Six gated pairs with the native
Windows client returned -0.130% with 5/6 retained: correct, but neutral,
and not worth widening `ngx_win32_router_flow_key()`,
`ngx_win32_router_find_flow()`, and `ngx_win32_router_set_flow()` by a
length argument.

**Accept-op recycling** in `ngx_event_acceptex_complete()` was left
unmeasured.  It is the same alloc-and-free-per-event shape as the routed
UDP receive, but the accept operation is small rather than ~66 KB, and it
is only observable under the connection churn described above.

### Where the routed UDP cost actually sits

Per-process CPU accounting over an 8.5 second native-client UDP run put the
router thread at 6.97 CPU seconds against 5.14 seconds for all four workers
combined, so the router is **57.5% of nginx CPU** and about 0.82 of one
core at roughly 43000 datagrams/s.  It is the bottleneck for routed UDP,
and it is single-threaded by design.

A gprof profile of that router, with instrumentation overhead excluded,
attributes 64.3% of real work to the dispatch loop body itself,
14.3% to `ngx_win32_router_find_flow()`, and 7.1% to `memcpy`.  Normalized
per datagram the loop performs about 3.1 `ngx_alloc()` calls, 2.1
`ngx_inet_get_port()` calls, and 1.0 `ngx_cmp_sockaddr()` call, the latter
two from the `ngx_win32_router_find_udp_listener()` verification on the
send path.  `find_flow` also refreshes an LRU deadline and moves two queue
nodes on every lookup.  Any further routed-UDP work should target the
dispatch loop and the flow lookup, not the copies: the copy-removal
experiments recorded above were all measured and rejected.

## August 27, 2026 current IOCP source

The current IOCP implementation is `d000c73ba`.  It includes the
read-readiness fix `0182a46ca`, the libuv-style try-write change
`c1b5f1c78`, and the file-prefix safety changes `83f2fe37f` and
`d000c73ba`.  The final native executable is
`.build-iocp-try-send-fd1024-prefix-final-20260827/nginx.exe`, SHA256
`229bcb1712b4526e4e3235f22a2cff6330a7dc6068a7494ab3c50850664761a0`.
The final wepoll-reference executable is
`.build-wepoll-try-send-fd1024-filefix-20260827/nginx.exe`, SHA256
`aa6edc52973178595eb15e4895cebc90e90114b4b8bc507913bf0e34baf89cb7`.
Both were built with MinGW `-O2 -Werror`, `FD_SETSIZE=1024`, and
`NGX_IOCP_DIRECT_SEND_MIN_SIZE=4096`.

This is a hybrid IOCP path rather than a claim that every operation is a
pure proactor.  A zero-byte `WSARecv()` notification tells the worker that a
socket is readable; the worker drains immediately available bytes
synchronously and records unknown notification byte counts as
`available=-1`.  For writes, `ngx_overlapped_wsasend_chain()` first tries a
nonblocking scatter/gather `WSASend()` over the leading memory-only prefix,
in the same style as libuv's TCP try-write path.  It stops at the first file
buffer so the remainder continues through `TransmitPackets()` or
`TransmitFile()`.  A partial memory write or `WSAEWOULDBLOCK` falls through
to a real overlapped `WSASend()`/IOCP completion, with the request pool
retaining the buffers until completion.  This keeps the common writable
loopback path out of operation allocation without removing the asynchronous
backpressure or file-transmit paths.

MinGW gprof measured the try-write effect at `c1b5f1c78`.  The read-fix
control recorded 9,018
requests, 209,075 `ngx_iocp_op_create()` calls, 187,057 send-side operation
creations, and 374,111 send-chain calls.  The try-write candidate recorded
21,041 requests, 50,542 total operation creations, 719 send-side operation
creations, and 438,426 send-chain calls.  Normalized by request, send-side
operation creation fell from 20.7426 to 0.0342 (99.835% fewer), while total
operation creation fell from 23.18 to 2.40 (about 89.6% fewer).  The 719
remaining send operations show that the overlapped fallback was exercised;
these are call counts, not precise time percentages.  Reports are retained
in `.codex-artifacts/iocp-gprof-read-available-worker-20260827.txt` and
`.codex-artifacts/iocp-gprof-try-send-worker-20260827.txt`.

The final file-prefix guard was then compared with the original try-write
binary using six alternating, CPU-gated 64 KiB `sendfile off` pairs.  The
filter retained four pairs and measured `-0.224%` request rate, `-0.224%`
bandwidth, and `+0.0007` nginx CPU core for the final guard, with zero errors.
That is neutral inside the documented 2% practical noise floor.  An exact-
final `sendfile on` smoke served 8,763 64 KiB requests at 8,714.5 requests/s
and 544.7 MiB/s with zero errors.  Compact final-source evidence is retained
under `.codex-artifacts/iocp-final-source-20260827/`.

The following fixed-load matrices used native
`oha-windows-amd64-pgo.exe`, a CPU gate before every measured sample, a
warm-up of at most 0.5 seconds, and the noise filter described above.  One-
and four-worker rows use the same total client load and are compared only
within their worker count.  CPU is aggregate nginx utilization in occupied
logical cores.

### Static 64 KiB response

The static matrix used 128 keepalive connections, `sendfile off`, four raw
rounds, a 0.3-second warm-up, and a 1.5-second sample.

| Backend | Workers | Requests/s | MiB/s | nginx CPU cores |
| --- | ---: | ---: | ---: | ---: |
| IOCP | 1 | 9,112.7 | 569.5 | 1.0011 |
| select | 1 | 9,360.6 | 585.0 | 0.9945 |
| poll | 1 | 8,740.5 | 546.3 | 1.0027 |
| IOCP | 4 | 19,195.6 | 1,199.7 | 3.4227 |
| select | 4 | 14,904.7 | 931.5 | 2.6304 |
| poll | 4 | 16,587.4 | 1,036.7 | 2.5355 |

The retained same-round pair deltas were IOCP one worker versus select
`-1.805%` and poll `-1.626%`; four-worker IOCP versus select was `+26.937%`
and versus poll `+22.051%`.  Four-worker IOCP scaled `+114.337%` over its
one-worker result.  Every retained row had zero errors.

### Fast 64 KiB proxy response

The fast proxy matrix used the in-memory upstream with zero delay, 128
keepalive connections, proxy buffering off, six raw rounds, a 0.3-second
warm-up, and a 1.5-second sample.

| Backend | Workers | Requests/s | MiB/s | nginx CPU cores |
| --- | ---: | ---: | ---: | ---: |
| IOCP | 1 | 6,290.6 | 393.2 | 0.9798 |
| select | 1 | 6,907.0 | 431.7 | 0.9467 |
| poll | 1 | 7,812.5 | 488.3 | 0.9822 |
| IOCP | 4 | 11,644.2 | 727.8 | 1.4559 |
| select | 4 | 9,958.6 | 622.4 | 1.3949 |
| poll | 4 | 11,498.3 | 718.6 | 1.6240 |

The retained pair deltas were IOCP one worker versus select `+6.330%` and
poll `-11.240%`; four-worker IOCP versus select was `+6.560%` and versus poll
`-0.438%`.  Four-worker IOCP scaled `+80.372%` over one worker.  The
one-worker IOCP condition had two noisy raw rounds rejected; the paired
result above uses five of six same-round pairs and is the authoritative
comparison under the documented filter.  All retained rows had zero errors.

### Slow high-connection proxy response

The slow proxy matrix used a 64 KiB in-memory upstream body delayed by 25 ms,
proxy buffering off, 448 keepalive connections, four raw rounds, a 0.5-second
warm-up, and a 2-second sample.

| Backend | Workers | Requests/s | MiB/s | nginx CPU cores |
| --- | ---: | ---: | ---: | ---: |
| IOCP | 1 | 7,428.5 | 464.3 | 0.9964 |
| select | 1 | 7,260.7 | 453.8 | 0.9859 |
| poll | 1 | 6,738.2 | 421.1 | 0.9746 |
| IOCP | 4 | 8,850.2 | 553.1 | 1.3927 |
| select | 4 | 8,244.0 | 515.2 | 1.6503 |
| poll | 4 | 8,731.5 | 545.7 | 1.7023 |

The retained pair deltas were IOCP one worker versus select `-1.796%` and
poll `+4.881%`; four-worker IOCP versus select was `+10.999%` and versus poll
`-1.065%`.  Four-worker IOCP scaled `+21.985%` over one worker while using
about 0.20 fewer nginx cores than select and 0.34 fewer than poll in the
retained comparisons.  All retained rows had zero errors, and upstream stats
confirmed that a delayed in-memory server rather than disk I/O was the proxy
source.

The compact filtered records are retained under
`.codex-artifacts/iocp-try-send-static-matrix-20260827/`,
`.codex-artifacts/iocp-try-send-fast-matrix-20260827/`, and
`.codex-artifacts/iocp-try-send-fd1024-slow-matrix-20260827/`.

### More-than-1,024 connection capacity

A separate exact-final one-worker IOCP smoke used 2,048 keepalive
connections, four native client processes, a 0.5-second warm-up, and a
1.5-second sample.  The
connection audit observed 2,048 established client sockets and 2,049 total
server-side TCP rows, with zero request errors, 65,048.5 requests/s, and
0.8876 occupied nginx cores.  Therefore `NGX_IOCP_MAX_ACCEPTS=1024` limits
the number of preposted `AcceptEx` operations, not the number of live
connections.  The compact record is under
`.codex-artifacts/iocp-final-source-20260827/`.

The repeatable current harness is committed as `902d45199`.  It records
request rate, MiB/s, nginx CPU by role, optional TCP connection audits,
in-memory upstream concurrency statistics, and unpaired rounds rather than
silently treating missing samples as paired.  Successful runs remove only
their validated scratch directory from WSL.

### Wepoll readiness reference

For an external readiness reference, the try-write source was also rebuilt
with the nginx module in `../wepoll-ex/nginx`, using the same `-O2`,
`FD_SETSIZE=1024`, and direct-send threshold.  A compact static 64 KiB pass
used 128 total connections, one- and four-worker IOCP and level-triggered
wepoll conditions, five alternating rounds, a 0.2-second warm-up, and a
one-second measured sample.  All raw runs completed with zero errors.

The noise-aware paired result put level-triggered wepoll 4.675% behind native
IOCP at one worker and 23.660% behind at four workers.  Native IOCP scaled
99.262% from one to four workers; wepoll scaled 94.789%.  This is a reference
comparison between different contracts: wepoll exposes readiness through an
epoll-compatible AFD adapter, while native nginx IOCP uses zero-byte receive
notification, synchronous drain/try-write fast paths, and overlapped fallback
under backpressure.  Filtered records are under
`.codex-artifacts/iocp-vs-wepoll-current-static-20260827/`.

The final file-prefix refactor does not change wepoll's readiness contract:
wepoll still calls the ordinary memory send-chain path, while only native
IOCP requests the stop-at-file try-write behavior.  The final-source wepoll
binary was rebuilt successfully with the hash recorded above.

### Current performance conclusion

The optimization is a large architectural improvement over the previous
native IOCP implementation, but it does not make IOCP universally faster.
At one worker, the retained matrices range from a 6.330% win over select in
the fast proxy case to an 11.240% loss versus poll in that same case; static
64 KiB is about 1.8% behind select and inside 2% of poll.  At four workers,
IOCP is 6.560% to 26.937% ahead of select across the three main workloads,
but ranges from 22.051% ahead of poll for static data to about 1% behind poll
for fast and slow proxy traffic.  The evidence therefore supports keeping
the IOCP architecture and its multi-worker path, not a claim that it wins
every single-worker or readiness comparison.

## Completed isolated experiments

| Experiment | Workload | Pairs retained/raw | Request-rate delta | Latency p50, candidate/control | Latency p95, candidate/control | Decision |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Synchronous TCP receive ready path | 8 KiB padded request | 5/7 | +0.183% | 0.840/0.832 ms | 1.661/1.651 ms | Rolled back: throughput gain was within the noise floor and latency regressed. |
| Synchronous TCP send ready path | Tiny response | 9/12 | +0.226% | 0.793/0.798 ms | 1.356/1.363 ms | Rolled back: no material throughput gain. |
| UDP receive operation cache | 64-byte datagram | 8/12 | +3.760% | 0.979/1.011 ms | 1.832/1.925 ms | Rolled back after the 1200-byte reversal. |
| UDP receive operation cache | 1200-byte datagram | 7/7 | -3.823% | 1.028/1.028 ms | 2.025/1.944 ms | Rolled back. |
| Persistent UDP receive slot | 64-byte datagram | 11/12 | -0.061% | 1.113/1.098 ms | 2.276/2.265 ms | Rolled back. |
| Persistent UDP receive slot | 1200-byte datagram | 6/7 | -4.002% | 1.067/1.056 ms | 2.086/2.015 ms | Rolled back. |
| UDP send operation cache | 64-byte datagram | 5/7 | -0.733% | 1.059/1.075 ms | 2.070/2.064 ms | Rolled back. |
| UDP send operation cache | 1200-byte datagram | 5/7 | -0.452% | 0.929/0.926 ms | 1.777/1.780 ms | Rolled back. |
| Direct TCP receive buffer | 8 KiB padded request | 5/7 | +1.032% | 0.855/0.850 ms | 1.657/1.656 ms | Rolled back after the 12 KiB reversal. |
| Direct TCP receive buffer | 12 KiB padded request | 7/7 | -0.748% | 1.155/1.148 ms | 1.777/1.758 ms | Rolled back. |
| Router completion status fast path | TCP connection churn | 5/7 | -1.456% | 3.290/3.280 ms | 18.505/18.459 ms | Rolled back. |
| Router completion status fast path | 64-byte UDP datagram | 4/7 | -0.646% | 1.005/0.986 ms | 1.893/1.888 ms | Rolled back. |
| Router completion status fast path | 1200-byte UDP datagram | 5/7 | -1.073% | 0.928/0.933 ms | 1.785/1.780 ms | Rolled back. |
| 128-entry router pipe-write buffer cache | 64-byte UDP datagram | 5/7 | -0.863% | 0.999/1.002 ms | 1.951/1.905 ms | Rolled back after the 1200-byte regression. |
| 128-entry router pipe-write buffer cache | 1200-byte UDP datagram | 7/7 | -6.751% | 1.043/1.006 ms | 2.124/1.931 ms | Rolled back. |
| Direct router UDP send ownership | 64-byte response datagram | 5/7 | -2.568% | 0.995/0.984 ms | 1.918/1.904 ms | Rolled back; small responses regressed. |
| Direct router UDP send ownership | 1200-byte response datagram | 5/7 | -5.162% | 1.016/0.992 ms | 2.017/1.909 ms | Rolled back; removing the second copy did not offset the longer pipe-read ownership lifetime. |
| Thresholded direct TCP send ownership | Empty response | 4/7 | +2.132% | 0.830/0.837 ms | 1.316/1.410 ms | Kept in that experiment: responses below the then-current 1024-byte threshold still used the copy path. |
| Thresholded direct TCP send ownership | 1 KiB file | 4/7 | +0.452% | 2.830/2.816 ms | 3.599/3.614 ms | Kept; throughput is effectively neutral at this size. |
| Thresholded direct TCP send ownership | 64 KiB file | 4/7 | +3.207% | 4.501/4.620 ms | 6.560/6.837 ms | Kept. |
| Thresholded direct TCP send ownership | 1 MiB file | 6/7 | -1.252% | 31.680/32.133 ms | 45.143/44.666 ms | Kept provisionally: regression is inside the 2% noise floor; continue watching large responses. |
| 128-entry worker completion batch | 256-way keepalive, empty response | 7/7 | +3.689% | 7.648/7.725 ms | 10.533/10.653 ms | Rolled back after the connection-churn reversal. |
| 128-entry worker completion batch | 256-way keepalive, 64 KiB file | 6/7 | +1.715% | 41.868/42.277 ms | 49.556/51.015 ms | Rolled back after the connection-churn reversal. |
| 128-entry worker completion batch | 128-way connection churn | 4/7 | -7.047% | 13.324/12.757 ms | 40.837/37.773 ms | Rolled back; the existing 64-entry batch remains. |
| 128-entry routed AcceptEx bookkeeping cache | 128-way connection churn | 5/7 | +11.337% | 13.189/14.157 ms | 36.289/67.924 ms | Kept at the time, but absent from the current tree; see below. |
| 128-entry routed AcceptEx bookkeeping cache | 256-way keepalive, empty response | 7/7 | +0.645% | 7.170/7.250 ms | 8.160/8.612 ms | Kept at the time, but absent from the current tree; see below. |
| 128-entry routed AcceptEx bookkeeping cache | 256-way keepalive, 64 KiB file | 6/7 | +1.906% | 37.575/38.224 ms | 42.086/44.741 ms | Kept at the time, but absent from the current tree; see below. |
| TLS write-notification handle cache | HTTPS, 16-way throttled 4 MiB response | 7/7 | +0.019% | 635.507/638.477 ms | 646.911/651.473 ms | Rolled back: throughput was neutral under backpressure. |
| TLS write-notification handle cache | HTTPS, 64-way connection churn | 7/7 | -12.705% | 98.405/94.641 ms | 167.722/136.845 ms | Rolled back: handle reuse regressed handshake/churn latency. |

These results show why allocation or copy removal is not accepted from an
instruction-count argument alone: lifetime management, cache locality, and
completion scheduling can reverse the result as payload size changes.

## August 21, 2026 reference-driven receive work

MinGW gprof was enabled in the actual worker and router threads rather than
only in the process entry thread.  A five-second fast-proxy profile recorded
about 1.29 million `ngx_iocp_op_create()` calls and the same number of
`ngx_calloc()` calls in the worker.  `ngx_iocp_post_read()` accounted for
about 1.29 million submissions, while send-chain completion accounted for
about 0.86 million operations.  These call counts identify operation
bookkeeping as a real hot path; the short sampling interval is not used as a
precise percentage attribution.

The local libuv 1.52.1 source keeps a per-handle zero-byte read request and
uses `OVERLAPPED.Internal`/`InternalHigh` rather than querying successful
completions again.  The local Asio development source submits with
`WSARecv()`/`WSASend()` and recycles operation storage through its operation
queue and handler allocator.  Each pattern was tested separately so that a
lifetime change was not confused with an allocation change.

| Experiment | Workload | Pairs retained/raw | Request-rate delta | nginx CPU delta | Decision |
| --- | --- | ---: | ---: | ---: | --- |
| Direct scalar receive into a request-pool buffer | Fast proxy, 64 KiB | 4/5 | +2.650% | not used as a gate | Kept. |
| Direct scalar receive into a request-pool buffer | Fast proxy, 1 KiB | 4/5 | -0.910% | not used as a gate | Kept: inside the 2% noise bound. |
| Direct chain receive into request-pool buffers | Fast proxy, 64 KiB | 8/9 | +2.458% | +0.075 percentage points | Kept. |
| Direct chain receive into request-pool buffers | Fast proxy, 1 KiB | 4/5 | +0.901% | -0.110 percentage points | Kept. |
| Skip `WSAGetOverlappedResult()` when `OVERLAPPED.Internal` reports success | Static 64 KiB | 4/4 | +0.701% | +0.017 percentage points | Rolled back after small and proxy reversals. |
| Skip successful-completion query | Fast proxy, 64 KiB | 3/4 | -0.940% | -0.007 percentage points | Rolled back. |
| Skip successful-completion query | Empty response | 3/4 | -1.931% | -0.042 percentage points | Rolled back. |
| Persistent per-owner zero-byte read notification | Static 64 KiB | 3/4 | -0.670% | +0.018 percentage points | Rolled back after the small-response reversal. |
| Persistent per-owner zero-byte read notification | Empty response | 3/4 | -4.900% | -0.148 percentage points | Rolled back. |
| Bounded retired read-notify object recycler | Empty response | 3/4 | -1.409% | -0.075 percentage points | Rolled back; no throughput gain. |
| Bounded retired read-notify object recycler | Static 64 KiB | 3/4 | -1.559% | -0.030 percentage points | Rolled back; no throughput gain. |

The persistent notification removed allocation but also changed completion
retirement and callback/rearm ordering.  Its lower nginx CPU did not translate
into higher throughput.  The implementation is absent from the tree.  Any
follow-up allocation experiment must preserve the original operation and
callback lifetime and must pass both small-response and 64 KiB gates before a
proxy comparison.

## August 24–25 final profiling and backend matrix

The final accepted source revision is `2b41e6972`.  Its owned-send change was
kept after the original paired gate measured +6.451% for a static 64 KiB
response and +2.808% for the fast 64 KiB proxy path, with zero request errors.
A later accepted-build profile recorded 265,537 send-chain calls, 371,752
scalar send calls, 159,338 IOCP operation creations, 106,296 completion
dispatches, and 53,123 zero-byte read posts.  These MinGW gprof call counts,
rather than static inspection, were used to select the next experiment.
The retained report files are under
`.codex-artifacts/win32-iocp-20260820-final/` (including the static IOCP,
fast-proxy, and select-worker reports); the separate completion-suppression
profile is retained with its routed experiment artifacts.

The release-candidate gprof pass was repeated on August 25 with the accepted
gprof binary in foreground mode so the CRT emitted profiles reliably.  It
used native oha, a one-second warm-up, a three-second 64 KiB sample, 24
connections, and the CPU gate; it completed with zero errors at 7,680.863
requests/s and 0.993 occupied nginx worker cores.  The worker report counted
60,649 `ngx_iocp_op_create()` calls, 30,329 `ngx_iocp_post_read()` calls,
30,401 completion dispatches, 60,640 send-chain calls, and 30,378 overlapped
receives.  The saved report and JSON files are in
`.codex-artifacts/win32-iocp-20260825-final/`.

The final fair static matrix used the same optimized executable and changed
only the event backend and worker count.  It used the native Windows
`oha-windows-amd64-pgo.exe` client, a 0.5-second warm-up, a two-second sample,
four rounds, and the CPU gate.  The fixed-total mode used 24 connections and
one client process; the scaled mode used 12 connections per worker and one
client process per worker (four clients for the four-worker rows).  Rates are
per-condition medians after the noise filter; CPU is the aggregate nginx
worker/router utilization expressed as occupied logical cores.  The raw and
filtered JSONL is retained in the benchmark results directory.

| Load model | Backend | Workers | Requests/s | MiB/s | nginx CPU cores |
| --- | --- | ---: | ---: | ---: | ---: |
| Fixed total | IOCP | 1 | 8,613.438 | 538.340 | 1.0047 |
| Fixed total | select | 1 | 8,443.165 | 527.698 | 0.9927 |
| Fixed total | poll | 1 | 10,112.264 | 632.017 | 1.0044 |
| Fixed total | IOCP | 4 | 20,271.195 | 1,266.950 | 3.5359 |
| Fixed total | select | 4 | 10,077.977 | 629.874 | 0.9991 |
| Fixed total | poll | 4 | 10,142.791 | 633.924 | 0.9970 |
| 12 connections/worker | IOCP | 1 | 9,014.136 | 563.383 | 0.9978 |
| 12 connections/worker | select | 1 | 10,044.925 | 627.808 | 1.0005 |
| 12 connections/worker | poll | 1 | 9,926.704 | 620.419 | 0.9991 |
| 12 connections/worker | IOCP | 4 | 18,325.835 | 1,145.365 | 3.3827 |
| 12 connections/worker | select | 4 | 17,181.216 | 1,073.826 | 1.9842 |
| 12 connections/worker | poll | 4 | 12,848.806 | 803.050 | 1.4747 |

The paired, same-round MAD comparison is the authoritative backend delta:
fixed-total one-worker IOCP was 12.499% behind select and 15.865% behind poll;
fixed-total four-worker IOCP was 108.782% ahead of select and 101.180% ahead
of poll.  Under the scaled load, four-worker IOCP was 4.739% ahead of select
and 47.026% ahead of poll, while scaling 100.528% over one-worker IOCP.  The
per-condition table and paired deltas intentionally differ slightly because
the former filters each condition independently while the latter retains only
same-round pairs.  Every retained native-oha row completed with zero request
errors.  A higher-load select sweep was rejected because Windows `FD_SETSIZE`
was exceeded; that is a backend limit, not workstation noise.

The same run also measured the small `/empty.gif` response.  Per-condition
medians were 84,868.634, 117,944.973, and 112,125.615 requests/s for fixed
one-worker IOCP, select, and poll; fixed four-worker IOCP, select, and poll
measured 145,289.655, 114,840.862, and 100,096.522 requests/s.  Under the
scaled load, one-worker IOCP/select/poll measured 77,598.982, 100,991.693,
and 97,978.609 requests/s, while four-worker IOCP/select/poll measured
151,127.963, 91,808.591, and 100,691.638 requests/s.  The paired deltas for
IOCP versus select/poll were -23.888%/-20.990% at fixed one worker,
+2.295%/+31.395% at fixed four workers, -23.163%/-19.210% at scaled one
worker, and +63.819%/+47.779% at scaled four workers.  This confirms that the
small-response path has the same real multi-worker IOCP gain, while its
single-worker result remains generator- and event-loop-sensitive.

The final lifecycle validation passed configuration testing, startup, reload,
1 KiB and 64 KiB responses with `sendfile on`, graceful quit, and a separate
close-mode connection-churn run with zero request errors.  The close-mode
64 KiB request rate was not used as performance evidence because the native
client/TIME_WAIT path was the limiting factor.

## Routed socket completion lifetime

Connection churn in an earlier experiment exposed late completion packets for
worker sockets recreated from `WSADuplicateSocket` protocol information.
Enabling `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` on those sockets allowed an
operation to be completed synchronously and recycled before the late packet
arrived; the packet could then refer to an operation owned by a different
connection.  The diagnostic run reported completion-owner mismatches and
unlinked operations, and that implementation was removed.

A second, libuv-style experiment deferred synchronous successes onto a local
ready queue so callbacks were not invoked inline.  The first profile showed
that the enable helper was never reached for routed imported sockets.  After
the import path was corrected, MinGW gprof recorded 49 calls to the enable
helper and 151,794 synchronous-completion handoffs, proving that the measured
binary exercised the intended path.

The corrected implementation was compared with an exact `2b41e6972` control.
It measured -0.092% for static 1 KiB, -0.139% for static 64 KiB, and -0.332%
for a slow 64 KiB proxy.  The slow proxy also consumed 0.0467 more nginx CPU
cores.  Four retained fast-proxy pairs measured +7.694%, but one additional
candidate attempt produced 12 connection errors and a long timeout tail.
Because the change was neutral or negative on static traffic, regressed the
clean slow-proxy gate, and had a correctness failure, it was rejected and
fully reverted.  The accepted source does not call
`SetFileCompletionNotificationModes()`.

## Completion and lifetime notes

The direct-send implementation has a 1024-byte source default.  The final
validated/package builds override `NGX_IOCP_DIRECT_SEND_MIN_SIZE` to 4096
bytes, the threshold selected by the later profiling campaign.  The request
pool is held by the IOCP operation, so buffers handed directly to `WSASend()`
remain alive until completion; smaller operations retain the explicit copy
for short-response stability.

The routed AcceptEx cache is **not present in the current tree**.  It was
measured and kept during the August campaign, then dropped by the
`19c7ac777` router reset without a recorded decision; the rows above are
retained because the measurement was real, not because the code survives.
The implementation is still recoverable from `2214973ca`: a 128-entry free
list where `ngx_win32_router_alloc_accept()` pops a cached object and
`ngx_win32_router_finish_accept()` pushes one back after closing its
socket.  Its safety rules were that it never cached or reused an accepted
socket, an outstanding OVERLAPPED operation, or an object still awaiting a
worker acknowledgement; an object entered the cache only after its socket
had been closed and its router queues had been unlinked.  Restoring it
requires re-porting onto the current router rather than reverting, and the
+11.337% churn result cannot currently be reproduced on this workstation:
connection churn exhausts the 16384-port Windows ephemeral range and drives
TIME_WAIT past 21000 sockets, so success rates collapse to 13-16% and the
measurement characterizes the client rather than the server.

The TLS wait-object experiment cached the `WSAEVENT` and thread-pool wait used
by OpenSSL `WANT_WRITE` notifications.  A diagnostic run confirmed that the
cache reduced handle creation (813 reuses out of 830 posts), but the paired
backpressure result was neutral and the 64-way HTTPS churn result was 12.705%
slower.  The cache is therefore absent from the implementation; each TLS wait
still follows the conservative create/retire/close lifetime.

The router pipe-write experiment cached completed message buffers of at most
4096 bytes.  It never reused an in-flight `WriteFile` buffer, but the extra
cache bookkeeping still reduced 1200-byte routed UDP throughput by 6.751%.
The cache is therefore absent; router pipe writes retain the allocator's
existing allocation/free path.

The direct router UDP-send experiment transferred the pipe-read allocation to
the overlapped `WSASendTo()`/`WSASendMsg()` operation and removed the second
payload copy.  It regressed 2.568% for 64-byte responses and 5.162% for
1200-byte responses, so the conservative copy-and-free lifetime remains.

## Pending experiments

No unmeasured optimization is present in the final build.  No additional safe
router data-copy candidate remains after the measured rollbacks above.  A
future completion-suppression design would need new profiling evidence and
must pass static, fast-proxy, slow-proxy, and churn gates before being kept.

## Earlier Windows packages

The August 25 release artifacts use source commit `2b41e6972`, GCC 16.1.0,
PCRE2 10.47 with JIT, zlib 1.3.2, `FD_SETSIZE=1024`, and the profiled
`NGX_IOCP_DIRECT_SEND_MIN_SIZE=4096` package override.  Both are stripped
PE32+ x86-64 executables with high-entropy ASLR, ASLR, NX/DEP, and only
Windows system DLL imports.

These ZIPs predate the August 27 read-readiness and try-write changes.  They
remain validated historical artifacts, but they are not packages of the
current `d000c73ba` IOCP source.

- Full: `nginx-1.31.4-win64-full-iocp-20260825`, statically linked with
  OpenSSL 3.6.3 and the requested HTTP SSL/2/3, mail, and stream modules.
  Executable SHA256:
  `6ec266e0f82fc3ace578f1de5498b6b0658b8aa3f93813a31ec67f36e68260fb`.
- Slim: `nginx-1.31.4-win64-slim-iocp-20260825`, without OpenSSL or TLS
  modules.  Executable SHA256:
  `ad28a3ab2c2f4390a3fe2d50fb791cb55cedd3bdfab28aa10aee5297581766ed`.

The packaged binaries, not intermediate build copies, passed configuration
testing.  Both passed CPU-gated HTTP startup, 1 KiB and 64 KiB requests,
reload, and graceful quit with zero request errors.  The full package also
passed the same HTTPS lifecycle; the slim package correctly rejected an SSL
listener with `requires ngx_http_ssl_module`.

## Historical August 18 cross-platform comparisons

These are light, five-round, CPU-gated comparisons using three one-second
samples per workload and 32 keepalive connections.  The Windows runs use
`sendfile off` for both binaries.  Positive deltas favor the modified fork.

| Candidate | Control | Workload | Pairs retained/raw | Request-rate delta | Errors |
| --- | --- | --- | ---: | ---: | ---: |
| 1.31.4 full IOCP | official 1.31.3 Windows select | empty response | 4/5 | +2.495% | 0 |
| 1.31.4 full IOCP | official 1.31.3 Windows select | 64 KiB file | 4/5 | -6.458% | 0 |
| 1.31.4 slim IOCP | official 1.31.3 Windows select | empty response | 4/5 | +6.852% | 0 |
| 1.31.4 slim IOCP | official 1.31.3 Windows select | 64 KiB file | 4/5 | -5.177% | 0 |
| 1.31.4 fork epoll | official 1.31.3 Linux epoll | empty response | 4/5 | -1.795% | 0 |
| 1.31.4 fork epoll | official 1.31.3 Linux epoll | 64 KiB file | 3/5 | +0.464% | 0 |

The official Windows archive contains a 32-bit debug build, while the two
fork packages are 64-bit optimized MinGW builds; those Windows numbers are a
directional reference, not an ABI-matched compiler comparison.  The Linux
comparison builds both versions with the same dependency-free epoll feature
set.  The fork rejects UDP listeners with the select and poll backends, so
UDP comparisons continue to use an otherwise identical IOCP control binary.

The official Windows select build is not a UDP control: this fork rejects UDP
listeners with the select and poll backends.  UDP microbenchmarks therefore
use an otherwise identical IOCP binary as their control.

## Long Windows Node and worker-count validation

On August 18, 2026, the long runs used a two-second warm-up followed by three
five-second samples, five interleaved candidate/control rounds, 32 total
keepalive connections, and the same CPU/memory gates.  `CLIENT_PROCESSES=4`
starts four native Windows Node 22 clients and aggregates their JSONL samples;
this is important for small responses because one Node event loop otherwise
hits one CPU core before nginx does.

Each sample now includes `nginxCpuPercent` and `nginxCpuCores`.  These are the
sum of nginx master/worker user and kernel time over the sample, normalized by
the eight logical processors in the workstation.  `clientCpuPercent` and
`clientCpuCores` are recorded as a diagnostic for the generator.  CPU fields
are not used to reject throughput outliers.

The single-worker run shows that the Windows Node runtime was not hiding a
server-side 64 KiB problem:

| Candidate | Control | Workload | Request-rate delta | nginx CPU candidate/control |
| --- | --- | --- | ---: | ---: |
| Full IOCP, 1 worker | Official Windows select, 1 worker | Empty response | -0.987% | 10.16% / 11.48% |
| Full IOCP, 1 worker | Official Windows select, 1 worker | 64 KiB file | -2.865% | 12.26% / 12.27% |

The client-side measurement reached about one occupied core for the empty
response, while nginx used less than one core.  Four clients remove that
generator ceiling and expose the IOCP worker/router scaling path:

| Candidate | Control | Workload | Pairs retained/raw | Request-rate delta | nginx CPU candidate/control |
| --- | --- | --- | ---: | ---: | ---: |
| Full IOCP, 4 workers / 4 clients | Official Windows select, 4 workers / 4 clients | Empty response | 5/5 | +25.695% | 24.11% / 12.19% |
| Full IOCP, 4 workers / 4 clients | Official Windows select, 4 workers / 4 clients | 64 KiB file | 4/5 | +105.147% | 42.40% / 12.33% |
| Slim IOCP, 4 workers / 4 clients | Official Windows select, 4 workers / 4 clients | Empty response | 4/5 | +22.099% | 24.15% / 12.19% |
| Slim IOCP, 4 workers / 4 clients | Official Windows select, 4 workers / 4 clients | 64 KiB file | 3/5 | +110.903% | 44.56% / 12.33% |

For connection churn, the safe long profile used one native Node client with
eight concurrent sockets.  It retained 4/5 pairs, completed with zero errors,
and measured +0.762% request rate for IOCP versus official select (inside the
noise floor).  A four-client close-mode attempt was intentionally discarded:
the client exhausted the Windows ephemeral-port/TIME_WAIT budget and both
controls reported client errors, so it is not a server performance result.

The direct IOCP worker-count comparison (four workers versus one, with one
Node client) measured +90.866% for 64 KiB and -2.582% for empty responses.
The former is a real server scaling gain; the latter remains client-limited.
No additional single-worker source change was accepted from this follow-up:
the fair multi-worker measurements already show a repeatable IOCP advantage,
while the remaining single-worker 64 KiB gap is inside a low-single-digit
throughput difference with indistinguishable nginx CPU use.

An exploratory eight-worker/four-client sweep was not used to choose a
default: its three-round empty-response filter retained only two runs after
one noisy round.  The packages therefore keep their conservative one-worker
configuration; operators can set `worker_processes auto;` or a fixed count
after measuring their own client and CPU capacity.

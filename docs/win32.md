# Windows Port Notes

This tree targets Windows 8 / Windows Server 2012 and later. Release builds
should use a 64-bit compiler and be validated on supported Windows client and
server editions. The MinGW64 workflow is documented in `AGENTS.md`.

## Process and Event Model

`worker_processes` supports a fixed count or `auto`. Each worker remains a
single-threaded nginx event loop; parallelism comes from multiple workers.
With `use iocp`, the master owns listening sockets and routes accepted TCP
connections and UDP datagrams to workers. This ownership also permits worker
replacement and overlapping generations during reload. The `select` and
`poll` backends continue to use shared listener coordination.

A typical configuration is:

```nginx
worker_processes auto;

events {
    use iocp;
    worker_connections 4096;
    iocp_threads 1;
    post_acceptex 32;
    iocp_udp_receives 16;
}
```

`iocp_threads` accepts `0` or `1`; it does not add parallel callbacks inside a
worker. Increase `worker_processes` to use more CPU cores. `post_acceptex`
controls the number of outstanding TCP accept requests per listener, not the
number of established connections. It defaults to 10 and accepts values from
1 through 1024; each completed accept is immediately replenished. Established
connections are limited by `worker_connections` per worker. The select
backend also remains limited by its compile-time `FD_SETSIZE`, while IOCP does
not use that descriptor set. `iocp_udp_receives` controls outstanding UDP
receives.

For direct same-process accepted sockets and outbound IFS stream sockets, IOCP
workers request `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`. A receive or send that
completes synchronously is consumed immediately instead of making a redundant
round trip through the completion port. Operations that return pending
continue to deliver normal IOCP completion packets. Sockets reconstructed in
a worker from `WSADuplicateSocket` protocol information, and providers without
IFS handles, retain the conservative completion-packet path.

On systems with multiple Windows processor groups, workers are assigned to
groups before their primary thread resumes. This preserves the single-threaded
nginx worker model while allowing `worker_processes auto` to use all groups.

## File IO and Logging

IOCP workers use overlapped file IO when `aio on` is configured. With
`sendfile on`, file responses use `TransmitPackets` or `TransmitFile` when
available and fall back to buffered overlapped writes when required by the
response chain or provider.

Windows workstation editions limit native transmit operations and optimize
them for reduced resource use rather than server throughput. IOCP therefore
uses buffered overlapped writes for file responses on workstation editions,
even when `sendfile on` is configured. Windows Server editions retain the
native `TransmitPackets` and `TransmitFile` path.

Only worker slot zero runs cache manager and loader threads. Named mutexes
serialize ownership across reload generations, and shutdown waits for both
threads. Both `error_log stderr` and `access_log stderr` are supported; the
master passes only an explicit duplicate of stderr to each worker.

An empty prefix resolves relative paths from the executable directory, not
the caller's current directory. Worker creation uses the wide Windows API, so
an executable located in a Unicode or long path can respawn workers without
ANSI path conversion.

## Compatibility and Release Checks

Windows dynamic modules must be built against this source tree. The module
signature intentionally rejects modules built against the upstream Windows
ABI, including `--with-compat` modules, because IOCP adds fields to core
structures.

Before packaging, run `nginx.exe -t`, HTTP loopback requests for each enabled
event backend, IOCP connection churn and a UDP loopback request, worker
crash/respawn, graceful
reload and quit, cache loader startup, Unicode executable-path, stderr logging,
TLS, file AIO, and sendfile tests. Confirm the release binary is built without
`--with-debug` and with the intended PCRE, zlib, and OpenSSL dependencies.

Run the shell benchmark drivers from WSL.  They use `cmd.exe` for Windows
telemetry and MSYS2/POSIX helpers for process startup and free-port selection.
`misc/win32-iocp-repeat.sh` covers HTTP keepalive or connection churn, while
`misc/win32-udp-repeat.sh` covers UDP loopback.  Each nginx start also runs
`nginx.exe -t`; release validation must additionally exercise worker respawn,
graceful reload and quit, TLS, file AIO, and sendfile.

For Windows Node 22 runs, set `NODE_BIN` to the repository's
`misc/win32-node22-wrapper.sh` and `NODE_OUTPUT_FILE=1`.  Set
`CLIENT_PROCESSES` above one to start multiple native Node clients; the
aggregated JSONL includes request rate, MiB/s, nginx CPU percentage/cores, and
client CPU percentage/cores.  `WORKER_PROCESSES` can be paired with
`CONTROL_WORKER_PROCESSES` to compare IOCP worker scaling against a control.

The noise-aware benchmark method, completed optimization measurements, and
rollback decisions are recorded in `docs/win32-iocp-performance.md`.

The fork does not currently register nginx directly with the Windows Service
Control Manager. Use a service wrapper that starts the master as a console
process, or treat SCM integration as a separate feature.

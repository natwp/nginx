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
controls outstanding TCP accepts, and `iocp_udp_receives` controls outstanding
UDP receives.

On systems with multiple Windows processor groups, workers are assigned to
groups before their primary thread resumes. This preserves the single-threaded
nginx worker model while allowing `worker_processes auto` to use all groups.

## File IO and Logging

IOCP workers use overlapped file IO when `aio on` is configured. With
`sendfile on`, file responses use `TransmitPackets` or `TransmitFile` when
available and fall back to buffered overlapped writes when required by the
response chain or provider.

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
event backend, an IOCP UDP loopback request, worker crash/respawn, graceful
reload and quit, cache loader startup, Unicode executable-path, stderr logging,
TLS, file AIO, and sendfile tests. Confirm the release binary is built without
`--with-debug` and with the intended PCRE, zlib, and OpenSSL dependencies.

The repeatable smoke/lifecycle matrix is in `misc/win32-rc-test.ps1`. Run it
from Windows PowerShell against a build with PCRE2 and zlib. Pass the OpenSSL
executable used for the build to add a live TLS handshake:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\misc\win32-rc-test.ps1 -Binary .\objs\nginx.exe `
    -OpenSSLBinary C:\path\to\openssl.exe
```

The fork does not currently register nginx directly with the Windows Service
Control Manager. Use a service wrapper that starts the master as a console
process, or treat SCM integration as a separate feature.

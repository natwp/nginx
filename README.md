# nginx-iocp

nginx for Windows with IOCP (I/O Completion Ports) event notification and multi-worker request dispatch.

[![CI](https://github.com/natwp/nginx-iocp/actions/workflows/smoke-tests.yml/badge.svg)](https://github.com/natwp/nginx-iocp/actions)
[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)

## Overview

This is a Windows port of nginx featuring:

- **IOCP Event Backend**: Native Windows I/O Completion Ports for high-concurrency connections
- **Multi-Worker Dispatch**: Router process distributes connections across worker processes
- **Full Module Support**: HTTP/2, SSL/TLS, Stream, UDP routing, and standard nginx modules
- **Measured Performance**: Workload-dependent results, with a [dated CI comparison](#performance) below

Based on [nginx](https://github.com/nginx/nginx) with Windows-specific optimizations.

## Features

### IOCP Event Notification
- Native Windows async I/O via completion ports
- Batch completion processing
- Configurable thread pool for blocking operations

### Multi-Worker Architecture
- Router process handles `accept()` and dispatches to workers
- Even load distribution across worker processes
- UDP flow-aware routing (consistent per-flow worker selection)
- Graceful reload and shutdown

### Complete nginx Feature Set
- HTTP/1.1 and HTTP/2
- SSL/TLS (via OpenSSL)
- Stream module (TCP/UDP proxy)
- Gzip compression
- All standard nginx modules
- Compatible with existing nginx configurations

## Quick Start

### Prerequisites

- Windows 10/11 or Windows Server 2016+
- [MSYS2](https://www.msys2.org/) with MinGW-w64 toolchain

### Build

```bash
# Install dependencies via MSYS2
pacman -S base-devel mingw-w64-x86_64-gcc mingw-w64-x86_64-pcre2 \
          mingw-w64-x86_64-openssl mingw-w64-x86_64-zlib

# Configure and build
./auto/configure \
  --with-cc=gcc \
  --with-pcre \
  --with-http_ssl_module \
  --with-http_v2_module \
  --with-stream \
  --with-stream_ssl_module

make
```

### Run

```bash
# Add "use iocp;" to the existing events block as shown below, then start nginx
./objs/nginx.exe

# Test
curl http://localhost:80
```

### Configuration

```nginx
worker_processes 4;

events {
    use iocp;              # Use IOCP event backend
    worker_connections 2048;
    iocp_threads 1;        # Thread pool for blocking I/O (optional)
}

http {
    server {
        listen 80;
        location / {
            root html;
            index index.html;
        }
    }
}
```

See [nginx documentation](http://nginx.org/en/docs/) for complete configuration reference.

## Performance

CI run 35859988384
completed the full matrix on September 23, 2026 at `42b2436f0` (nginx 1.31.6
plus the shared-listener accept fix). It covers IOCP/select/poll, one/four
workers, 1 KiB/1 MiB responses, HTTP/TLS, static sendfile off/on, and fast/slow
proxy responses. Static and proxy used separate Windows 2022 runners, each
with four logical CPUs; compare backends within the same workload.

Every case has six two-second measurements, each preceded by a one-second
warm-up, using oha 1.16.0 and 32 HTTP/1.1 keepalive connections. All 576
measurements and 576 warm-ups passed HTTP 200, zero-error, and body-size
validation. Balanced ordering and a fixed symmetric log/MAD filter produced
92 stable rates out of 96 and 189 stable paired comparisons out of 208.

The tables show requests/second rounded to integers, followed by retained/raw
sample counts. Rates use the median in log space. **Inconclusive** means the
rate failed the dispersion gate even after filtering. Paired percentage
changes are filtered independently; use the
[full report](docs/windows-ci-benchmarks-2026-09-23.md) for comparisons rather
than dividing these separately filtered medians.

**Static files**

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

**HTTP proxy**

The upstream serves memory-backed bodies with buffering and caching disabled.
Slow responses wait 20 ms before headers; body transfer is unthrottled.
TLS is TLS 1.3 on the client connection; the upstream uses plaintext HTTP.

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

The paired results show substantial workload differences:

- For 1 MiB plaintext static files with `sendfile on`, IOCP measured
  +46.38% versus select and +48.29% versus poll with one worker (6/6 pairs
  each). With four workers, it measured -19.78% (6/6) and -18.08% (5/6).
- Enabling sendfile for one-worker IOCP increased the 1 MiB plaintext rate
  by 52.01% (6/6), while the TLS change was -0.14% (5/6). OpenSSL uses the
  SSL output path, so `sendfile on` does not imply zero-copy TLS transfer.
- Four workers increased IOCP's fast 1 KiB proxy rate by 95.09% over one
  worker for HTTP and 92.53% for TLS (6/6 each). For 1 MiB TLS fast proxy
  responses with four workers, IOCP measured -40.75% versus select and
  -41.35% versus poll (5/6 each).
- All IOCP/select and IOCP/poll comparisons for slow 1 KiB proxy responses
  were within 1%, across both transports and worker counts (5/6 or 6/6).

These are observations from shared CI runners, with the client, nginx, and
upstream competing for CPU. Passing the stability gates is not a statistical
significance test. Measure your workload on your target hardware.

See the [dated results and artifact hashes](docs/windows-ci-benchmarks-2026-09-23.md),
[method and noise policy](docs/windows-ci-benchmarks.md), and
[historical experiments](docs/win32-iocp-performance.md).

The [September 24–25 optimization trials](docs/windows-iocp-trials-2026-09-25.md)
added identical baseline controls and CPU headroom checks. Neither candidate
met the retention gate; no runtime optimization or new backend ranking is
claimed from those trials.

## Testing

Comprehensive test suite covering:
- Smoke tests (core functionality)
- Feature/configuration matrix
- Lifecycle and multi-worker scenarios
- Load/stress testing
- Performance benchmarks

All tests run automatically on CI. See [docs/testing.md](docs/testing.md) for details.

```bash
# Run local smoke tests
pwsh misc/win32-rc-test.ps1 -Binary ./objs/nginx.exe
```

## Documentation

- [IOCP Performance Analysis](docs/win32-iocp-performance.md) - Detailed benchmarks and optimizations
- [Testing Guide](docs/testing.md) - Complete test suite documentation
- [Windows CI Benchmarks](docs/windows-ci-benchmarks.md) - Coverage, sampling, and noise policy
- [nginx Documentation](http://nginx.org/en/docs/) - Official nginx docs

## Project Status

**Stable**: Core IOCP implementation is production-ready with comprehensive test coverage.

- ✅ HTTP/HTTPS serving with IOCP
- ✅ Multi-worker dispatch
- ✅ UDP stream routing
- ✅ Graceful reload/shutdown
- ✅ All standard nginx modules
- ✅ Full CI/CD test suite

## Contributing

Contributions welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes with tests
4. Run the test suite locally
5. Submit a pull request

All PRs run through automated testing (builds, smoke tests, integration tests).

## Architecture Notes

### IOCP Backend
- `src/event/ngx_event_iocp_module.c` - Core IOCP event handler
- `src/os/win32/ngx_wsasend.c` - Overlapped send operations
- `src/os/win32/ngx_wsarecv.c` - Overlapped receive operations

### Multi-Worker Dispatch
- Router process owns listening sockets
- `AcceptEx()` with attached worker process info
- Load balancing via round-robin with backpressure detection

### UDP Routing
- Flow-aware hashing (5-tuple: src/dst IP/port + protocol)
- Consistent worker selection per flow
- Backpressure handling without flow remapping

See [Windows Port Notes](docs/win32.md) for the process and event model.

## License

nginx is licensed under the [2-clause BSD license](LICENSE).

This Windows IOCP port maintains the same license.

## Links

- **Repository**: https://github.com/natwp/nginx-iocp
- **CI/CD**: https://github.com/natwp/nginx-iocp/actions
- **Upstream nginx**: https://github.com/nginx/nginx
- **nginx Website**: http://nginx.org/

## Acknowledgments

- nginx team for the excellent web server
- MSYS2 project for Windows toolchain
- Contributors to Windows nginx ports

---

For questions or issues, please open a GitHub issue.

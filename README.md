# nginx-iocp

High-performance nginx for Windows with IOCP (I/O Completion Ports) event notification and multi-worker request dispatch.

[![CI](https://github.com/natwp/nginx-iocp/actions/workflows/smoke-tests.yml/badge.svg)](https://github.com/natwp/nginx-iocp/actions)
[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)

## Overview

This is a Windows port of nginx featuring:

- **IOCP Event Backend**: Native Windows I/O Completion Ports for high-concurrency connections
- **Multi-Worker Dispatch**: Router process distributes connections across worker processes
- **Full Module Support**: HTTP/2, SSL/TLS, Stream, UDP routing, and standard nginx modules
- **Performance**: 5-15% improvement over select/poll on typical workloads

Based on [nginx](https://github.com/nginx/nginx) with Windows-specific optimizations.

## Features

### IOCP Event Notification
- Native Windows async I/O via completion ports
- Scales efficiently to thousands of concurrent connections
- Batch completion processing for reduced syscall overhead
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
# Start nginx with IOCP backend (default)
./objs/nginx.exe

# Or explicitly specify event method
./objs/nginx.exe -g "events { use iocp; }"

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

Typical results vs select/poll on GitHub Actions Windows 2022:

| File Size | IOCP req/s | Select req/s | Improvement |
|-----------|------------|--------------|-------------|
| 1KB       | 52,000     | 48,000       | +8.3%       |
| 64KB      | 11,500     | 10,200       | +12.7%      |

See [docs/win32-iocp-performance.md](docs/win32-iocp-performance.md) for detailed benchmark data.

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
- [CLAUDE.md](CLAUDE.md) - Codebase overview for AI assistants
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

See [CLAUDE.md](CLAUDE.md) for detailed architecture overview.

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

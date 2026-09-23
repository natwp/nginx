# nginx-iocp Test Suite

Comprehensive testing infrastructure for the nginx Windows IOCP port.

## Quick Start

All CI tests run automatically on push/PR. Manual tests can be triggered via GitHub Actions UI.

```bash
# View all workflows
https://github.com/natwp/nginx-iocp/actions

# Monitor latest run
https://github.com/natwp/nginx-iocp/actions/workflows/smoke-tests.yml
```

## Test Suite Overview

### 🔄 Continuous Integration (Automatic)

These workflows run on every push to master, fix/*, or feature/* branches:

| Workflow | Duration | Purpose |
|----------|----------|---------|
| **Smoke Tests** | 15 min | Quick validation of core functionality |
| **Feature Tests** | 30 min | Module combinations and configurations |
| **Integration Tests** | 30 min | Lifecycle and multi-worker scenarios |
| **Windows Build** | 10 min | Release/Debug builds and native RC matrix |
| **Code Quality** | 5 min | Static analysis |

**Total parallel CI time: ~90 minutes** (all run concurrently)

### 🎯 Manual/Scheduled Tests

Trigger manually or run on schedule for deep validation:

| Workflow | Duration | Schedule | Purpose |
|----------|----------|----------|---------|
| **Load Tests** | 60 min | Weekly (Sun 2 AM) | Stress testing with various loads |
| **Benchmarks** | 90 min | Monthly (1st, 3 AM) | IOCP vs Select performance |
| **Release** | 15 min | Version tags, fix/release-* branches, manual | Optimized release builds and package checks |

The Windows build matrix enables mail and tests IMAP/POP3 greetings and
first/subsequent commands on IOCP, select, and poll, both after startup and
after worker replacement. The RC helper tests also run under Windows
PowerShell 5 to verify header parsing and HTTP probe disposal.

Release branch pushes and manual dispatch are dry runs. They build, test,
package, and attest the candidate; only a version tag push publishes it.

## Test Coverage

### ✅ Smoke Tests
- Binary execution and version check
- Configuration validation
- All event methods (IOCP, select, poll)
- Multi-worker lifecycle (start/reload/quit)
- HTTPS with OpenSSL
- Gzip compression
- PCRE2 regex support

### 🔧 Feature Tests
Module combinations tested:
- **Core**: HTTP SSL, HTTP/2, realip
- **Content**: addition, sub, gunzip, gzip_static
- **Stream**: SSL, realip, ssl_preread
- **Extended**: DAV, FLV, MP4, random_index, secure_link, slice, stub_status

Configuration matrix:
- Event methods: iocp, select, poll
- Worker counts: 1, 2, 4

### 🔄 Integration Tests
- Graceful shutdown
- Configuration reload under load
- Multi-worker coordination
- Log rotation (reopen signal)
- Event method switching

### 📊 Load Tests
File sizes tested:
- Small (1KB)
- Medium (64KB)
- Large (1MB)

Scenarios:
- HTTP/1.1 keepalive
- Connection churn (no keepalive)
- High concurrency stress

### 🏎️ Performance Benchmarks
- **IOCP vs Select** backend comparison
- Multiple file sizes: 100B, 1KB, 4KB, 16KB, 64KB
- Configurable test suites:
  - Quick: 10s, 50 connections, 1 run
  - Standard: 30s, 100 connections, 3 runs
  - Comprehensive: 60s, 200 connections, 5 runs
- UDP stream benchmarks

## Running Tests

### Local Testing

```bash
# Build nginx
./auto/configure --with-cc=gcc --with-pcre --with-http_ssl_module --with-http_v2_module
make

# Run smoke tests
pwsh misc/win32-rc-test.ps1 -Binary ./objs/nginx.exe
```

### Manual Workflow Triggers

**Load Tests:**
1. Go to https://github.com/natwp/nginx-iocp/actions/workflows/load-tests.yml
2. Click "Run workflow"
3. Configure: duration (30s, 1m, 5m), connections (100, 200)

**Performance Benchmarks:**
1. Go to https://github.com/natwp/nginx-iocp/actions/workflows/benchmarks-extended.yml
2. Click "Run workflow"
3. Select `test_suite` (quick/standard/comprehensive) and the multi-worker
   `worker_count` (2, 4, or 8; always compared with one worker).

All suites cover 1 KiB/1 MiB static files with sendfile off/on, fast/delayed
HTTP proxy responses, TLS off/on, and IOCP/select/poll. The default quick
suite uses six two-second samples for every case, each after a one-second
warm-up. See the [benchmark method](windows-ci-benchmarks.md) for the complete
matrix, balanced ordering, and fixed noise policy.

## Interpreting Results

### CI Status
- ✅ All green = ready to merge
- ❌ Red = check logs for failures
- 🟡 Yellow = workflow in progress

### Load Test Results
Look for in workflow logs:
- **Requests/sec**: Throughput
- **Latency percentiles**: P50, P95, P99
- **Error rate**: Should be 0%
- **Resource usage**: CPU/memory from nginx logs

### Benchmark Results
Download `benchmark-results-static` and `benchmark-results-proxy` from a
completed run. `summary.md` contains rates and paired comparisons;
`summary.json` records retained counts, exclusions, and reasons for
inconclusive results. `samples.jsonl`, `raw/`, and `manifest.json` preserve
measurements and their build/runner context.

Only comparisons passing the sample-count, dispersion, and execution-order
gates support a reported difference. An inconclusive result does not establish
a winner. HTTP errors or wrong response sizes fail the job. See the
[README performance snapshot](../README.md#performance) for completed CI
evidence; backend performance depends on the workload and host.

## Test Artifacts

All workflows upload logs and results on completion:

```
Artifacts available for 7-90 days depending on workflow
Download from: GitHub Actions run page → Artifacts section
```

| Workflow | Artifact Name | Contents |
|----------|---------------|----------|
| Smoke | smoke-test-logs | Error logs on failure |
| Feature | nginx-{module}-build | Built binaries per module set |
| Integration | integration-test-logs | Lifecycle test logs |
| Load | load-test-results | Performance logs, configs |
| Benchmarks | benchmark-results-{suite} | JSON results, comparison data |
| Release | nginx-iocp-{version} | Optimized release binary |

## Maintenance

### Adding New Tests
1. Edit appropriate workflow YAML in `.github/workflows/`
2. Test manually via workflow_dispatch
3. Update `.github/workflows/README.md`

### Modifying Parameters
Edit workflow files directly:
- Connection counts
- Test duration
- Worker process counts
- Test matrices

### Troubleshooting
- **Hangs**: Check timeout settings, review last successful step
- **Random failures**: Port conflicts, resource contention
- **Build failures**: Verify MSYS2 packages, configure flags

## Performance Baselines

Current typical results on GitHub Actions Windows 2022:
- **IOCP improvement over select**: 5-15%
- **Multi-worker scaling**: Near-linear up to 4 workers
- **Small files (1KB)**: 50,000+ req/s
- **Large files (64KB)**: 10,000+ req/s
- **UDP datagram rate**: Measured in dedicated benchmarks

See benchmark artifacts for detailed historical data.

## Workflow Details

For complete documentation of each workflow, see `.github/workflows/README.md`.

## CI Status Badge

Current build status: [![CI](https://github.com/natwp/nginx-iocp/actions/workflows/smoke-tests.yml/badge.svg)](https://github.com/natwp/nginx-iocp/actions)

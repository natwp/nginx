# GitHub Actions CI/CD Workflows

This directory contains all automated testing and release workflows for nginx-iocp.

## Workflow Overview

### Continuous Integration (Auto-Run on Push/PR)

#### 1. **windows-build.yml** - Windows Build and Test
- **Triggers**: Push to master, fix/*, feature/* branches; Pull requests
- **Duration**: ~10 minutes
- **Purpose**: Build verification with all major modules
- **Modules tested**: HTTP SSL, HTTP/2, Stream SSL, Mail SSL, gzip, stub_status
- **Tests**: Release and Debug builds; IOCP/select/poll lifecycle and TLS;
  IMAP/POP3 greetings and commands after startup and worker replacement;
  HTTP header parsing under PowerShell 7 and Windows PowerShell 5

#### 2. **smoke-tests.yml** - Quick Smoke Tests
- **Triggers**: Push to master, fix/*, feature/* branches; Pull requests
- **Duration**: ~15 minutes
- **Purpose**: Fast validation of core functionality
- **Coverage**:
  - Binary execution and help output
  - Configuration parsing
  - Release candidate smoke tests (via `misc/win32-rc-test.ps1`)
  - IOCP, select, poll backends
  - Multi-worker lifecycle (start, reload, quit)
  - HTTPS support (with OpenSSL)
  - Gzip compression
  - PCRE2 regex

#### 3. **feature-tests.yml** - Module and Configuration Tests
- **Triggers**: Push to master, fix/*, feature/* branches; Pull requests
- **Duration**: ~30 minutes
- **Purpose**: Validate different module combinations and configurations
- **Test Matrix**:
  - Core Modules (SSL, HTTP/2, realip)
  - Content Modules (addition, sub, gunzip, gzip_static)
  - Stream Modules (stream SSL, realip, ssl_preread)
  - Extended Features (DAV, FLV, MP4, random_index, secure_link, slice, stub_status)
- **Configuration Tests**:
  - Event methods: IOCP, select, poll
  - Worker counts: 1, 2, 4

#### 4. **integration-tests.yml** - Lifecycle and Multi-Worker Tests
- **Triggers**: Push to master, fix/*, feature/* branches; Pull requests
- **Duration**: ~30 minutes
- **Purpose**: Test complex lifecycle scenarios
- **Coverage**:
  - Graceful shutdown
  - Configuration reload
  - Multi-worker coordination (1, 2, 4 workers)
  - Log rotation (reopen signal)
  - Event method switching (IOCP, select, poll)

#### 5. **code-quality.yml** - Code Quality Checks
- **Triggers**: Push to master, fix/*, feature/* branches; Pull requests
- **Duration**: ~5 minutes
- **Purpose**: Static analysis and code quality
- **Checks**: Formatting, style, potential issues

### Manual/Scheduled Workflows

#### 6. **load-tests.yml** - Load and Stress Tests
- **Triggers**: Manual dispatch, Weekly (Sundays at 2 AM UTC)
- **Duration**: ~60 minutes
- **Purpose**: Stress testing under various loads
- **Tests**:
  - Small file (1KB) load test
  - Medium file (64KB) load test
  - Large file (1MB) load test
  - Keepalive stress test
  - Connection churn test
- **Tool**: oha (HTTP load generator)
- **Configurable**: Duration, connection count

#### 7. **benchmarks-extended.yml** - Performance Benchmarks
- **Triggers**: Manual dispatch, monthly (1st at 3 AM UTC), benchmark changes
  on `fix/benchmark-*` branches
- **Execution**: Build/smoke job, then separate static and proxy measurement jobs
- **Purpose**: Repeated comparisons with a fixed noise filter and raw evidence
- **Test Suites**:
  - **Quick**: 2s samples, 6 rounds (default)
  - **Standard**: 3s samples, 6 rounds
  - **Comprehensive**: 3s samples, 12 rounds
- **Every Case**: 1s warm-up, 32 keepalive connections, balanced run order
- **Backends Compared**: IOCP, select, poll
- **Matrix**: 1 KiB/1 MiB, sendfile off/on, fast/delayed HTTP proxy, TLS off/on,
  one/multiple workers; all suites cover all 96 cases
- **Configurable**: Test suite, multi-worker count (2/4/8, default 4)
- **Method and limitations**: [Windows CI benchmarks](../../docs/windows-ci-benchmarks.md)

#### 8. **release.yml** - Release Build
- **Triggers**: Version tags (v*), fix/release-* branches, manual dispatch
- **Duration**: ~15 minutes
- **Purpose**: Create optimized release builds
- **Artifacts**: Windows x64 package, SHA256 checksum, provenance attestation
- **Publication**: Only version tag pushes publish a GitHub release. Branch
  pushes and manual dispatch run the same build, RC, and package checks as
  a dry run, without publishing.

## Usage

### Running Tests Locally

Before pushing, you can validate changes locally:

```bash
# Build and run smoke tests
# Run from the repository root
./auto/configure --with-cc=gcc --with-pcre --with-http_ssl_module
make
pwsh misc/win32-rc-test.ps1
```

### Triggering Manual Workflows

**Load Tests:**
```
GitHub UI → Actions → Load and Stress Tests → Run workflow
  - duration: 30s (default), or 1m, 5m, etc.
  - connections: 100 (default)
```

**Performance Benchmarks:**
```
GitHub UI → Actions → Performance Benchmarks (Extended) → Run workflow
  - test_suite: quick / standard / comprehensive
  - worker_count: 4 (default)
```

### Interpreting Results

#### CI Workflows (Push/PR)
All CI workflows must pass before merging:
- ✅ Green check = all tests passed
- ❌ Red X = failures need investigation
- Check "Details" link for specific test failures

#### Load Test Results
Look for:
- Request rate (req/s)
- Error count (should be 0)
- Response time percentiles
- Server resource usage in logs

#### Benchmark Results
Compare IOCP vs Select performance:
- Download artifacts from workflow run
- Read `summary.md` for filtered rates and paired comparisons
- Positive changes favor the named candidate over its matched control
- Check retained counts and `summary.json` for inconclusive reasons
- Keep raw samples and the manifest when citing results

## Artifacts

Workflows upload test artifacts on completion:

| Workflow | Artifact | Contents | Retention |
|----------|----------|----------|-----------|
| smoke-tests | smoke-test-logs | Error logs on failure | 7 days |
| feature-tests | nginx-{module}-build | Built binaries | 3 days |
| integration-tests | integration-test-logs | Lifecycle test logs | 7 days |
| load-tests | load-test-results | Logs, configs | 14 days |
| benchmarks-extended | benchmark-results-{static,proxy} | Raw/filtered results, manifest, configs, logs | 90 days |
| benchmarks-extended | benchmark-smoke-results | All-case correctness smoke results | 14 days |
| benchmarks-extended | benchmark-binaries | nginx, native helper, OpenSSL, oha | 7 days |
| release | nginx-iocp-{version} | Release binary | 90 days |

## Test Coverage Summary

```
├── Build Verification
│   └── windows-build.yml (10 min)
│
├── Functional Testing
│   ├── smoke-tests.yml (15 min)          ← Quick validation
│   ├── feature-tests.yml (30 min)        ← Module combinations
│   └── integration-tests.yml (30 min)    ← Lifecycle scenarios
│
├── Performance Testing
│   ├── load-tests.yml (60 min)           ← Stress testing
│   └── benchmarks-extended.yml          ← Build, then parallel static/proxy jobs
│
├── Quality Assurance
│   └── code-quality.yml (5 min)
│
└── Release
    └── release.yml (15 min)
```

**Total CI Time per Push**: ~90 minutes (parallel execution)
**Coverage**: Build, smoke, features, integration, quality = 5 workflows

## Dependencies

All workflows use:
- **MSYS2**: MinGW-w64 GCC toolchain
- **System Libraries**: PCRE2, OpenSSL, zlib (from MSYS2 packages)
- **Testing Tools**:
  - Node.js 22 (for benchmark scripts)
  - oha v1.4.5 (HTTP load testing)
  - PowerShell (test orchestration)

## Maintenance

### Adding New Tests

1. Add test logic to appropriate workflow file
2. Test manually via workflow_dispatch
3. Adjust timeout if needed
4. Update this README

### Modifying Test Parameters

Edit workflow YAML files directly:
- Connection counts
- Duration values
- Worker process counts
- Test matrices

### Disabling Workflows

Comment out trigger section or disable via GitHub UI:
```yaml
# on:
#   push:
#     branches: [ master ]
```

## Troubleshooting

**Workflow hangs:**
- Check timeout settings
- Review last successful step in logs
- Look for stuck nginx processes

**Random failures:**
- Check for port conflicts
- Review CPU/memory in workflow logs
- Consider increasing sleep delays

**Build failures:**
- Verify MSYS2 package availability
- Check configure flags compatibility
- Review compiler error messages

**Test failures:**
- Download test artifacts
- Check error logs
- Compare with previous successful runs

## Performance Baselines

See benchmark artifacts for current performance data. Typical results:

- **IOCP vs Select**: 5-15% improvement for IOCP
- **Multi-worker scaling**: Near-linear up to 4 workers
- **Small files (1KB)**: 50,000+ req/s
- **Large files (64KB)**: 10,000+ req/s

All measurements on GitHub Actions Windows 2022 runners.

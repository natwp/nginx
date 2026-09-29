# nginx-iocp testing

Pull requests run short commit-message and whitespace checks. Windows builds,
correctness tests, and diagnostics run only by manual dispatch. Select one
relevant workflow and wait for it to finish before requesting another.
See the [workflow guide](../.github/workflows/README.md) for job limits.

## Correctness coverage

- Windows Build and Test builds one Release or Debug configuration per run.
  The native RC matrix covers IOCP/select/poll, HTTP/TLS, file I/O, reload,
  cancellation, and worker lifecycle. It includes mail protocol greetings
  and commands, and PowerShell 5 compatibility checks.
- Smoke Tests checks executable startup, configuration, core HTTP behavior,
  and process lifecycle.
- Feature Tests checks module configurations and event methods.
- Integration Tests checks reloads, graceful shutdown, worker coordination,
  log rotation, and switching event methods.
- Code Quality checks configure failure reporting, helper syntax, result
  validation, and the statistical gates used for saved measurements.

Required checks must finish successfully for the exact candidate revision.
A skipped or unrelated workflow is not validation. Keep unvalidated runtime
changes on their own branch until their correctness and retention gates pass.

## Local correctness checks

Run commands from the repository root. Enable the modules required by the
change, keep build output and temporary files inside the workspace, and run
one task at a time. Never run local benchmarks.

```sh
./auto/configure --with-debug
make -j1
./objs/nginx -V
git diff --check
```

Use the MSYS2 MinGW64 login shell for a Windows build. With the required
modules enabled, validate the binary using `nginx.exe -t`, a loopback request,
and graceful reload/quit. The native helper is `misc/win32-rc-test.ps1`.

## Profiling and performance evidence

Full load tests, extended benchmark matrices, and performance trials are
paused. Their job definitions remain disabled while profiling guides focused
bottleneck investigations. Do not treat skipped jobs as passing gates or
weaken a retention gate to accommodate noisy runners.

Analyze saved raw profiles and call counts first. Use narrowly scoped hosted
profiles only when needed. Report sampling limits, instrumented-build overhead,
client saturation, and incomplete cases. No broad IOCP performance advantage
or multi-worker scaling improvement has been established by the latest trials.

Historical evidence and methods:

- [Windows benchmark matrix](windows-ci-benchmarks.md)
- [September 23 results](windows-ci-benchmarks-2026-09-23.md)
- [IOCP candidate trials](windows-iocp-trials-2026-09-25.md)
- [Optimization and retention protocol](windows-iocp-optimization.md)

Preserve raw artifacts, source provenance, errors, and digests. Historical run
IDs and source hashes refer to the originally recorded measurements.

## Release checks

The release workflow supports manual package validation with native RC tests,
checksums, and build provenance. Automatic publication is disabled; a manual
dry run does not publish a release.

# GitHub Actions workflows

Dispatch one focused Windows workflow at a time. Heavy workflows share the
`nginx-heavy-ci` concurrency group across all branches and workflow types.
Matrix jobs use `max-parallel: 1`; builds use one compiler process.
Only short commit-message and whitespace checks run automatically on pull
requests. No workflow runs on a schedule or starts a build on push.

| Workflow | Trigger | Job timeout |
| --- | --- | --- |
| Commit message / whitespace | Pull request | 5 minutes |
| Windows Build and Test | Manual, Release or Debug | 20 minutes |
| Smoke Tests | Manual | 15 minutes |
| Feature Tests | Manual | 15 minutes |
| Integration Tests | Manual | 15 minutes |
| Code Quality | Manual | 10 minutes |
| Release package validation | Manual, dry run | 20 minutes |

Select only the suite that covers the change. The Windows build defaults to
Release; request Debug separately when needed. Do not dispatch another heavy
workflow until the current run finishes. GitHub concurrency keeps only one
pending run, so it is not a queue for a batch of requested suites.

Load tests, extended benchmark matrices, and the IOCP performance trial are
paused at the job level. Their harnesses, artifacts, and retention gates remain
available for review. A skipped workflow does not validate correctness or
performance. Resume broad measurements only with a suitable execution budget
and an environment that can produce useful evidence.

Use saved profiles and targeted diagnostics to identify bottlenecks before
requesting another measurement. Instrumented rates are not performance gains,
and noisy or incomplete samples do not satisfy the retention gate.

Manual release runs build, test, package, and attest the selected revision.
They do not publish releases. Automatic branch/tag publication is disabled.

Keep raw artifacts and manifests when documenting results. Historical CI run
identifiers, source hashes, and artifact digests describe the original runs;
they are not claims that those runs exist under a new repository owner.

For coverage and local correctness checks, see [testing](../../docs/testing.md).
For historical measurement methods, see the
[benchmark matrix](../../docs/windows-ci-benchmarks.md) and
[optimization protocol](../../docs/windows-iocp-optimization.md).

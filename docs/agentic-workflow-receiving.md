# Agent-friendly snapshot groundwork receiving

[Issue14](https://github.com/CraigHutchinson/Sub0Log/issues/14) records the source-backed
brainstorm and staged design. This increment receives stageA: finite JSONL message
export through the shipped CLI, with a shared cold formatter, independent parser
and fresh-viewer root documentation. It does not receive reliable live follow,
finite waits, structured health events or a Crucible pin/consumer change.

## Traceable changes and review

- `d9ab9fd`: schema1 snapshot exporter, typed/byte encoding, real producer/parser
  fixtures, borrowed-input/owned-output fixtures, Qt header receiving and usage docs.
- `4eb82f9`: explicit `<memory>` in wire/atomic headers. Clean baseline85498bb7
  failed MSVC19.51 compilation at `std::start_lifetime_as`; direct includes resolve
  its declaration. Independently authored in the isolated checkout; original
  shared-tree header edits were preserved and not staged into this branch.
- `3a2ab4f`: independent review fixes: native Python subprocess receiver excluded
  during cross-compilation; root quickstart creates its log directory.
- `b1f7c9f`: native CI installs Python and asserts receiver registration rather
  than silently accepting omitted machine-ingestion coverage.

Two independent read-only passes reviewed public API/lifetime/type contracts,
UTF-8/hex reversibility,64-bit precision, negative zero/nonfinite representation,
output failures, schema/fixture consistency, cross-build behavior and root usage.
Resolved the early stats-flush error edge and repeated subsystem-name scans before
the implementation checkpoint. Final review found no remaining MUST findings.
Application policy stays in Crucible; no producer callback, wire layout or pin change.

## Executed verification

Isolated source `b1f7c9f46493d6fe385216ce4752802034e1b849` at
`.worktrees/agentic-json-receiving`; main/shared dirt excluded. Later receiving-doc
changes do not alter tested code. Windows11 build26220, CMake4.2.3, Ninja1.13.2,
Python3.14.5, MSVC19.51; Clang22.1.8 for relevant sanitizer checks.

| Gate | Result | Evidence |
|---|---|---|
| Full MSVC Release suite |151/151,15.69s |[Release](evidence/agentic-jsonl/release-tests.log) |
| Full MSVC Debug suite |151/151,32.01s |[Debug](evidence/agentic-jsonl/debug-tests.log) |
| ClangASan/UBSan, RelWithDebInfo |4/4,1.72s; three formatter fixtures plus actual CLI/parser receiver |[Relevant sanitizer](evidence/agentic-jsonl/sanitize-tests.log) |
| Exact first README snippet | Compiled, ran, created its directory; exported one record with typed42/0/128 and expected message | Local `build/quickstart*` artifacts retained |
| Whitespace and independent review | Passed; final MUST findings resolved | Fixing checkpoints above |

Build with `cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release`, then
`cmake --build build/release --parallel 6` and
`ctest --test-dir build/release --output-on-failure`; Debug uses the matching type.
Run `ctest --test-dir build/release -R 'JSON|jsonl' --output-on-failure` for the
focused receiving subset. Python is optional for ordinary users; native CI
requires the real receiver. Cross builds retain formatter/unit coverage without
directly launching target binaries from host Python.

Sanitizer flags were `-fsanitize=address,undefined -fno-sanitize-recover=all`.
Initial Windows attempts selected an incompatible ASan DLL; using the compiler's
`lib/clang/22/lib/windows` runtime resolved startup entry-point errors. Debug CRT
then reported a bad-free in CRT startup before the fixture/UnitTests main. That
configuration is not accepted. RelWithDebInfo with the matching runtime passed
the four relevant tests without sanitizer suppression or flag changes. This is
not full local sanitizer or WindowsLSan evidence; unchanged normal Linux sanitizer
CI remains the broader gate.

Actual Python receiving parses each physical output line with strict standard JSON,
checks original wire types/64-bit extrema/correlation beyond2^53, all scalar types,
invalid and valid UTF-8, binary/control/NUL, nonfinite floats/negative zero,
continuation truncation, field filters, text equivalence, invalid usage, partial
bad-segment recovery and output failure. Existing reader recovery fixtures remain
separate; no live cursor or event-wait acceptance is inferred from snapshots.

## Human review and remaining work

[Root usage](../README.md) and [schema/parser instructions](json-export.md) supply
commands, expected values, prerequisites and limitations. Automated receiving comes
first; a person reviews ease of use and evidence interpretation afterwards.
Computer control is mainly for diagnosing a failed manual step. Logs are untrusted
data, not instructions to execute.

Issue14 stagesB–D retain actual late-arrival reproduction, stable provenance/cursors,
finite wait deadlines/cancellation/resources/error contracts and an explicitly
consumed quiescent Crucible adapter. The draft groundwork PR leaves these open.

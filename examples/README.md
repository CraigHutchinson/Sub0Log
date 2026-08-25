# Sub0Log examples

A runnable ladder from "hello, typed record" to the multi-process, crash-surviving
case the library exists for. Each example is a single self-contained `.cpp`
with its own `main()`; each prints readable output explaining what it just
did, and exits `0` on success.

## A note on the comments in this directory

Library code comments explain *why* to a maintainer who already knows the
shape of the thing. The comments in `examples/` are teaching material instead:
they explain *what and why* to a newcomer meeting Sub0Log for the first time,
which means they are more generous with prose than `STYLE_GUIDE.md` would
otherwise call for. This is a deliberate departure from the rest of the
codebase, not an oversight. It still stops short of narrating the obvious --
nothing here explains what `++i` does.

## The ladder

| # | File | What it teaches | Requirements |
|---|------|------------------|--------------|
| 01 | `01_hello.cpp` | Create a `Logger`, log a few typed records, read them back with `SegmentReader` + `Decoder`. Nothing formats at the call site. | R1, R2.1 |
| 02 | `02_subsystems.cpp` | The consumer, not the library, owns the subsystem vocabulary. Filtering ("Storage records at Warning or above") is field comparisons, never a text search. | R2.2, R2.3 |
| 03 | `03_correlation.cpp` | `CorrelationScope` on several threads; joining an activity's records back together is an equality test on `correlationId_`. | R6.1, R6.2 |
| 04 | `04_crash_handler.cpp` (POSIX) | Logging *from inside a signal handler*, and a hard `SIGKILL` with no handler at all -- both survive with no graceful shutdown anywhere. The example the whole library is for. | R3.1, R3.2 |
| 05 | `05_multi_process.cpp` (POSIX) | A parent and a forked child each write their own segment; `Merger` produces one time-ordered stream, correlation crossing the process boundary via the environment. | R5.1-R5.4 |
| 06 | `06_child_capture.cpp` (POSIX) | Capturing a real third-party subprocess's stdout/stderr as attributed records, with a `LineInterceptor` that suppresses noise and harvests a value live. | R5.5, R5.6 |
| 07 | `07_live_tail.cpp` | A miniature console tailer: a reader repeatedly re-opens the same segment a producer thread is still writing. The console view is a view. | R3 (read side), R5.2 |
| 08 | `08_testing_your_code.cpp` | How a consumer tests their own logging code with no test framework: `ScopedBind` over a temp directory, assertions on decoded fields, the drop counter. | R7.1, R7.2, R2.3, R9.1 |

Examples 04-06 are POSIX-only (`fork`, `execvp`, POSIX signals): each still
builds and runs on Windows, but its `main()` prints a one-line note that the
Windows arm of that story is a v2 item (`docs/architecture.md`'s phasing) and
returns `0` rather than attempting a stub.

## Building

From the repository root:

```sh
cmake -S . -B build -DSUB0LOG_BUILD_EXAMPLES=ON
cmake --build build
```

This adds one executable per example, named `sub0log_example_01_hello`
through `sub0log_example_08_testing_your_code`, each linking
`Sub0Log::Sub0Log` the same way any downstream consumer would.

## Running

Run one directly:

```sh
./build/examples/sub0log_example_01_hello
```

Or, when `SUB0LOG_BUILD_TESTING` is also on (the default), run the whole
ladder as ctest smoke tests, labelled `example`:

```sh
ctest --test-dir build -L example --output-on-failure
```

Every example is self-contained: it creates its own scratch directory under
the system temp path, cleans it up before exiting, and finishes in a few
seconds with no arguments and no network access required.

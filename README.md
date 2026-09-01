# Sub0Log

**A C++23 structured diagnostic stream: typed records, no producer formatting, and a log that survives a hard kill.**

```cpp
#include <sub0log/log.hpp>

sub0log_debug(Storage, "read {} at {} for {} bytes", blobId, offset, length);
```

Nothing above formats a string, allocates, or takes a lock. The format text,
file, line, subsystem, severity and argument types live in a descriptor emitted
once per call site; the record carries the raw argument bytes and a reference to
that descriptor. Text is produced later, by whoever wants text -- and in a
headless run where nobody does, it is never produced at all.

---

## Why

Most C++ loggers answer "make this fast" by deferring formatting to a background
thread. That is the right first move, and several libraries do it well. Two
problems survive it.

**The record arrives as characters.** By the time a sink is invoked, an `int` has
become `"42"`. Filtering on a numeric field means parsing text back out, joining
related records means substring matching, and a test asserts on wording rather
than on values.

**A staging queue is process memory.** Records sitting in a frontend queue when
the process is killed are gone, whatever the sink does with the ones that made
it out. That rules out the two cases where a log matters most: the crash handler
and the post-mortem of a hung process.

Sub0Log removes the staging step. A producer claims a chunk of a file-backed
mapping with one atomic and copies its arguments in, so a record is in
kernel-owned memory the moment the call returns.

## Background

`docs/` carries the groundwork rather than the plan: a survey of the existing
low-latency C++ loggers and what each one costs, the platform tracers and the
question of whose buffer a record lands in, what a hard kill actually destroys,
and the framing and recovery problems as the established binary formats already
solved them. Start at `docs/README.md`. Two of those files record a claim that
was wrong first and say why -- that is deliberate.

## Examples

`examples/` is a ladder, from a five-minute hello-world to the cases the
design exists for: a fatal record written from inside a signal handler and
recovered after the process is killed, two processes merged into one ordered
stream, a third-party tool's output captured and intercepted, and a console
tailer reading a segment while it is still being written.

```sh
cmake -S . -B build -DSUB0LOG_BUILD_EXAMPLES=ON && cmake --build build
ctest --test-dir build -L example      # every example, run unattended
```

They are registered as tests on purpose: an example nobody builds is an
example that has already stopped compiling. See `examples/README.md`.

## Reading records

`sub0log-cat` ships with the library and is built by default:

```sh
sub0log-cat /var/log/myservice            # every segment in the directory, merged
sub0log-cat -l error -s 3 --follow .      # errors from subsystem 3, as they arrive
```

A directory argument means every `*.s0l` inside it, and several segments are
merged onto one timeline -- which is how a group of processes is meant to be
read. Filtering is on the fields a record actually carries (severity,
subsystem, correlation), never a text search of the rendered message.

## Operating it

Three things a service needs to know before it runs this in anger, none of
which the API can tell you at the point you need them:

**A segment does not wrap.** It is sized once, at `Logger::create`, and when
it fills every later record is dropped and counted -- permanently, while the
process keeps running perfectly. That is deliberate: rotation policy is where
logging libraries go to acquire configuration surfaces, and `REQUIREMENTS.md`
puts it out of scope. The intended pattern is a new segment per run, or per
interval, with `Merger` putting them back into one ordered stream at read
time. Size `segmentBytes_` for the rate you expect, and alert on the drop
counter rather than on the file.

**Watch two counters and one number.** `Logger::stats()` gives dropped and
truncated records; `sub0log::unboundEmits()` gives call sites that reached no
instance at all (R9.3) -- nonzero means something is logging into nowhere,
which no other signal will tell you. All three are relaxed atomic loads, safe
and cheap to poll from a metrics thread. There is deliberately no callback:
that would put your code on the emit path. `examples/09_operating.cpp` is the
whole story, runnable.

**Including it is not free.** `<sub0log/log.hpp>` costs about 420 ms per
translation unit over an empty one on GCC 13 at `-O2`, roughly 160 ms more
than a TU that already includes `<string>` and `<vector>`. That is the price
of header-only, and it is worth knowing before the include goes into a widely
included header. `<sub0log/reader.hpp>` costs another 220 ms and belongs in
tools and tests rather than in producer TUs.

## Status

Running, v2 complete. v1 built the producer and reader paths -- a round-trip
through a real file, records surviving a hard-killed producer, a two-process
merge. v2 added the plugin C ABI (a shared library reaches the host's stream
without linking the library or duplicating its instance pointer), argument
chains past the 512-byte inline cap, and child capture's Windows arm. The
architecture and phasing are in `docs/architecture.md`; `REQUIREMENTS.md` is
the contract this library is built to, and remains the right place to
disagree.

What a new consumer actually hits is written down rather than left to be
discovered: `docs/adoption-friction.md` is the register -- measured, not
guessed -- of the bugs, the gaps and the friction that is deliberate, each
with what was done about it. Everything in it is resolved. The last to close
was a plugin built with hidden visibility getting its own instance pointer
and logging into nowhere; it took two steps, a counter that made the silence
audible and then the C ABI (R4) that removed the cause.

## License

MIT. See `LICENSE.md`.

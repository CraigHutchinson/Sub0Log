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

## Status

Early, but running. The v1 producer and reader paths are implemented and
tested -- including a round-trip through a real file, records surviving a
hard-killed producer, and a two-process merge -- with the architecture in
`docs/architecture.md`. `REQUIREMENTS.md` is the contract this library is
built to, and remains the right place to disagree.

## License

MIT. See `LICENSE.md`.

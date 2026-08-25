# Memory: what is allocated, what is reserved, and what a small target needs

R1.2 says a call site must not allocate. That is true, and it is also
narrower than "this library is cheap on memory" -- the way the producer path
avoids allocating is by *reserving in advance*, which is a different
property and a much larger number. This file is the ledger, written because
a claim about allocation that is only true of the hot path invites someone
to deploy the whole library somewhere it does not fit.

## The allocation ledger

**The emit path allocates nothing, and this is enforced rather than
intended.** `enabled()` → `emit()` → `encodeArgs()` → `commit()` performs no
heap traffic on any branch. What makes it hold is the encoder's refusal:
`std::string` is not `Encodable`, so the conversion that would allocate is a
compile error rather than a review comment (`unit/encode.test.cpp` asserts
the refusal). Argument sizing and packing happen on the stack, and the stack
frame is small and fixed -- one `TypeCode[N]` in the definition writer, scalars
everywhere else. No `<iostream>`, no `printf`, no `std::function`, no RTTI
dependence anywhere on the path.

**Setup allocates, deliberately.** `Logger::create` takes `std::string`
directory and stem, and builds the segment path. That is once per process,
before any producer thread exists.

**One allocation is not ours and cannot be removed here.** The emit path uses
two `thread_local`s (the writer cache and the correlation id). In a
dynamically loaded plugin -- the configuration R4.1 exists for -- these
compile to general-dynamic TLS, and glibc `malloc`s the TLS block on a
thread's first access. So R1.2's letter holds for the store sequence, and
"no allocation, ever, anywhere" does not hold for a plugin's first emit on
a new thread. Stated rather than papered over.

## The fixed costs, which are the interesting number

| what | default | notes |
|---|---:|---|
| segment file | 8 MiB | created and mapped at full size |
| chunk | 64 KiB | one per thread, claimed on first emit |
| chunks per segment | 127 | `(8 MiB - 4 KiB header) / 64 KiB` |
| per call site | ~56 bytes RAM | see below |
| per thread | 64 KiB + cache | the chunk is never returned |

Three consequences worth knowing before deploying this anywhere small:

**A thread costs a whole chunk, permanently.** The first emit on a thread
claims 64 KiB and no chunk is ever reclaimed. With the defaults the 128th
logging thread finds the segment exhausted and every record it writes is
dropped (counted, per R9.1 -- but dropped). A process with many short-lived
threads exhausts a segment far faster than its record volume suggests.

**A call site costs writable memory, not read-only memory.** The whole point
of the site descriptor is that it lives in the binary's constant data
(`record-model.md`), but it carries a `mutable std::atomic` for the announce
generation, which keeps it out of `.rodata`: roughly 56 bytes of RAM plus
relocations per site. On a host that is nothing; on a target counting
kilobytes of RAM it is the thing that scales with how much you log.

**Alternating Loggers on one thread is pathological.** The per-thread writer
cache holds one chunk, keyed by owner and generation. A thread that logs
alternately through two bound Loggers claims a fresh chunk on *every* emit,
because the cache can only hold the other one's. Rare in production, easy to
hit in a test that binds per case. If this matters, the fix is a small
per-thread map rather than a single slot; it has not been made because
nothing yet needs it.

## The reader

Bounded by its input rather than by any constant, and the input is a whole
segment. It no longer buffers, though: `decodeAll` makes two streaming
passes over the mapping (definitions and a count, then messages), which
removed a per-record `RecordView` buffer that had peaked at several times
the segment's own size -- and made decoding about a third faster as a side
effect. `SegmentReader::visit` is the streaming interface underneath, now a
template rather than a `std::function`, so a caller that wants bounded
memory can filter as records arrive instead of materialising all of them.

`Merger::merged()` still deep-copies each record, including its argument
vector. That is one allocation per record and doubles peak. It is on the
cold path and it is the honest place to spend, but it is the next thing to
fix if a merge ever needs to be memory-bounded.

## What a memory-restricted or embedded target would need

None of this is currently in scope -- `REQUIREMENTS.md` R8.1 names Windows,
Linux and macOS, and the library targets those honestly. The gap is written
down because "embedded" is a reasonable thing to want from a logger with no
formatting and no allocation, and because most of the distance is smaller
than it looks.

Already true, and the hard part:

- The producer path allocates nothing and takes no lock.
- The header chain compiles with `-fno-exceptions -fno-rtti` (verified;
  `<random>` and a `try`/`catch` used to prevent that, and were removed --
  which also cut the preprocessed size of `log.hpp` from 71,103 lines to
  58,862).
- 64-bit atomics are now a `static_assert` rather than an assumption. A
  32-bit core without a 64-bit read-modify-write (Cortex-M, older ARM) would
  otherwise have had the standard library silently substitute a lock table,
  making R1.3 quietly false. It now fails the build and says why.

Still missing, in the order that matters:

1. **A backend that is not a file mapping.** `mmap`, a filesystem and a
   pre-sized file are all assumed. `vnext-frontend-backend.md` already
   fixes where that seam belongs -- the chunk source -- and a
   `MemorySegment` writing the same format into a static buffer is the
   first rung. That is the whole of what "runs on an MCU" needs
   structurally.
2. **Geometry defaults an order of magnitude smaller.** 8 MiB and 64 KiB are
   host numbers. The format takes geometry from the segment header, so
   nothing but the defaults has to change.
3. **`std::string` out of the configuration surface.** `Logger::Options`
   and the segment path are the only string users left on the producer
   side; a `string_view`-and-fixed-buffer form would remove `<string>`.
4. **A thread-local-free mode.** Single-threaded targets do not need the
   writer cache or the correlation id in TLS, and some toolchains make TLS
   expensive or unavailable.
5. **A decision about the 64-bit head word.** It is what the `static_assert`
   above guards. A 32-bit variant of the format would widen the audience
   and fork the wire format; that is a real cost and should be an explicit
   choice, not a drift.

Items 1 and 2 are most of the value and neither changes the format.

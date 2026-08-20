# What survives a hard kill

`REQUIREMENTS.md` R3 asks that a committed record survive `TerminateProcess` or
`SIGKILL` without a graceful shutdown path. This file records what was actually
established about that, including a claim that was got wrong first and had to be
withdrawn.

A note on how much of this is quoted and how much is derived. Neither platform
documents "this survives the death of the writing process" in those words. What
they document is where the bytes go and what the named loss conditions are, and
the survival property is a short inference from those. The inference is flagged
each time rather than dressed up as a citation, because the whole point of this
file is that a plausible-sounding durability claim is easy to get wrong.

## The claim that was wrong

An early version of this reasoning justified a memory-mapped transport by saying
that a record in a mapped page survives a hard kill *while a queued one does
not*. The first half holds. The second half was doing more work than it can
bear, and the correction matters because it changes which argument carries the
design.

**A file mapping's dirty pages do survive.** On Windows, write-back of modified
pages in a view over a real file is attributed to the system and is explicitly
allowed to happen after the view has been unmapped; the only loss conditions the
documentation names are power failure and system crash. On POSIX, `MAP_SHARED`
updates are visible to other processes mapping the same file and are carried
through to the underlying file. In neither case is the death of the mapping
process named as a loss condition, and that absence is the inference the
property rests on.

One portability caveat, because it is routinely overstated in the other
direction: `msync(2)` says plainly that without it "there is no guarantee that
changes are written back before `munmap(2)`". So "the kernel gets round to it"
is not a portable guarantee. Linux is what makes people believe it is -- there,
`MS_ASYNC` has been a no-op since 2.6.19 because write-back happens anyway.

**But so does a plain `write()`.** Once bytes are handed to the kernel they are
in the system's file cache, and the calling process dying is not among the
things documented to lose them. `write(2)` is careful to say that a successful
return "does not make any guarantee that the data has been committed to disk",
and what it names as the risk is system failure -- again, not process exit.

So the survival of a hard kill rests on exactly the same inference for an
ordinary write as for a mapping, which is precisely why the two cannot be told
apart on this axis. "Survives a hard kill" does not distinguish a mapped file
from simply writing to one, and any argument built on that distinction is
invalid.

Where the property genuinely disappears is C stdio. A `FILE*`'s buffer is
process-private memory, so anything sitting in it at the moment of the kill is
gone. Durability that rests on remembering to `fflush` after every write is the
kind that decays under maintenance -- it holds on the day it is written and
fails silently the first time someone adds a path that forgets.

## What actually distinguishes the designs

The question is not what the sink writes with. It is whether anything holds a
record in process-private memory between the producer's call returning and the
bytes being in kernel-owned memory. There are three shapes, and only the third
has both properties this library needs.

**Queue plus writer thread.** The producer costs no syscall, but the record sits
in a process-private queue until a background thread drains it. Whatever is in
that queue at kill time is lost, and no sink can change that, because the sink
is downstream of the queue. Pointing a queue-based logger at a memory-mapped
sink does not fix this -- it moves the durable boundary, not the staging.

**Direct `write()` per record.** No staging at all: the producer's call returns
only once the kernel has the bytes. It costs a syscall per record, which is not
affordable on a thread that logs once per operation.

**Store into a mapped page.** No staging and no syscall. The producer's `memcpy`
lands in kernel-owned memory as an ordinary store.

So the property being bought is **no in-process staging, without a syscall per
record**, and a shared file mapping is the only mechanism on that list that
delivers both. That is narrower and more defensible than the claim it replaces,
and it should be read as the honest version rather than the stronger one.

## How much the staging window actually costs

Less than the framing above implies, and saying so is the strongest argument
*against* building anything new.

A bounded, preallocated queue caps the window at the queue's capacity, and a
backend thread that is keeping up leaves it near-empty. For a process that is
killed while idle, the lost tail is the least interesting part of the run.
Anyone arguing that a well-tuned queue is good enough for that case is right.

The window matters in exactly two cases, and they are the two where the records
nearest the end are the entire point.

A **hang or crash under investigation**, where the last thing the process did
before wedging is the thing being looked for. The tail is not the least
interesting part of that run; it is the only interesting part.

The **crash handler itself**. A signal handler or an unhandled-exception filter
runs at a point where a background writer thread may already be dead, so
anything queued for it is unrecoverable -- which is why crash handlers
conventionally write to `stderr` directly rather than through the logger. A
staging queue cannot serve a crash handler, and no amount of configuration
changes that. With nothing between a record and the file, the crash handler can
emit its trace as an ordinary record in the same stream, correlated with the
work that was in flight, and it survives for the same reason everything else
does.

Neither case is reachable by configuring a queue-based library differently. That
is the whole of what the transport choice buys, and it is worth knowing that it
is that narrow.

## The per-platform guarantee

Both platforms reach the same place by the same underlying mechanism, and each
offers an adjacent, almost identically-spelled option that gives the opposite.
That is worth a comment at the mapping call and a test, not a footnote.

**Windows.** A section created by `CreateFileMapping` over a **real file
handle** and mapped `FILE_MAP_WRITE` has its modified pages written to that file
by the system, and the documentation is explicit that this can happen after the
view has been unmapped.

The wording people cite for this -- that an explicit flush is what protects
integrity across a crash -- sits on the pages covering unmapping a view and
closing a file mapping object. It is **not** on the `FlushViewOfFile` page,
which does not mention a crash at all. An earlier version of this file
attributed it there, which is worth recording because the misattribution is
common and it is the kind of citation nobody re-checks.

Two further details. `FlushViewOfFile` on its own is not physical durability: it
flushes no file metadata and does not wait for the disk's hardware cache, so
`FlushFileBuffers` has to follow it. And a section created with
`INVALID_HANDLE_VALUE` is backed by the paging file rather than by a file, so it
has none of this. The implementation must never take that path, and a test
should assert that the segment has a real backing file -- see the through-line
in `platform-tracers.md` for why that is a test rather than a comment.

**Linux and macOS.** `mmap` with `MAP_SHARED` over a real file makes the updates
visible to every other mapper and carries them through to the file.
`MAP_PRIVATE` is copy-on-write and private to the process, so it loses
everything -- the same trap in a different spelling.

## What a hard kill destroys

What is documented: a terminated process gets no chance to run additional code,
and `DLL_PROCESS_DETACH` notifications are not delivered to its libraries.
`SIGKILL` cannot be caught, blocked or ignored.

What follows but is not separately documented: destructors do not run and
`atexit` handlers do not run. That is an inference from "no chance to run
additional code" rather than a quotation, though not a controversial one.

One thing to state carefully, because the loose version of it is misleading.
"Buffered data is lost" is true only of **user-mode** buffers -- a `FILE*`'s
buffer, a logger's queue, an application's own accumulation. Data already handed
to the kernel by `WriteFile` or `write(2)` is not affected by the process
dying. Conflating the two is how the wrong durability conclusion gets reached in
the first place.

This is why R3.2 forbids making durability depend on a flush-on-exit path. Such
a path is correct in every test that exercises it and absent in every case that
matters, because none of it runs.

## What is not attempted

Durability against machine power loss. That needs `FlushViewOfFile` plus
`FlushFileBuffers`, or `msync`, per record -- a syscall per log call, which
gives back the property the design exists to obtain. The failure model being
defended against is a killed process. Power loss is a different problem and this
library does not claim to solve it.

Also knowingly lost: at most one partially-written record per thread, the one
being copied at the instant of the kill. Detecting that is a correctness
requirement rather than a nicety, and `framing-and-recovery.md` covers the
mechanism.

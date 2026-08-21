# Several processes, one stream

Requirements R5.1 through R5.5 say the library must produce one coherent stream
across a group of cooperating processes. This file records why the mechanism is
one segment per process merged at read time, rather than any of the three
arrangements that look more direct.

## The choice

Each process writes its own segment file into a shared directory. Nothing is
shared for writing. A decoder opens whatever segments it finds and merges them
by timestamp when someone actually reads.

That is the whole mechanism, and it is worth defending precisely because it
looks like the lazy answer -- it defers work rather than doing it, and the
alternatives all sound more engineered.

## Why not send records to a central process

A named pipe or Unix domain socket to a collector is the obvious design, and it
fails on availability. The collector has to be running. It is not running before
it starts, which is exactly the window where startup diagnostics matter most, and
it is not running at all for a standalone tool that does its work and exits.

It also adds a failure mode to the one subsystem whose entire job is to report
failure modes. What should a producer do when the pipe is full and the collector
is wedged? Block, and a diagnostic call can now stall the application. Drop, and
the records lost are the ones written while something was already going wrong.
Neither answer is acceptable, and the question does not arise if there is no
pipe.

## Why not share one mapped file between processes

Sharing a single mapping means sharing a write cursor, which means cross-process
synchronisation on the hot path, races on file growth, and platform-specific
sharing-mode behaviour to get right on every target. That is a substantial
amount of difficult, concurrency-sensitive machinery.

What it buys is skipping a merge step. The merge is an N-way ordered scan of
data nobody is reading yet.

There is a second, worse problem. A shared writable structure means one
process's abrupt death can leave that structure inconsistent for everyone else:
a claimed-but-unfilled region, a cursor advanced past bytes that were never
written. Requirement R3.1 says a hard kill must not cost committed records --
with a shared cursor, one process's kill threatens *other* processes' records.
Per-process segments contain the blast radius by construction. The worst a dying
process can do is truncate its own segment, which is exactly the case R3.3
already requires a reader to tolerate.

## Why not inherit a handle to the parent's segment

Passing an inherited handle down to spawned children is genuinely neat, and it
works for the children a process spawns itself. It does nothing for a peer
process started independently by a user, and nothing for a third-party binary,
which will not write records in this format no matter what handle it is given.
R5.5 covers that case by capturing the child's output streams instead.

A mechanism that covers some of the cases still leaves the general one to be
solved, and then there are two mechanisms to maintain instead of one.

## What makes merging at read sound

Two things, and the first is easy to get wrong.

**A comparable clock.** Merging by timestamp requires timestamps that mean the
same thing in every process. R5.3 exists because the C++ standard leaves
`steady_clock`'s epoch unspecified, so readings from two processes are not
portably comparable even on one machine. The anchor record in each segment
header -- a monotonic reading paired with a wall-clock reading -- is what lets a
merger align segments that started at different times, without depending on an
implementation detail that happens to hold today.

**Ordering is a decoder concern.** Because the merge happens at read time, the
ordering policy is not baked into anything a producer does. Interleaving by
timestamp, grouping by correlation id, or presenting one process at a time are
all decisions a reader can revisit later against data already written. A
write-time merge fixes that policy at the moment of capture and pays for it on
every record, forever, whether or not anybody ever reads the result.

That cost asymmetry is the argument in one line: merging at read is paid once by
whoever reads, and merging at write is paid always by everyone who logs.

# Platform tracers, and buffers that do not belong to the process

The C++ logging libraries surveyed in `prior-art-cpp-loggers.md` all stage
records in process-private memory before something else moves them. The platform
tracers are usually described as not doing that, and that description is what
makes them worth studying even though none of them is adopted here.

It is also less absolute than it sounds. For every tracer below, the property
turns out to depend on a configuration choice rather than being intrinsic, and
the conditional version is the one worth carrying forward. Several statements
here are consequences of documented mechanics rather than documented statements,
and those are flagged.

## ETW

For a standard session the story holds. Buffers are non-paged, kernel-managed,
allocated per processor per session, and flushed by the session's own logger
thread. A producer that is terminated therefore does not take already-written
events with it. That is a consequence of the mechanics rather than a quotation:
no Microsoft page states it in those words, so it should be presented as
something derived, not something cited.

The caveat is the part that usually gets skipped, and it undoes the clean claim.
A private, in-process session -- `EVENT_TRACE_PRIVATE_LOGGER_MODE` -- is
documented plainly: the memory for buffers comes from the process's memory. That
restores the exposure in full. Nor is it an obscure corner, because Microsoft
recommends in-process sessions where they are workable.

So buffer ownership is a property of the session type, not of ETW. Anyone
repeating "ETW events survive the producer" without saying which session type
they mean is repeating something that is true half the time.

Starting a standard session requires administrator rights or membership of
Performance Log Users, and the kernel logger is administrator-only.

## LTTng-UST

Recording requires a session daemon and a channel configuration. Worth being
precise about what that costs, because the usual phrasing overstates it:
`lttng create` starts the daemon if it is not already running, so this is not
manual daemon management.

What remains is still real. There is a service that must exist, a channel that
must be configured, and a session that must have been created before the
interesting thing happened. "Somebody noticed something odd, what do the logs
say" is answered with "nothing, you were not recording".

## eBPF and USDT

The producer-side cost when nothing is attached is often described as zero. It
is nearly zero, and the gap is instructive: the probe site itself becomes a
`nop`, but the instructions that marshal the probe's arguments still execute on
every pass. That is exactly why the semaphore mechanism exists -- a tracer sets
it so that the caller can skip the marshalling too, rather than only the trap.

Scope the platform claim correctly. USDT probes are not Linux-specific; they
originated with DTrace. It is the eBPF attach path that is Linux-only, and it
needs a BPF program and, normally, privilege.

As a way to add deep, occasional, opt-in instrumentation on one platform this is
attractive and nothing here forecloses it. As the transport for a library's
ordinary diagnostics on three platforms it is not a candidate.

## os_log and os_signpost

Storage is system-side rather than in the emitting process, which is the
property being looked for. Three caveats matter more than that headline.

The name of the system daemon that owns the store is not asserted here. It did
not appear in the developer documentation consulted, and a name that cannot be
sourced is not worth printing.

Whether data survives an abnormal termination is not documented. Apple describes
retrieval after an app has already exited; nothing addresses being killed.
"Survives a `SIGKILL`" should be treated as unestablished rather than as true.

Retrievability is level-dependent, which is the caveat with real consequences.
Debug messages are held in memory and never persisted. Info persists only when
collected with the `log` tool. So "os_log survives the crash" is true for some
levels and false for others, and which levels is the first question anyone
relying on it has to answer.

## Why none of them is the transport

Not the property -- everything attached to it.

Each is a different API on a different platform, so a cross-platform requirement
means three implementations and a lowest common denominator across them, which
is the opposite of what R7.1 asks for.

Each needs something arranged before the interesting thing happens: a session of
the right type, with the right privilege, at the right log level. That is
hostile to the ordinary diagnostic case, and equally hostile to unattended runs
that must produce the same artifacts on three platforms without administrative
setup on any of them.

The reading tools are built for interactive analysis rather than for a program
that has to extract one field from one run, which is a real cost when the log's
main consumer is another program.

## The tracing SDKs

**Perfetto's C++ SDK** buffers in process memory and flushes to a service, so a
killed process loses the buffer -- the same exposure as a queue-based logger,
with a service to deploy on top. The part of Perfetto worth keeping costs
nothing: emitting Chrome Trace Event JSON gets a capable viewer with no
dependency at all, because `ui.perfetto.dev` opens that format directly. A
decoder should emit it for exactly that reason.

**OpenTelemetry C++** is an SDK plus a collector, which is the wrong shape for a
library that must work with nothing running. The requirement that would change
this is a need to correlate with other systems inside somebody else's
observability stack, and the right response to that is an exporter in the
decoder -- offline, on a consumer's thread, costing the producer nothing --
rather than an SDK in the producer.

**Tracy** remains a capable interactive profiler and nothing here prevents
attaching it for a session. It is not a candidate for the record of what
happened, because its record lives in a connected UI.

## The through-line

For every tracer surveyed, the property this library wants turns out to be
conditional. ETW's buffer ownership depends on the session type. `os_log`'s
persistence depends on the message level. LTTng records nothing unless a session
was created first. None of them is intrinsically "the buffer outlives the
process"; each is "the buffer outlives the process, provided somebody configured
it that way and that configuration is still in force".

That generalises past the survey. R3.1 is worth writing down as a requirement
precisely because this is the kind of property that quietly becomes conditional:
a mapping over a real file has it, and the same code over an anonymous mapping
does not. `hard-kill.md` therefore asks for a test that the segment has a real
backing file rather than a comment saying it should.

The other conclusion is the one that shapes the transport. What makes the
tracers interesting is not the durability but the pair of properties underneath
it: the buffer is not process-private, and the producer reaches it without a
syscall per event. That pair is not exclusive to them. A file mapping's dirty
pages are equally not owned by the process, and a store into one is equally not
a syscall, so a mapped segment reaches the same place with no session daemon, no
elevation, no privilege, and no platform-specific reader.

# Prior art: the low-latency C++ loggers

The preference should be to take a library, and since building is the expensive
answer it needs the stronger argument. This file is that argument, written so
that "why not just use X" has an answer nobody has to re-derive.

Two of the claims here were wrong in an earlier version of this reasoning and
are corrected in place rather than quietly replaced. A wrong reason for a right
answer is worse than no reason at all, because it survives review and then
collapses the first time somebody checks it.

Where a statement below is our inference rather than something a project
documents, it says so. Facts were checked against each project's own sources at
the time of writing, and the licence and status table at the end is the part
most likely to have gone stale by the time you read this.

## The shared insight, and who had it first

`quill`, `NanoLog`, `fmtlog` and `Binlog` all rest on the idea this library is
built on, and they got there first: do not format on the producer. Capture the
argument values into a buffer, keep the format string as static metadata, and
let something else turn bytes into text later.

`quill` puts a per-thread single-producer queue in front of one backend thread
and formats there. `fmtlog` uses the same per-thread queue and copies arguments
raw, but has **no background thread by default** -- the consumer runs when the
application calls `fmtlog::poll()`, and `startPollingThread()` is opt-in with a
default interval of one second. That is a materially different operational
shape from `quill`'s, and treating the two as interchangeable is a mistake.

`NanoLog` compacts log statements into a binary format that a separate
`decompressor` tool expands. `Binlog` writes a binary stream in which the schema
for each event travels in the file, so its reader needs nothing from the
producing binary. "Self-describing" is our word for that property; the project
does not use it.

Those debts should be stated plainly rather than left implicit. R1.1 is the move
`quill` and `fmtlog` made. The site-descriptor split in `record-model.md` is
`NanoLog`'s. The stream that carries its own schema, and therefore R4.3, is
`Binlog`'s.

## Two unrelated projects called NanoLog

The survey has to keep these apart, because conclusions drawn about one are
false about the other and the name collision is easy to miss.

`PlatformLab/NanoLog` is the one meant when "NanoLog" appears in the low-latency
literature. Its C++17 version needs no preprocessor step; a separate
Preprocessor version exists and is documented as being for advanced users, which
is the detail most often got wrong about it. Output is compacted and expanded
offline by a `decompressor` tool.

Its headline latency figure is a **median**, and quoting it without saying so
overstates the case: the project's own table reports single-digit nanoseconds at
the median and roughly 37 to 40 ns at the 99.9th percentile. The tail is the
number that matters for a producer on a latency-sensitive thread.

- Stephen Yang, Seo Jin Park and John K. Ousterhout, "NanoLog: A Nanosecond
  Scale Logging System", USENIX ATC '18, pp. 335-350.
  https://dblp.org/rec/conf/usenix/YangPO18

`Iyengar111/NanoLog` is a different codebase by a different author with the same
name: a straightforward asynchronous logger that buffers arguments into a line
object -- the README describes it as staying under about 256 bytes before
spilling to the heap -- and formats on a background thread. It is a reasonable
small library and it is not the system the performance literature is describing.

Anyone comparing against "NanoLog" should say which one.

## Maintenance status is a property of the dependency

Two of the interesting projects are archived, and that alone removes them as
candidates. Adopting an archived library means forking it and owning it, which
is the cost of building without the benefit of having designed the thing for the
problem in front of you.

A third, `PlatformLab/NanoLog`, is not archived but has not had a commit on
master since December 2020. Its repository metadata shows a much more recent
push date, which is misleading; the commit date is the one to read.

Borrowing an archived project's *format idea* costs nothing, and that is what
this library does with `Binlog`. Taking `Binlog` as a dependency would be
strictly worse than writing the encoder.

`Binlog` is worth reading before it is set aside, because it ships something
directly relevant that most loggers do not: alongside its `bread` reader it has
`brecovery`, which recovers log content from a core dump. That is the same
problem R3 is about, approached from the other end -- if the records are still
in the process image, a tool can go and get them.

## Why none of the maintained ones is the dependency

Four reasons, given in order of how much weight they carry. That order has been
revised twice as claims were checked: an early version led with hard-kill
durability, a later one with structure, and both were overstated. What is left
is narrower and is stated here in its checked form.

### Typed argument values do not reach the consumer

`quill`, `fmtlog` and `spdlog` render arguments into text. `NanoLog` and
`Binlog` produce a binary encoding of what will become text.

`quill` goes further than the others, and the correction below says how much
further. But the values it hands a sink are strings: an `int` arrives as `"42"`,
so numeric and range filtering means re-parsing text for every record, and a
join on a 128-bit identifier compares thirty-two characters instead of sixteen
bytes. R2.1 asks for the value, not for its rendering.

`Binlog`'s events are typed structures and come closest of anything surveyed,
but its model is a user-defined struct per event type rather than a uniform
record with a correlation field -- and it is archived.

### Structure is a per-call-site opt-in, and nothing enforces it

`quill` populates its structured key-value list only when the format string
contains named placeholders: `MacroMetadata::has_named_args()` is driven by a
scan of the format string, and a positional `{}` never sets it. A site written
with positional placeholders produces an empty list and is a text line with no
structure at all.

Two consequences follow, and the second is worse than the first. An existing
codebase yields no structure until its call sites have been rewritten to name
their arguments -- which is a human judgement per site about what each value
should be called, not a scripted substitution. And afterwards it stays a hazard:
a site written positionally is silently outside the structured surface, with no
compile error and nothing to notice it.

That is a silent-degradation mode, and it is the shape requirement 8 exists to
forbid: a mechanism that stops working as intended without saying so.

Under a design where the encoder captures types, structure is a property of the
mechanism rather than of what the author remembered to type.

One thing not to claim: what `quill` does with a format string that mixes named
and positional placeholders was not established, so no argument here rests on
it. A related trap for anyone reading its output -- the `_1` and `_2` suffixes
that appear on some keys are collision handling against reserved field names,
not index keys for positional arguments.

### A frontend queue is an in-process staging buffer

This is the durability argument, and `hard-kill.md` restates it correctly: the
naive form was wrong, because an ordinary `write()` survives a hard kill for the
same reason a mapping does, and the corrected form is narrow -- it matters for a
crash handler and for the post-mortem of a hung process, and little else.

Real, but the lightest of the four.

### Dependency weight, particularly at a plugin boundary

These libraries are built around `fmt`-style formatting: `fmtlog` takes `fmt` as
a submodule and `quill` carries a bundled copy. R7.3 and R4.2 turn that from a
preference into a constraint, because a dependency-light plugin cannot
reasonably be asked to link a formatting library and a thread-local frontend in
order to emit a record. A two-pointer C channel is a few dozen lines; the
equivalent for any of these libraries is "link the whole thing into the plugin".

`quill`'s frontend is thread-local state, so static-linking it into two modules
reproduces exactly the duplicated-singleton failure R4.1 forbids, and avoiding
that means shipping it as a shared library into plugins whose whole point is to
carry as little as possible.

The exact dependency shape varies by version and is worth re-checking before
anyone reopens this. The direction does not change: none of them is a
header-only producer that a dependency-light plugin can adopt without pulling a
formatting library and a thread-local frontend in behind it.

## Correction one: quill's producer is not inherently allocating

Its default queue is `UnboundedBlocking`, and that one does allocate on the
producer thread: when the queue fills it allocates a new node and doubles
capacity, up to a documented ceiling. Selecting `BoundedDropping` or
`BoundedBlocking` gives a queue the documentation states never reallocates, and
`Frontend::preallocate()` forces the thread-local context and its queue to be
built up front rather than on first use.

The precise claim, and no more than it: **a bounded queue plus `preallocate()`
removes the queue's own producer-side allocations**, and gives a bounded,
countable drop policy -- which is exactly the policy shape R8.1 asks for,
arrived at independently. It says nothing about allocation inside a user's own
argument types, and no source claims a blanket allocation-free hot path.

Any comparison that assumes `quill` allocates is therefore comparing against a
default rather than against the library. The default is a real trap worth
knowing about, and it is a configuration trap, not a design limit.

## Correction two: quill's sinks do receive structured fields

An earlier version of this survey claimed that `quill` formats on its backend
thread, so that by the time any sink is invoked the typed argument values have
become characters in a rendered string -- leaving a memory-mapped sink under
`quill` producing a text log that is unfilterable by subsystem, unjoinable by
correlation id, and assertable in tests only by substring match.

All three claims fail against the actual sink interface. A custom sink overrides
`write_log`, a protected twelve-parameter virtual on `Sink` -- the backend
worker is a friend -- and among those parameters are call-site metadata, a
logger name, a severity, a timestamp, thread identity, and

```cpp
std::vector<std::pair<std::string, std::string>> const* named_args
```

Subsystem filtering is therefore available, because the logger name and the
call-site metadata arrive as their own values. Joining by correlation id is
available, because an id passed as a named argument arrives as its own pair. And
a test can assert on that list rather than on a substring of a sentence.

A sink built on `quill` could write a structured, queryable, memory-mapped
record stream. This library's design is not the only route to one, and any
argument that says otherwise is wrong.

What survives that correction is the narrower set listed above, plus two costs
that are worth naming because they compound. Building that vector allocates per
record -- one vector and two strings per named argument -- on the backend
thread, which does not violate the producer constraint but does drain the queue
more slowly, which lengthens exactly the staging window that the durability
argument identified. And key-value text is several times the size of a compact
binary record, so "every surface is a view of one stream" weakens to "every
surface is a view of one text stream": still true, still useful, less compact.

## The ones named only to dismiss

**`spdlog` formats the user's arguments on the calling thread**, and the chain
is worth spelling out because the library's asynchronous mode invites the
opposite assumption. `logger::log_()` calls `fmt::vformat_to` before anything is
handed off; `log_msg_buffer` then deep-copies a payload that has already been
formatted; and the thread pool does pattern rendering and sink work only. So
async mode defers I/O and pattern rendering, not argument formatting -- which is
the substantive difference between it and `fmtlog` or `Binlog`, and the reason
adopting it would trade one format-on-the-hot-path design for another while
adding a dependency.

**`Boost.Log`** is a large dependency, and virtual dispatch through its sink
interface is inherent: `sinks::sink` declares `will_consume`, `consume` and
`flush` as pure virtual. Locking is not inherent, and this document previously
said it was -- it is a property of the frontend chosen. `unlocked_sink` takes no
lock, `synchronous_sink` serialises with a mutex, and `asynchronous_sink` hands
off to a thread. The size remains the reason not to take it here.

**`glog`** is worth naming because it is the answer people reach for by default,
not because it is a candidate: it is archived, which settles the question before
its mechanism needs discussing.

## The honest bottom line

Adopting `quill` with a bounded, preallocated queue and a custom memory-mapped
sink is a serious answer and a close call rather than a dismissal. It is
maintained, its producer-side queue allocation can be removed by configuration,
and its sinks do receive structured fields.

What decides against it is narrow and should be stated as such: typed values
rather than stringified ones (R2.1), structure without a per-call-site opt-in
and without a silent decay mode (R2.2), and no in-process staging for the
crash-handler and hung-process cases (R3.1). Anyone who weighs the first and
third lightly should reach the opposite answer, and this document should be
revised to say so rather than defended.

The saving from adopting is also smaller than it first looks, because most of
what this library specifies is unaffected by the choice: the segment layout, the
framing and generation rules, the multi-process merge, the decoder, and the
plugin channel are all still to be written either way. What `quill` would supply
is the frontend queue, thread registration, the filter check and the
deferred-formatting machinery.

## What would reverse the decision

Two upstream changes. Either narrows the gap; both together close it, and at
that point adopting is the right answer and this design should be abandoned in
its favour.

**Typed values rather than stringified ones.** The structured argument list
exposed as a variant, or as the encoded argument buffer plus the call site's
type descriptor, rather than as pairs of strings. A backend that formats holds
exactly that immediately before it formats, so exposing it is a plausible
upstream request rather than a redesign.

**Structure without a per-call-site opt-in.** Named arguments populated from
ordinary positional placeholders, so that adopting the library does not require
rewriting existing call sites first, and so that a positional placeholder stops
silently leaving a record out of the structured surface.

Both are worth raising upstream before writing an encoder rather than assumed
impossible. The worst outcome available here is spending a month on a record
model that a maintained library would have supplied had anyone asked.

## Licence and status

Recorded because both change, and because a licence that does not suit a
consumer removes a library from consideration regardless of its merits. Read
this as a snapshot to re-check, not as a standing fact.

| Project | Licence | Status | Note |
| --- | --- | --- | --- |
| `odygrd/quill` | MIT | Active; v12.1.0, 2026-07-15 | The licence file carries no SPDX identifier; the headers say "Distributed under the MIT License" |
| `PlatformLab/NanoLog` | ISC | Not archived; last commit on master 2020-12-22 | The licence file reads "ISC License", but GitHub reports `NOASSERTION` because its classifier does not recognise it |
| `Iyengar111/NanoLog` | MIT | Last commit 2017-03-08 | There is no licence file at all -- the notice appears only in `NanoLog.hpp`, and not in the README -- so GitHub reports none |
| `morganstanley/binlog` | Apache-2.0 | Archived 1 July 2026 | Ships `bread` and `brecovery` |
| `MengRao/fmtlog` | MIT | Not archived; last push 2025-02-13 | Hard `fmt` dependency via submodule |
| `gabime/spdlog` | MIT | Active | GitHub classifies it as `NOASSERTION` because the licence file appends a note about the bundled `fmt` licence |
| `google/glog` | BSD-3-Clause | Archived 30 June 2025 | |
| Boost.Log | BSL-1.0 | Ships with Boost | Per boost.org and the source headers; the repository API reports no licence |

Two cautions for anyone re-checking this. Both archive dates come from the
repository banner, because the API payload omits the archive timestamp in both
cases. And for three of the eight, querying the repository API alone gives
`NOASSERTION` or no licence at all -- the licence has to be read out of the
file, or in one case out of a header, before an identifier can be written down.
An audit that trusts the API will mis-report all three.

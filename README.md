# Sub0Log

[![CI](https://github.com/CraigHutchinson/Sub0Log/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/CraigHutchinson/Sub0Log/actions/workflows/ci.yml)
[![tests](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/CraigHutchinson/Sub0Log/badges/tests.json)](https://github.com/CraigHutchinson/Sub0Log/actions/workflows/ci.yml)
[![coverage](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/CraigHutchinson/Sub0Log/badges/coverage.json)](https://github.com/CraigHutchinson/Sub0Log/actions/workflows/ci.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](REQUIREMENTS.md)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE.md)

The tests and coverage badges come from the `coverage` CI job
(`.github/workflows/ci.yml`), published to this repository's `badges` branch
on every push to `main`. They read "unknown" until that job has run once.

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

## Getting it into a build

Header-only, zero third-party dependencies on the producer path
(`REQUIREMENTS.md` R8.3). Two supported ways in, both covered by
`tests/packaging`:

**CPM**, if you don't already have an install step:

```cmake
include(CPM.cmake) # or fetch it first -- https://github.com/cpm-cmake/CPM.cmake

CPMAddPackage(
    NAME Sub0Log
    GITHUB_REPOSITORY CraigHutchinson/Sub0Log
    GIT_TAG main # no tagged release yet -- pin to a commit once you've picked one
    OPTIONS "SUB0LOG_BUILD_TESTING OFF") # skip Sub0Log's own test deps; sub0log-cat still builds

target_link_libraries(myapp PRIVATE Sub0Log::Sub0Log)
```

`tests/packaging/consumer_cpm` and `tests/packaging/drive_cpm.cmake` build
this exact block against `CPMAddPackage(GIT_REPOSITORY ...)`, as the
`packaging::cpm_fetch` ctest.

**`find_package`**, if your build already has an install prefix -- a distro
package, a vendored SDK, a superbuild, Conan or vcpkg downstream:

```cmake
find_package(Sub0Log REQUIRED)
target_link_libraries(myapp PRIVATE Sub0Log::Sub0Log)
```

`packaging::find_package` builds this against a real `cmake --install` into
a throwaway prefix.

Either way, C++23 and (on MSVC) the conformant preprocessor
(`/Zc:preprocessor`, needed for the call-site macros' use of `__VA_OPT__`)
arrive as usage requirements on `Sub0Log::Sub0Log` -- nothing extra to set in
your own `CMakeLists.txt`.

## How it works

A call site expands to writing raw argument bytes into a chunk of a
file-backed mapping, claimed with one atomic read-modify-write. The format
string, file, line, subsystem, severity and argument types are not part of
that write -- they live in a descriptor emitted once per call site the first
time it runs, and the record itself carries only a reference to it. Nothing
on that path allocates, formats, or takes a lock, so a record is sitting in
kernel-owned memory before the call returns -- which is what lets a
`SIGKILL`, a `TerminateProcess`, or a segfault leave every record already
committed intact for the next reader to open.

An argument is one of two kinds, and nothing else compiles. A fixed-size
value -- `bool`, `char`, an integer or float up to 8 bytes, an enum (via its
underlying type), a pointer -- is copied by value. Anything that views bytes
it doesn't own -- `std::string_view`, `std::span<const std::byte>`,
`const char*`, or any type with a non-throwing conversion to
`std::string_view` (`std::string` included) -- is copied by view, no
allocation. A `std::vector`, a `std::filesystem::path`, a
`std::chrono::duration`, or any other custom type does not compile: each
needs a representation decision -- an id, `.count()`, `.native()` -- that
only the call site can make, so the compile error asks for it there instead
of guessing inside the library. There's no formatter customisation point to
hook a type into; that refusal is deliberate (`docs/record-model.md`), not
a gap.

A segment is one memory mapping, sized once. `Logger::create` creates a
file, sizes it up front (`segmentBytes_`, 8 MiB by default) and maps the
whole thing -- no growth, no remap, no wraparound afterward. That's part of
what makes the hard-kill guarantee above unconditional: nothing about
staying alive involves resizing or relocating the mapping while it's being
written to. A *thread*, not a call site, claims a chunk (64 KiB by default)
out of that fixed space with one atomic -- once when it first logs, again
whenever the one it's holding fills up -- and every record from every call
site that thread executes lands in whichever chunk it currently holds. So
it's write volume and thread count that exhaust a segment, not the number
of call sites in your code: once the fixed chunk supply runs out, later
records are dropped and counted rather than blocked or grown into
("Operating it" below covers what that means for sizing a real service).

A separate `Decoder`/`Merger` turns that back into text (or a value you can
assert on), on whatever thread and in whatever process wants it. Several
processes each write their own segment and are merged at read time into one
ordered stream, aligned by a machine-wide monotonic clock rather than
coordination between the producers.

`REQUIREMENTS.md` is the fuller, argued version of this, written as what
each requirement rules out rather than what it promises.

## Naming it your way

The call sites are macros, not functions, for two specific reasons
(`STYLE_GUIDE.md`, "Macros"): a disabled site must not evaluate its
arguments (R1.4) -- a function call always evaluates its arguments before
the call happens, a macro-guarded `if` does not -- and each call site needs
its own `static` descriptor, which a macro expanding at that exact source
location guarantees and a shared function template does not.

Nothing stops you naming them what you like. It's ordinary preprocessor
substitution, so `__FILE__`/`__LINE__` resolve to your real call site
through as many macro layers as you add -- this is a consumer's own header,
not something Sub0Log ships:

```cpp
#define logDebug(...) sub0log_debug(__VA_ARGS__)
#define logInfo(...)  sub0log_info(__VA_ARGS__)
#define logWarn(...)  sub0log_warning(__VA_ARGS__)
#define logError(...) sub0log_error(__VA_ARGS__)

logDebug(Storage, "read {} at {} for {} bytes", blobId, offset, length);
```

## Known limitations

Stated here rather than left to be discovered:

- **Pre-1.0.** Version `0.1.0`, no tagged release yet. The public API can
  still move between releases; `wire::cFormatVersion` (the on-disk format,
  separate from the library version) is meant to change far more rarely, but
  neither is frozen.
- **A segment does not wrap** ("How it works" above has why). A real
  constraint to plan around, not a bug -- "Operating it" below covers
  sizing one for a real service.
- **One host.** No network transport, no collector daemon, no query
  language, and no aggregation across machines -- all explicitly out of
  scope (`REQUIREMENTS.md`, "Explicitly out of scope"). What this produces
  is a stream and a decoder; a query layer or a shipping pipeline is the
  caller's business.
- **C++23 is a hard requirement**, not a preference (R8.2) -- there is no
  fallback path for an older standard. In practice this means a recent GCC
  or Clang, and MSVC with `/Zc:preprocessor` (carried automatically for you,
  per above).
- **Header-only has a real compile cost.** See "Including it is not free"
  below for measured numbers -- it is not free, and it is worth knowing
  before the include goes somewhere widely included.
- **The examples ladder has two acknowledged gaps** (a truncated-segment
  reader, and `Merger::totals()`) -- listed, not hidden, in
  `examples/README.md`.

## Background

`docs/` carries the groundwork rather than the plan: what a hard kill
actually destroys, the platform tracers and the question of whose buffer a
record lands in, and the framing and recovery problems as the established
binary formats already solved them. Start at `docs/README.md`. Two of those
files record a claim that was wrong first and say why -- that is deliberate.

If the question is "why not an existing library", the direct answer is
`docs/prior-art-cpp-loggers.md`: what `quill`, `Binlog`, `fmtlog`, `spdlog`,
`glog`, `Boost.Log` and both projects named `NanoLog` each actually do --
their queue and allocation behaviour, plugin-boundary story, licence and
maintenance status -- and the two upstream changes that would have made
adopting one of them the better answer instead of building this. It's
where the comparisons live; this file doesn't make them.

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

**Size for the rate you expect.** A segment does not wrap ("How it works"
above has the mechanism and why); rotation policy is where logging
libraries go to acquire configuration surfaces, and `REQUIREMENTS.md` puts
it out of scope on purpose. The intended pattern is a new segment per run,
or per interval, with `Merger` putting them back into one ordered stream at
read time. Set `segmentBytes_` for the rate you expect, and alert on the
drop counter rather than on the file.

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
discovered: `docs/adoption-friction.md` registers the bugs, the gaps and the
friction that is deliberate, each with what was done about it. Everything in
it is resolved. The last to close was a plugin built with hidden visibility
getting its own instance pointer and logging into nowhere; it took two
steps, a counter that made the silence audible and then the C ABI (R4) that
removed the cause.

## License

MIT. See `LICENSE.md`.

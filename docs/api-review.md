# API review: the surface as a surface

`adoption-friction.md` writes the code a consumer would write and records
what happens. This does the same thing turned sideways: not "does the first
afternoon work" but "does every name, return type, and lifetime note agree
with every other one" -- because v2 just finished, nothing has shipped, and
this is the last point an API mistake is cheap.

The verdicts are the same three `adoption-friction.md` uses, for the same
reason (a register that only complains is less useful than one that also
settles questions):

- **Bug** -- the library does the wrong thing.
- **Gap** -- nothing is wrong, but a consumer has to notice something we
  could have made unmissable, cheaply.
- **Deliberate** -- friction on purpose, kept because the alternative costs
  a property the library exists to provide, and stated here because
  undocumented deliberate friction is indistinguishable from an oversight.

Each entry shows the declaration or the probe behind it. Ranked by what it
costs a consumer to hit, not by what it costs us to fix. **Applied** entries
are done, in this tree, and re-verified against all three suites (see
"What was checked" at the end). **Proposed** entries are this document's
opinion, not a change -- signature and rename changes are explicitly out of
my hands here.

## 1. `ChildProcess::wait()` could return an exit status nobody can get back

**Bug (missing `[[nodiscard]]`), fixed.**

```cpp
struct ExitStatus { std::int32_t exitCode_{}; std::int32_t signal_{}; };
ExitStatus wait() noexcept;   // <- as declared, before this review
```

`exitStatus_` is a private member with no accessor. `wait()`'s return value
is the *only* way to read it, and `wait()` was not `[[nodiscard]]`. The
first-day shape --

```cpp
auto child = sub0log::ChildProcess::spawn(options);
child.wait();               // compiles, blocks, discards the exit code
```

-- compiles cleanly and silently throws away the one thing most callers
spawn a child to find out. It is technically recoverable (`wait()` is
idempotent: a *second* call returns the cached `ExitStatus` rather than
re-waiting), but nothing advertises that, and reading "block until done,
then block until done again" off two `child.wait();` lines is not how
anyone would arrive at it.

This is exactly the "counted-but-unnoticed" shape the rest of the library
goes out of its way to avoid -- except here there is no counter at all
standing in for the lost value, just silence.

**Applied:** `[[nodiscard]]` added to the declaration and the out-of-line
definition; the one internal caller that intentionally discards it (the
destructor, which only needs the draining side effect) now says `(void)
wait();` with a comment explaining why. The doc comment states the
idempotence and the "capture it the first time" consequence explicitly.
Four call sites in `tests/system/child.test.cpp` that block-and-discard for
reasons unrelated to the exit code (interceptor suppression, correlation
propagation, truncation) now spell that discard as `(void)child.wait();`
rather than a bare statement -- edited to keep the suite warning-clean
after the finding was fixed, not to change what they test.

## 2. `sub0log_fatal` sounds like it terminates the process. It does not.

**Gap, fixed.**

```cpp
#define sub0log_fatal(subsystem, formatText, ...) \
    SUB0LOG_EMIT(::sub0log::Severity::Fatal, (subsystem), formatText __VA_OPT__(, ) __VA_ARGS__)
```

Every one of the other severities means what it says (`Info` informs,
`Warning` warns). `Fatal` does not: it is a record with the highest
severity value, full stop -- no abort, no throw, no `std::terminate`. A
consumer arriving from glog (`LOG(FATAL)` aborts by contract), spdlog
(`SPDLOG_CRITICAL` is commonly paired with an abort in application code but
at least says so), or almost any C logging convention (`syslog`'s `LOG_EMERG`
included) has every reason to expect `sub0log_fatal(...)` ends the process,
and nothing at the macro itself said otherwise. The one place the true
contract was spelled out was `log.hpp`'s file-level doc comment, three
screens away from the macro a consumer actually calls, and phrased as "an
example logs Fatal and then lets the signal kill the process" rather than
as a statement about what the macro itself does.

The gap is not a missing feature (a library that aborted for you would be
the wrong design -- unwinding, cleanup, and whether to abort at all are the
caller's call) -- it is the missing sentence at the point where a consumer,
reading top to bottom, would otherwise assume the opposite.

**Applied:** a doc comment directly above `sub0log_fatal`'s definition
states the non-effect plainly and points at
`examples/04_crash_handler.cpp` for the pattern that does terminate.

## 3. `Decoder` in a `std::vector` reproduces a use-after-free the library already found once

**Gap, fixed (documentation).**

`merge.hpp` explains, in a private implementation comment on
`Merger::segments_`, exactly how AddressSanitizer caught a Decoder-holding
container silently switching from moving elements to *copying* them on
reallocation:

> a Decoder was [nothrow-move-constructible] until it gained a
> `std::deque` member -- whose move constructor libstdc++ does not mark
> `noexcept`. That one addition silently flipped this vector from moving
> ... to copying ... AddressSanitizer caught it as a use-after-free in the
> two-process merge test.

That is real, hard-won knowledge -- and it lives entirely inside `Merger`'s
private section. `Decoder` itself, in `reader.hpp`, says a great deal about
lifetime (the "Lifetime, and the mistake it invites" paragraph is genuinely
excellent, see §10 below) but never mentions that *the Decoder object
itself* is unsafe to keep in a relocating container once a `DecodedRecord`
has been read out of it. `REQUIREMENTS.md` leaves "whether the decoder
ships as a library, a CLI, or both" open, and `Merger` is only one answer;
a consumer who decodes several segments by hand -- `std::vector<Decoder>`,
one per segment, the completely natural first reach for "several of these"
-- reproduces the exact bug the library's own authors already hit and
fixed for themselves, with none of the breadcrumbs that would tell them
why.

**Applied:** a paragraph added to `Decoder`'s class comment in `reader.hpp`
naming the hazard, the mechanism (deque's non-`noexcept` move poisoning
the whole class), and the fix (a non-relocating container), citing
`Merger::segments_` as the worked example.

## 4. `SegmentReader::open()`'s borrow was documented one level up, not at the source

**Gap, fixed (documentation).**

`Merger::addSegment()` says plainly: "The image must outlive the Merger
(views point into it)." `SegmentReader::open(std::span<const std::byte>
image)` -- the thing that actually stores the span and hands out every
`RecordView::payload_` as a view into it -- said nothing at all about
`image`'s lifetime, at the class comment or at the declaration. A consumer
who reaches for `SegmentReader` directly (it is a public, documented type,
not a `Merger` implementation detail) meets the borrow with no warning
until they free the buffer under it.

**Applied:** the `open()` declaration now states the borrow and points
downstream to `Decoder`'s lifetime comment for what follows from it.

## 5. Two `detail::`-namespaced types are load-bearing parts of the public surface

**Bug (naming/namespace-boundary), proposed only -- a type change.**

```cpp
struct Options {
    ...
    detail::SegmentOptions segment_{};   // instance.hpp
};
[[nodiscard]] detail::PlatformError error() const noexcept;  // Logger, ChildProcess
```

`detail::` is used consistently everywhere else in this codebase to mean
"implementation, not the surface" -- `detail::ChunkWriter`,
`detail::Segment`, `detail::emit`, all private members or arguments to
private functions. Two exceptions cross that boundary directly:

- `Logger::Options::segment_` is `detail::SegmentOptions`, and it is not a
  corner case -- `adoption-friction.md` 3.2 explicitly tells an operator
  they need to choose `segmentBytes_`, and every example and several tests
  configure it directly (`options.segment_.segmentBytes_ = ...`,
  `examples/07_live_tail.cpp`, `examples/09_operating.cpp`,
  `tests/integration/producer.test.cpp`, and others).
- `Logger::error()` and `ChildProcess::error()` both return
  `detail::PlatformError`, the same pattern `Segment::error()` (a private
  implementation type) uses internally.

The inconsistency is visible next to a type that got this right:
`SegmentReader::error()` returns `sub0log::SegmentError` -- no `detail::`,
defined in the public namespace, because it *is* public. `PlatformError`
and `SegmentOptions` are exactly as public in practice (a consumer
configures one and inspects the other on every error path a `Logger` or
`ChildProcess` has) but are typed as if they were not, which is confusing
in an IDE tooltip or a `decltype` and actively wrong as a signal to a
future maintainer deciding whether these types are safe to change.

**Not applied** -- moving either type out of `detail::` is a type rename
and changes every consumer's spelling of `Logger::Options` and every
`catch`/inspection of `error()`'s return type; that is exactly the class of
change this review defers to a decision, not a fix.

## 6. Three counters, three names, three shapes

**Gap (naming), proposed only -- a rename.**

| Type | Where | Fields |
|---|---|---|
| `Stats` | `instance.hpp` (`Logger::stats()`) | `droppedRecords_`, `truncatedRecords_` |
| `CaptureStats` | `child.hpp` (`ChildProcess::stats()`) | `capturedLines_`, `suppressedLines_`, `unloggedLines_`, `truncatedLines_` |
| `Totals` | `merge.hpp` (`Merger::totals()`) | `unreadableBytes_`, `unwrittenBytes_`, `undecodableRecords_` |

All three are the same shape of thing -- a snapshot struct returned by a
method named `stats()`/`totals()` reporting counters kept for observability
(R9.1/R9.2/R9.3) -- and none of the three names is wrong on its own. Read
side by side, though, a consumer has to hold three different words for one
concept, and the method names do not even agree with their own return
types (`Merger::totals()` returns `Totals`, consistent; `Logger::stats()`
and `ChildProcess::stats()` return `Stats` and `CaptureStats`
respectively, inconsistent with each other). Alongside them,
`sub0log::unboundEmits()` and `sub0log::abi::siteTableExhausted()` are
free functions returning a bare `std::uint64_t` rather than a struct at
all -- which is the right shape for a single counter (see §11), but widens
the vocabulary further: a consumer learns "check `.stats()`", "check
`.totals()`", and "call this free function" as three unrelated idioms for
what is, every time, R9's answer to "was anything lost".

**Not applied.** A consistent name (`Stats` everywhere, with
`ChildProcess::Stats` and `Merger::Stats` replacing `CaptureStats` and
`Totals`) is a rename across three headers and every consumer that spells
the type today; left for a decision rather than done here.

## 7. `atLeast(value, threshold)` -- two `Severity`s, swappable, used directly by consumers

**Gap, fixed (documentation).**

```cpp
[[nodiscard]] constexpr bool atLeast(const Severity value, const Severity threshold) noexcept;
```

Not an internal helper: `tools/sub0log_cat.cpp` and
`examples/02_subsystems.cpp` both call `sub0log::atLeast(...)` directly to
build their own filters, which is exactly the kind of code a consumer
writing a custom sink or dashboard would write on their first day. Both
parameters are the same type, nothing at the call site names which is
which, and `atLeast(a, b)` and `atLeast(b, a)` both compile and disagree
whenever `a != b` -- textbook adjacent-same-type-swap risk, and the
function had no doc comment at all to anchor the reader against it.

**Applied:** a doc comment states the order and the mnemonic ("is `value`
severe enough for `threshold`").

## 8. `Logger::segmentPath()` hands back a reference with no stated lifetime

**Gap, fixed (documentation).**

```cpp
[[nodiscard]] const std::string& segmentPath() const noexcept { return segment_.path(); }
```

No comment at all, before this review, on what the returned reference is
tied to. It is a reference into the `Logger`'s own member, so it is valid
exactly as long as the `Logger` is (and, per the class's own "final
address" contract, invalid the moment a bound `Logger` moves) -- ordinary
enough, but every other borrow in this library gets a sentence at the
point a consumer meets it (`Options::subsystemNames_`, `Merger::merged()`,
now `SegmentReader::open()` too), and this one did not.

**Applied:** a one-line doc comment ties the reference to the `Logger`'s
own lifetime and cross-references the class comment's move contract.

## 9. Same-typed adjacent fields in the two `Options` structs invite a silent positional swap

**Gap, fixed (documentation) -- not a call-site bug today.**

```cpp
struct Options {          // instance.hpp
    std::string directory_{"."};
    std::string stem_{"sub0log"};
    ...
};
struct ChildOptions {     // child.hpp
    ...
    bool captureStdout_{true};
    bool captureStderr_{true};
    ...
};
```

Both are aggregates; both have same-typed fields sitting next to each
other (`directory_`/`stem_`, both `std::string`; `captureStdout_`/
`captureStderr_`, both `bool`). Every actual call site in this codebase
already uses designated initializers (`Options{.directory_ = ...}`), which
sidesteps the hazard entirely -- but the language does not require it, and
nothing in either struct's declaration says a consumer should. A
positional `ChildOptions{{"cmd"}, "", false, true}` compiles and silently
captures stderr instead of stdout.

**Applied:** a one-line comment above each struct recommends
name-qualified initialization and names the specific adjacent pair to
watch. No functional change, and no existing call site needed touching --
every one already does this correctly.

## 10. What is documented exactly right, and should not change

Three places this review looked hard at, expecting a finding, and did not
get one:

- **`Decoder`'s "Lifetime, and the mistake it invites" paragraph**
  (`reader.hpp`) is the standard the rest of the borrowing documentation in
  this library should be held to: it names what is a view, what it is a
  view *into*, what re-reading a growing segment does wrong, and says so
  as "this cost an example author a segfault" rather than as an abstract
  warning. §3 and §4 above extend its reach; nothing in it needed
  correcting.
- **`Logger` and `ChildProcess` delete the identical set of special
  members**, for the identical reason, stated in each place: copy deleted,
  move-assignment deleted, move-construction kept and documented as safe
  only before the object is bound/spawned into its final address. Two
  independently-designed RAII owners in this codebase arrived at the same
  shape, which is a sign the shape is right rather than a coincidence
  worth flattening into one base class.
- **`SUB0LOG_ABI_HOST_EXPORT()` is UPPER_SNAKE, and that is correct even
  though `STYLE_GUIDE.md`'s own wording ("a name a library consumer writes
  in ordinary code reads as a call") could be read as demanding lowercase.**
  It is written once, by a consumer, at namespace scope -- but it expands
  to a function *definition*, the same shape `main()` is, enforced by the
  linker rejecting a second one anywhere in the program. That is a
  different rhetorical category from `sub0log_debug(...)`, which reads
  (and is meant to read) as an ordinary call made many times per function.
  The style guide's own test -- "is the reader's question here *is this
  the preprocessor*" -- says yes for a macro that silently becomes a
  global `extern "C"` symbol, so UPPER_SNAKE is the rule working correctly
  at its edge, not an exception to it.

## 11. Considered and judged correct: `SegmentReader::visit()`'s return is not `[[nodiscard]]`

**Deliberate, and right.**

```cpp
template <typename OnRecord>
std::uint64_t visit(OnRecord&& onRecord);   // damage count; not [[nodiscard]]
```

This looked, at first pass, like the same bug as `ChildProcess::wait()`
(§1): a `[[nodiscard]]`-shaped return with real call sites that discard it,
including two inside `Decoder::decodeAll()` itself and one in
`examples/06_child_capture.cpp`. The difference that settles it: `wait()`'s
`ExitStatus` has no other accessor, so discarding the return loses the
value outright (barring the undocumented idempotent-recall trick). `visit()`'s
return is a convenience duplicate of `unreadableBytes()`, which the class
doc says explicitly is "also available afterwards" -- nothing is lost by
ignoring the return, which is exactly what `decodeAll()`'s own two internal
passes do. Marking it `[[nodiscard]]` would have forced a `(void)` at every
one of those legitimate call sites for no safety gained. Left as it is.

## What was checked

Full surface read: `log.hpp`, `instance.hpp`, `severity.hpp`, `context.hpp`,
`reader.hpp`, `merge.hpp`, `child.hpp`, `abi_host.hpp`, `sub0log_abi.h`,
`version.hpp`, `wire.hpp`'s consumer-facing constants, `site.hpp`,
`segment.hpp`, `chunk.hpp` and `encode.hpp`'s public concepts, plus
`tools/sub0log_cat.cpp` and the root `CMakeLists.txt`. Every declaration in
every public class was checked for `[[nodiscard]]`, `explicit`, deleted/kept
copy and move members, and a stated lifetime wherever a `string_view` or
reference crosses the boundary. `encode.hpp`'s `StringViewLike` refusal
diagnostics and `tools/sub0log_cat.cpp`'s option handling were read in full
and found consistent with `record-model.md` and `adoption-friction.md` 1.2
-- no new finding there, which is itself worth recording: the highest-volume
friction item in the whole project was closed correctly.

One item outside the header surface, noted briefly rather than written up:
`write_basic_package_version_file(... COMPATIBILITY SameMajorVersion ...)`
with `PROJECT_VERSION_MAJOR` at `0` means every `0.x` release is currently
treated as compatible with every other `0.x` by `find_package(Sub0Log)`,
which is looser than pre-1.0 semver norms expect. Not written up as a
finding of its own because it stops applying the moment this library
reaches `1.0`, which `adoption-friction.md`'s own framing says is close.

**Changes applied** (all additive; no signature, rename, or behaviour
change; `wire.hpp` and `sub0log_abi.h` untouched):

- `include/sub0log/child.hpp` -- `[[nodiscard]]` on `ChildProcess::wait()`
  (declaration and definition); destructor's internal call now
  `(void)wait()`; doc comments on `wait()` and on `ChildOptions`.
- `include/sub0log/instance.hpp` -- doc comments on `Logger::segmentPath()`
  and `Logger::Options`.
- `include/sub0log/log.hpp` -- doc comment on `sub0log_fatal`.
- `include/sub0log/reader.hpp` -- doc additions to `Decoder`'s class
  comment, `SegmentReader::open()`, `DecodedSite`, and `DecodedRecord`.
- `include/sub0log/severity.hpp` -- doc comment on `atLeast()`.
- `tests/system/child.test.cpp` -- four `child.wait();` statements changed
  to `(void)child.wait();` to keep the suite warning-clean after the
  `[[nodiscard]]` change above; no assertion changed.

Verified after every edit: `cmake --build build -j8 && ctest --test-dir
build` (91/91), `cmake --build build-gcc -j8 && ctest --test-dir build-gcc`
(91/91), and `./build-stress/benchmarks/Sub0LogStress --quick` (all seven
invariants held) after the changes touching `log.hpp`, `severity.hpp`, and
`child.hpp`, since those sit on or beside the producer path.

**Proposed, not applied** (renames or type changes -- your call):

1. Move `PlatformError` and `SegmentOptions` out of `detail::` into the
   public `sub0log` namespace (§5).
2. Rename `CaptureStats` and `Totals` to a shared name with `Stats`, or
   otherwise unify the three counter-snapshot vocabularies (§6).

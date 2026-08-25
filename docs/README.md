# Reference material

The groundwork this library was built on: what the existing low-latency loggers
do, what the platform tracers offer and cost, what a hard kill actually
destroys, and which established binary formats already solved the framing and
recovery problems.

`REQUIREMENTS.md` at the repository root says what the library must do. These
files say what was learned about how other people did it and what the
constraints really are, so that the requirements can be argued with on evidence
rather than on assertion. Where a requirement is the direct output of something
learned here, the file cites it by number rather than restating it.

Two questions come up more than any other, so they get direct answers:

**"Why not just use library X?"** -- `prior-art-cpp-loggers.md`.

**"What survives a kill?"** -- `hard-kill.md`.

## The files

**`prior-art-cpp-loggers.md`** surveys `quill`, both unrelated projects called
`NanoLog`, `Binlog`, `fmtlog`, `spdlog`, `glog` and `Boost.Log`: what each one's
mechanism is, how its queue and allocation behave, what it does at a plugin
boundary, and what its licence and maintenance status are. It ends with the two
upstream changes that would make adopting a library the better answer.

**`platform-tracers.md`** covers ETW, LTTng-UST, eBPF/USDT and Apple's `os_log`
and `os_signpost`, analysed around one question: does the buffer belong to the
process, or to something that outlives it? None is adopted, and the answer turns
out to be conditional in every case -- ETW's buffer ownership depends on the
session type, `os_log`'s persistence on the message level -- which is a more
useful result than the yes/no table it replaced.

**`hard-kill.md`** records what `TerminateProcess` and `SIGKILL` actually
destroy, including the correction that an ordinary unbuffered write survives
them for the same reason a file mapping does. That correction narrows the
argument for the design considerably, and it is stated in its narrowed form.

**`framing-and-recovery.md`** takes the framing and truncation problem to the
formats that already solved it: RFC 7464's leading record separator, TFRecord's
separately-checksummed length against LevelDB's deliberately unchecksummed one,
Avro's per-file random sync marker, LevelDB's block alignment, Kafka's
version-before-checksum ordering, and RocksDB's generation-stamped recycled logs
and its enumeration of recovery policies.

**`multi-process.md`** explains why a group of processes writes one segment
each and merges at read time, rather than sending records to a collector,
sharing one mapped file, or inheriting a handle from a parent. The
availability, blast-radius and cost-asymmetry arguments are all there, along
with the clock problem that makes a timestamp merge sound in the first place.

**`memory.md`** is the allocation ledger: what the emit path really does
(nothing, and why that is enforced rather than intended), what it reserves
instead, the fixed costs per thread and per call site, and an honest gap
analysis for memory-restricted targets -- which are not in scope, but are
closer than they look.

**`vnext-frontend-backend.md`** records a design consideration for the next
version -- a front-end/backend split -- and, more importantly, the constraint
that rules out its obvious shape: a record queue under the word "backend" is
the staging arrangement `hard-kill.md` rejects. The honest seam is the chunk
source, where backend choice costs nothing per record.

**`record-model.md`** covers keeping the constant half of a call site out of the
record, why a site definition must be written by the producer rather than
resolved by the reader, where formatting happens, and the four mechanisms for
variable-length payloads.

## On sourcing

Claims carry a citation where one exists, and say so explicitly where the
reasoning is ours rather than something a project documents. Several widely
repeated statements in this area turn out to be inferences that no primary
source makes -- reasonable ones, mostly, but a reader deserves to know which is
which before quoting one onward.

## On the corrections

Two of these files record a claim that an earlier version of the reasoning made
and that turned out to be false: that a queued record is lost on a hard kill in
a way a written one is not, and that no existing library gets structured fields
to a sink. Both are kept, with what was claimed and why it was wrong, rather
than tidied away.

They are the most useful content here. A conclusion on its own invites a reader
to re-derive the wrong answer and act on it; a conclusion with its own
correction attached does not.

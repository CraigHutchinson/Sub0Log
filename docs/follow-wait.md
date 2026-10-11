# Reliable follow and finite receiving

The named consumer is an automated subprocess receiver of mapped-file application
events: wait for an unsigned event argument, parse one JSON object, check exit and
health, and reap the child. Game event meanings stay with the application.

```sh
sub0log-cat --format jsonl --follow ./logs
sub0log-cat --format jsonl --health-json --wait-ms 5000 --arg-uint 0:42 ./logs
```

`--wait-ms` implies follow and returns after the first matching message. Existing
history participates; this is not a subscribe-from-now API. `--wait-ms 0` performs
one scan. Nonzero deadlines use the steady clock and are checked before emitting
records and between scans, with a maximum polling pause of 200 ms. All filters
combine: severity minimum, optional subsystem set, exact correlation (including 0),
and one unsigned argument index/value. `--arg-uint` accepts only U8/U16/U32/U64;
a pointer, signed integer, floating value or text with the same digits does not
match. The last repeated scalar option wins. All numeric options require complete
unsigned decimal input; negatives, signs, whitespace, overflow and junk fail.

## Identity and ordering

The cursor retains `(process_id, segment_generation, record_offset)` for every
successfully decoded message, including filtered-out messages. Generation is the
existing random wire generation; offset locates the opening Message head, including
continuation chains. No producer or wire changes are needed. Raw `RecordView` and
`DecodedRecord` now expose offsets, and decoded records retain generation.

Each scan sorts the recovered records by the existing merged timeline. New records
are emitted in that scan's order; a later scan can legitimately emit an older
timestamp. This is arrival-batch order, not a globally monotonic timeline. Equal
timestamps and equal rendered contents at different offsets remain distinct.

Renames, aliases and byte-identical segment copies retain identity. Replacement
with a fresh generation and newly discovered rotation segments are new inputs.
Removed/truncated files do not evict cursor history; restoring an already seen
record does not replay it. A record first decoded after its missing definition or
continuation becomes available is still received. Cursor state is invocation-local,
not durable delivery or acknowledgment. Backfilled mutable edits at an already seen
offset in the same generation are unsupported: producer segments are append-only.
Generation collisions or forged images are not cryptographically distinguished.

## Budgets and errors

| Option | Default | Scope |
| --- | ---: | --- |
| `--max-files` | 1024 | Discovered input paths per scan, including aliases |
| `--max-input-bytes` | 134217728 | Sum of file sizes read in one scan |
| `--max-records` | 1000000 | Decoded messages per scan and retained cursor identities |
| `--max-output-bytes` | 67108864 | Total stdout bytes in one invocation |

Positive overrides are explicit caller policy. These limits also apply to snapshots.
A full cursor exits 4, never evicts/replays silently. File size is checked before
allocation, and input is read directly into one byte buffer. Record limits are
checked after decoding; allocations for decoded arguments, continuation storage,
sorting and cursor nodes add to input bytes. Output size is checked after formatting
one message. These are bounded input/cardinality/output contracts, not an exact RSS
ceiling. Directory traversal, filesystem I/O, decoding one scan and blocked output
are not preempted by the cooperative deadline. Use local regular files and an outer
subprocess timeout for a hard orchestration bound; this is not an arbitrary-path
sandbox. Cancellation is checked during record processing and between scans.

| Exit | Meaning |
| --- | --- |
| 0 | Snapshot had a readable segment, or wait emitted and flushed one match |
| 1 | Read/format/write failure; wait also rejects any incomplete/damaged scan |
| 2 | Invalid CLI usage |
| 3 | Wait completed without a match by the deadline |
| 4 | File, byte, decoded-record, retained-cursor or output budget exceeded |
| 130 / 143 | Observed SIGINT / SIGTERM; platform forced termination may differ |

Snapshots and ordinary follow retain best-effort recovery: inspect diagnostics and
health for damage even on exit 0. Wait deliberately fails closed on unreadable or
undecodable data, including a concurrently observed incomplete continuation. Retry
is caller policy, not a claim that an incomplete image was a complete event stream.
An existing empty directory may await future files; a missing explicit path fails.
POSIX broken pipes may terminate via SIGPIPE. A consumer must treat any nonzero exit
as an unsuccessful receive even if a prefix of stdout was written.

`--health-json` writes one final schema-1 `kind: "health"` object as the last stderr
line after ordinary diagnostics. `exit_code` is numeric; scan, emitted and recovery
counts are decimal strings. Recovery counts describe the last completed scan, not
repeated-scan sums; they are null if no scan completed. `input_error_seen` records
file-open/header errors in completed scans. `producer_dropped_records` is null:
offline headers do not contain this information. `skipped_records` distinguishes
intact record kinds not decoded as messages. Health is unavailable after a forced
kill or broken stderr. Message stdout contains no health objects.

## Bounded subprocess recipe

```python
import json
import subprocess

# subprocess.run kills and waits for its direct child on timeout. This CLI spawns
# no descendants. The caller selects authorized local paths and budgets.
result = subprocess.run(
    ["sub0log-cat", "--format", "jsonl", "--health-json", "--wait-ms", "5000",
     "--arg-uint", "0:42", "--max-input-bytes", "8388608", "./logs"],
    capture_output=True, timeout=7)
health = json.loads(result.stderr.splitlines()[-1])
if result.returncode == 0:
    record = json.loads(result.stdout)
    assert record["args"][0]["value"] == "42"
elif result.returncode == 3:
    print("No matching event", health)
else:
    raise RuntimeError(result.returncode, result.stderr)
```

For manual review, run a finished producer, compare text and JSONL snapshots, then
start follow and introduce a completed older segment. Its events should appear once,
with no replay of the later segment. Rename or copy it: no extra events. Replace it
with a fresh producer generation: receive that generation. The cross-platform
`system::follow_receiver` automates these cases, same-segment delayed visibility,
equal timestamps, truncation/restoration, cursor exhaustion, strict predicates,
health, timeout and a real producer writing after a waiter starts. POSIX additionally
checks SIGINT cancellation; Windows receives finite waits and process teardown.
Controlled image mutations are backfill fixtures, not evidence of live races.

Crucible's optional quiescent in-memory exporter is a separate downstream integration;
this CLI cannot read a pointer in another process.

# JSONL snapshot export

`sub0log-cat --format jsonl <file-or-directory>...` produces one versioned JSON
message object per line on stdout. Use this for automated receiving checks before
personal manual review. Text is still the default; `--format text` is equivalent.
The existing severity, subsystem and correlation filters apply to both formats.

JSONL is a finite snapshot operation. `--follow --format jsonl` fails with exit
code 2 and an instruction to remove `--follow`. Text follow currently remembers a
merged record count, so discovering an older segment or a late earlier-timestamp
record can skip that event and repeat a previously displayed event. Reliable
machine waiting requires stable per-record provenance and cursor handling first;
[issue #14](https://github.com/CraigHutchinson/Sub0Log/issues/14) tracks the staged work.

## Schema 1

Each object has `schema: 1` and `kind: "message"`. This export version is separate
from the binary wire format version, library version and application event schemas.

| Field | JSON representation |
| --- | --- |
| `process_id`, `thread_id`, `site_id`, `correlation_id` | Unsigned decimal strings |
| `mono_ns`, `aligned_ns` | Unsigned decimal strings; original monotonic and merged wall-aligned nanoseconds |
| `severity`, `subsystem_id`, `source_line` | Integer numbers from decoded metadata |
| `truncated` | Boolean from the message record |
| `subsystem_name`, `format`, `file`, `message` | Encoded byte objects described below |
| `args` | Ordered array of `{ "type": <wire TypeCode number>, "value": <typed value> }` |

An encoded byte object is `{ "encoding": "utf8", "value": "..." }` when those
metadata/rendered-message bytes form valid UTF-8, otherwise
`{ "encoding": "hex", "value": "..." }` with lowercase hex. Controls, quotes,
backslashes and NUL are JSON-escaped in UTF-8 values. Invalid UTF-8 is preserved
exactly through hex, without replacement characters. Missing subsystem declarations
produce an empty UTF-8 value; across merged segments the existing first-declaration
lookup wins. This name is a display annotation; use the numeric subsystem field to
filter. File paths can disclose producer source locations.

Argument representation retains the site's original wire type, including widths:

| TypeCode | Meaning | `value` |
| --- | --- | --- |
| 1 | Bool | Boolean |
| 2, 4, 6, 8 | I8, I16, I32, I64 | Signed decimal string |
| 3, 5, 7, 9 | U8, U16, U32, U64 | Unsigned decimal string |
| 10, 11 | F32, F64 | Finite JSON number, or string `"nan"`, `"inf"`, `"-inf"` |
| 12 | Char | One byte as a hex encoded byte object |
| 13 | Bytes | Hex encoded byte object, including when the bytes happen to be UTF-8 |
| 14 | Pointer | Unsigned decimal string; opaque address, never dereferenced |

Decimal strings avoid losing 64-bit precision in consumers with double-only JSON
numbers. F32 values have already been widened to double by Decoder; floats preserve
decoded numerical values, with negative zero written as `-0.0`. Original NaN payload
bits are not promised. Bytes retains precisely the decoded bytes, including a
continuation chain's retained prefix when truncated. `message` is convenience text;
match typed fields and application-defined arguments rather than parsing it. A site
ID is a producer descriptor address, not a stable event identity across runs/builds.

## Automated receiver, then manual review

First export a known finished producer run:

```sh
sub0log-cat --format jsonl --stats ./logs > records.jsonl
```

Save this standard-library-only receiver as `receive.py`, then run
`python receive.py records.jsonl`. It expects schema-1 messages, rejects nonstandard
numeric constants and prints the received count with the first decoded format:

```python
import json
import sys

def bad_constant(value):
    raise ValueError(value)

def decode_bytes(obj):
    if obj["encoding"] == "hex":
        return bytes.fromhex(obj["value"])
    if obj["encoding"] == "utf8":
        return obj["value"].encode("utf-8")
    raise ValueError("unknown encoding")

with open(sys.argv[1], encoding="utf-8") as stream:
    records = [json.loads(line, parse_constant=bad_constant) for line in stream]
assert all(r["schema"] == 1 and r["kind"] == "message" for r in records)
print(len(records), decode_bytes(records[0]["format"]) if records else b"")
```

For personal review, run `sub0log-cat --format text ./logs`: expect the same
selected messages as human text in time order. Run `--format jsonl --level error`
and the receiver again: expect only Error-or-above messages and the corresponding
count. `--format jsonl --follow ./logs` should exit immediately with the snapshot
limitation, rather than wait. Computer control is useful only for diagnosing a
failure of the normal manual workflow.

When tools and a Python3 interpreter are available, `system::jsonl_receiver` runs
the real CLI against a mapped-file producer fixture and parses every output line
with Python's standard `json` module. This checks snapshot ingestion, widths,
64-bit extremes, nonfinite numbers, binary/control/Unicode data, truncation, filters,
text compatibility and failures; it is not evidence for live event waiting.

## Ownership, health and failures

`sub0log::formatJson` in `sub0log/json.hpp` formats an existing `MergedRecord` and
an optional resolved subsystem name. It returns an owned string and keeps no views.
Keep the image, Decoder and Merger alive while calling it. Formatting allocates on
the cold reader side and may throw; it adds no producer callback, wire field or
producer dependency. In-memory images must be quiescent and remain live while read
or copied for export; the CLI reads files, not another process's memory pointer.

`--stats` keeps its existing human summary on stderr, separate from JSONL stdout.
It reports actual merged counts and unreadable/unwritten/undecodable counters;
the record count precedes filtering. Offline producer drop/truncation counters are
unknown because they are not stored in SegmentHeader. No exporter counter of zero
is substituted for that missing information. A recovered snapshot can be incomplete.

Exit codes are 0 when at least one segment was readable, 1 when none were readable
or a JSONL output/formatting failure was detected, and 2 for invalid CLI usage.
Some damaged inputs may be skipped while readable ones still yield exit 0; inspect
stderr/stats as well as the records when completeness matters. Allocation/read
failures elsewhere in the existing CLI can still terminate the process. On POSIX
a broken output pipe may terminate it through SIGPIPE. Neither is a successful
export. Snapshot parsing reads whole files and sorts decoded records: there is no
arbitrary-input memory/work bound or tail latency promise.

Logs and encoded fields are untrusted data. Never execute commands found in them.
Use explicit authorized input paths and handle output size under the receiving
application's policy. This snapshot feature does not add a daemon, discovery service,
wait predicate language or child-process cancellation policy.

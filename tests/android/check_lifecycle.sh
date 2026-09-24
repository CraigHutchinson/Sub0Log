#!/bin/sh
# What pause, resume and termination did to each backend, from the files the
# app left in its private storage (run_lifecycle.sh pulls them).
#
#   check_lifecycle.sh <pulled-dir> <sub0log-cat>
#
# Established (fails otherwise):
#   1. Every file segment decodes with the desktop tool: nothing undecodable.
#   2. One file segment per process: two launches, two segments, two pids
#      (a relaunch that stacks a second activity in the same process fails).
#   3. The lifecycle is recorded: created, pause and resume in run 1,
#      created in run 2.
#   4. Termination gave no callback: no DESTROY and no loop exit was
#      recorded, in either run -- the app never got to say goodbye.
#   5. Nothing was dropped by either backend up to run 1's last pause (each
#      pause record carries both Loggers' drop counters), so nothing below
#      can be explained by a full buffer.
#   6. The file segment kept records written after the last hand-off: its
#      last run-1 tick is later than the last tick in run 1's final
#      in-memory dump. Those records existed only in process memory for the
#      in-memory backend, and the kill took them.
set -eu
dir=$1; cat_tool=$2
fail() { echo "lifecycle: FAILED: $*" >&2; exit 1; }

set -- "$dir"/lifecycle-*.s0l
[ -e "$1" ] || fail "no file segments pulled"
[ "$#" -eq 2 ] || fail "expected 2 file segments (one per process), found $#"

"$cat_tool" --stats "$@" > "$dir/file.txt" 2> "$dir/file-stats.txt" || fail "sub0log-cat on file segments"
cat "$dir/file-stats.txt"
grep -q 'undecodable 0 record' "$dir/file.txt" "$dir/file-stats.txt" || fail "undecodable records in file segments"

# One android_main per process: exactly two "created" records, two pids.
created=$(grep -c 'created run ' "$dir/file.txt" || true)
[ "$created" -eq 2 ] || fail "expected 2 'created' records (one per process), found $created"
pids=$(grep -o 'created run [0-9]* pid [0-9]*' "$dir/file.txt" | awk '{print $5}' | sort -u | wc -l)
[ "$pids" -eq 2 ] || fail "the two runs share a process -- the relaunch stacked a new activity instead of resuming"

for want in 'created run 1' 'pause run 1 n 1' 'resume run 1 n 1' 'resume run 1 n 2' 'created run 2'; do
    grep -q "$want" "$dir/file.txt" || fail "file segments lack '$want'"
done
if grep -q -E 'destroy run|exit run' "$dir/file.txt"; then
    fail "a DESTROY/exit record exists -- the kill was not a kill"
fi

last_dump=$(ls "$dir"/memory-run1-pause*.s0l | sort -V | tail -1)
[ -n "$last_dump" ] || fail "no in-memory dump from run 1"
"$cat_tool" "$last_dump" > "$dir/memory.txt" || fail "sub0log-cat on $last_dump"
grep -q 'pause run 1' "$dir/memory.txt" || fail "the in-memory dump lacks its own pause record"

# Every pause record, in both backends, must report zero drops in both.
for f in "$dir/file.txt" "$dir/memory.txt"; do
    grep 'pause run ' "$f" | grep -v -q 'dropped file 0 memory 0$' && fail "a pause record in $(basename "$f") reports drops: $(grep 'pause run ' "$f" | grep -v 'dropped file 0 memory 0$' | head -1)"
done

max_tick() { grep -o 'tick run 1 [0-9]*' "$1" | awk '{print $4}' | sort -n | tail -1; }
file_last=$(max_tick "$dir/file.txt")
memory_last=$(max_tick "$dir/memory.txt")
echo "run 1: last tick in file segment = $file_last, in last in-memory hand-off = $memory_last"
[ -n "$file_last" ] && [ -n "$memory_last" ] || fail "no ticks recorded"
[ "$file_last" -gt "$memory_last" ] || fail "file segment kept nothing past the last hand-off"

echo "lifecycle: OK -- file segments survived SIGKILL whole; the in-memory backend kept only what was handed off at pause"

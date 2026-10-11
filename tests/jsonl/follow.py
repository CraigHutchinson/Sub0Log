"""Cross-platform receiving of real mapped producers plus controlled image backfill."""
import contextlib
import json
import os
import pathlib
import queue
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time

TOOL, FIXTURE = sys.argv[1:]


def run(*args):
    return subprocess.run([TOOL, *map(str, args)], capture_output=True, timeout=10)


def snapshot(path):
    result = run('--format', 'jsonl', path)
    assert result.returncode == 0, result.stderr
    return [json.loads(line) for line in result.stdout.splitlines()]


def identity(record):
    return tuple(record[key] for key in ('process_id', 'segment_generation', 'record_offset'))


def produce(directory):
    directory.mkdir()
    subprocess.run([FIXTURE, str(directory), 'static'], check=True, timeout=10)
    return next(directory.glob('*.s0l'))


@contextlib.contextmanager
def following(path, *options):
    process = subprocess.Popen([TOOL, '--format', 'jsonl', '--follow', *options, str(path)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    lines = queue.Queue()
    def drain():
        for line in process.stdout:
            lines.put(json.loads(line))
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        yield process, lines
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        reader.join(timeout=5)
        assert not reader.is_alive()
        process.stdout.close()
        process.stderr.close()


def take(lines, count):
    return [lines.get(timeout=5) for _ in range(count)]


def quiet(lines):
    try:
        extra = lines.get(timeout=0.45)
    except queue.Empty:
        return
    raise AssertionError(f'duplicate/unexpected event: {extra}')


def main(root):
    early = produce(root / 'early')
    late = produce(root / 'late')
    watched = root / 'watched'
    watched.mkdir()
    active = watched / 'active.s0l'
    shutil.copyfile(late, active)
    early_records, late_records = snapshot(early), snapshot(late)
    assert len({identity(r) for r in early_records}) == 3
    assert all(int(r['record_offset']) > 0 for r in early_records)
    # Actual regression: a completed older segment arrives after newer events.
    with following(watched) as (_, lines):
        assert {identity(r) for r in take(lines, 3)} == {identity(r) for r in late_records}
        shutil.copyfile(early, watched / 'older.s0l')
        assert {identity(r) for r in take(lines, 3)} == {identity(r) for r in early_records}
        quiet(lines)
        # Aliases, renames, disappearance/reappearance retain wire identity.
        (watched / 'older.s0l').rename(watched / 'renamed.s0l')
        shutil.copyfile(early, watched / 'alias.s0l')
        quiet(lines)
        active.unlink()
        quiet(lines)
        shutil.copyfile(late, active)
        quiet(lines)
        # Same path, new generation is a new segment (rotation/replacement).
        replacement = produce(root / 'replacement')
        replacement_records = snapshot(replacement)
        shutil.copyfile(replacement, active)
        assert {identity(r) for r in take(lines, 3)} == {identity(r) for r in replacement_records}
        quiet(lines)

    # Same generation: later visible records in an earlier chunk, identical times.
    image = bytearray(early.read_bytes())
    for record in early_records:
        struct.pack_into('<Q', image, int(record['record_offset']) + 16, 100)
    backfill = root / 'backfill.s0l'
    complete = bytes(image)
    first = min(int(r['record_offset']) for r in early_records)
    image[first:first + 8] = bytes(8)  # hides the main thread's chunk tail
    backfill.write_bytes(image)
    visible = snapshot(backfill)
    assert len(visible) == 1  # other thread/chunk remains visible
    with following(backfill) as (_, lines):
        assert identity(take(lines, 1)[0]) == identity(visible[0])
        with backfill.open('r+b') as stream:
            stream.seek(first)
            stream.write(complete[first:first + 8])
        recovered = take(lines, 2)
        assert {identity(r) for r in recovered}.isdisjoint({identity(visible[0])})
        assert len({r['mono_ns'] for r in recovered + visible}) == 1
        quiet(lines)
        # Physical truncation/restoration must not replay earlier identities.
        backfill.write_bytes(complete[:first])
        quiet(lines)
        backfill.write_bytes(complete)
        quiet(lines)

    # Strict typed predicates: correlation zero is a real filter, not disabled.
    match = run('--health-json', '--format', 'jsonl', '--wait-ms', '0', '--arg-uint', '0:1', '--correlation', '0', early)
    assert match.returncode == 0, match.stderr
    assert len(match.stdout.splitlines()) == 1
    assert json.loads(match.stdout)['args'][0]['value'] == '1'
    health = json.loads(match.stderr.splitlines()[-1])
    assert health['kind'] == 'health' and health['exit_code'] == 0
    assert health['emitted_records'] == '1' and health['records'] == '3'
    assert health['producer_dropped_records'] is None
    assert health['skipped_records'] == '0' and health['unreadable_bytes'] == '0'
    start = time.monotonic()
    timeout = run('--wait-ms', '80', '--arg-uint', '0:999', early)
    assert timeout.returncode == 3 and not timeout.stdout, timeout.stderr
    assert time.monotonic() - start < 3
    assert run('--wait-ms', '0', '--correlation', '1', early).returncode == 3
    for option, values in (('--correlation', ['-1', '+1', '1x', '18446744073709551616', '']),
                           ('--subsystem', ['4294967296', '-1', 'x']),
                           ('--wait-ms', ['-1', '4294967296', '1x']),
                           ('--arg-uint', ['0:-1', '0:1x', ':1', '1:', '0:1:2'])):
        for value in values:
            result = run(option, value, early)
            assert result.returncode == 2 and not result.stdout, (option, value, result.stderr)
    for option, limit in (('--max-input-bytes', '1'), ('--max-records', '1'),
                          ('--max-output-bytes', '1')):
        limited = run('--format', 'jsonl', option, limit, early)
        assert limited.returncode == 4, limited.stderr
    assert run('--max-files', '1', early, late).returncode == 4
    capped = root / 'capped.s0l'
    shutil.copyfile(early, capped)
    with following(capped, '--max-records', '3') as (process, lines):
        take(lines, 3)
        shutil.copyfile(late, capped)
        assert process.wait(timeout=5) == 4
        assert lines.empty()
    bad = root / 'bad.s0l'
    bad.write_bytes(b'bad')
    damaged = run('--health-json', '--wait-ms', '50', bad)
    assert damaged.returncode == 1
    assert json.loads(damaged.stderr.splitlines()[-1])['input_error_seen'] is True
    budget = run('--health-json', '--max-input-bytes', '1', early)
    assert budget.returncode == 4
    assert json.loads(budget.stderr.splitlines()[-1])['records'] is None
    assert run('--wait-ms', '50', root / 'missing').returncode == 1

    # A real live producer commits after the wait process starts. No image surgery.
    live = root / 'live'
    live.mkdir()
    producer = subprocess.Popen([FIXTURE, str(live), 'live'], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    waiter = None
    try:
        assert producer.stdout.readline().strip() == 'ready'
        waiter = subprocess.Popen([TOOL, '--format', 'jsonl', '--wait-ms', '5000',
                                   '--arg-uint', '0:42', str(live)],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(0.25)
        assert waiter.poll() is None
        producer.stdin.write('42\n')
        producer.stdin.flush()
        assert producer.stdout.readline().strip() == 'committed'
        stdout, stderr = waiter.communicate(timeout=7)
        assert waiter.returncode == 0, stderr
        assert json.loads(stdout)['args'][0] == {'type': 9, 'value': '42'}
        if os.name != 'nt':
            with following(live) as (cancelled, lines):
                take(lines, 1)
                cancelled.send_signal(signal.SIGINT)
                assert cancelled.wait(timeout=3) == 130
    finally:
        if waiter and waiter.poll() is None:
            waiter.kill()
            waiter.wait(timeout=5)
        producer.stdin.write('quit\n')
        producer.stdin.flush()
        producer.communicate(timeout=5)
        assert producer.returncode == 0
    print('follow/wait receiving passed: late chunks, backfill, replacement, budgets, predicates and live producer')


with tempfile.TemporaryDirectory(prefix='sub0log follow ') as directory:
    main(pathlib.Path(directory))

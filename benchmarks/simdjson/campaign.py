"""Freeze and measure the opt-in experiment; preserves every timing epoch and failure.

Run with no concurrent builds/tests. Shared-host results are advisory. Counter
collection is disabled: retain elapsed/iterations, not fabricated zero counters.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--crucible', type=Path)
p.add_argument('--consumer-variants', nargs='+', default=['fresh', 'reuse', 'stdlib'])
p.add_argument('--variants', nargs='+', default=['fresh', 'reuse', 'fast', 'fast-reuse', 'buffered', 'buffered-reuse'])
a = p.parse_args()
build, output = a.build.resolve(), a.output.resolve()
output.mkdir(parents=True, exist_ok=True)
binary = build / 'benchmarks/simdjson/Sub0LogJsonEvaluation'
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
manifest = {'platform': platform.platform(), 'binary_sha256': digest(binary),
            'compiler': subprocess.check_output(['c++', '--version'], text=True),
            'compile_commands': json.loads((build / 'compile_commands.json').read_text()),
            'cooldown_seconds': 1, 'order': 'five AB/BA/AB/BA/AB pairs per variant',
            'scope': 'unprofiled, cache-hot repeated records; uncontrolled virtual-host load/frequency/temperature',
            'source_sha256': {str(path): digest(path) for path in
                [Path(__file__), Path(__file__).with_name('evaluate.cpp'),
                 Path(__file__).with_name('adapter.hpp'), Path(__file__).with_name('generate_adapter.py'),
                 build / 'benchmarks/simdjson/formatter.hpp']}}
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
for variant in a.variants:
    rows = []
    for pair in range(1, 6):
        for arm in (('control', variant) if pair % 2 else (variant, 'control')):
            time.sleep(1)
            assert digest(binary) == manifest['binary_sha256'], 'binary changed during campaign'
            raw = output / 'current-nanobench.json'
            raw.unlink(missing_ok=True)
            command = [str(binary), arm, str(raw)]
            started = time.perf_counter()
            try:
                r = subprocess.run(command, capture_output=True, text=True, timeout=90)
                row = {'pair': pair, 'arm': arm, 'returncode': r.returncode,
                       'stdout': r.stdout, 'stderr': r.stderr, 'wall_seconds': time.perf_counter() - started}
            except subprocess.TimeoutExpired as error:
                row = {'pair': pair, 'arm': arm, 'timeout': True, 'error': str(error)}
            if raw.exists():
                data = json.loads(raw.read_text())
                row['results'] = []
                for item in data['results']:
                    kept = {k: v for k, v in item.items() if k in (
                        'name', 'batch', 'epochs', 'minEpochIterations', 'median(elapsed)',
                        'medianAbsolutePercentError(elapsed)', 'totalTime')}
                    kept['measurements'] = [{k: m[k] for k in ('iterations', 'elapsed')}
                                            for m in item['measurements']]
                    row['results'].append(kept)
            rows.append(row)
            (output / (variant + '.json')).write_text(json.dumps(rows, separators=(',', ':')) + '\n')
            print(variant, pair, arm, row.get('returncode', 'timeout'), flush=True)
            if row.get('returncode') != 0:
                raise RuntimeError('failed run retained; stop campaign')
(output / 'current-nanobench.json').unlink(missing_ok=True)
if a.crucible:
    consumer = a.crucible.resolve()
    receivers = []
    for workload in ('normal', 'exhaust'):
        for variant in a.consumer_variants:
            for pair in range(1, 6):
                for arm in (('control', variant) if pair % 2 else (variant, 'control')):
                    time.sleep(1)
                    exe = consumer / arm
                    times = []
                    for sample in range(20):
                        command = [str(exe)] + (['exhaust'] if workload == 'exhaust' else [])
                        start = time.perf_counter()
                        r = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
                        times.append({'seconds': time.perf_counter() - start, 'returncode': r.returncode})
                        if r.returncode:
                            times[-1]['stderr'] = r.stderr.decode(errors='replace')
                    receivers.append({'workload': workload, 'variant': variant, 'pair': pair, 'arm': arm,
                                      'sha256': digest(exe), 'samples': times})
                    (output / 'crucible-processes.json').write_text(json.dumps(receivers, separators=(',', ':')) + '\n')

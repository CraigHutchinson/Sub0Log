"""Receive identical CLI JSON/health, then measure whole subprocesses on one image."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('build', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
build, output = a.build.resolve(), a.output.resolve()
output.mkdir(parents=True, exist_ok=True)
binaries = {'control': build / 'tools/sub0log-cat',
            **{arm: build / ('benchmarks/simdjson/sub0log-cat-simdjson-' + arm) for arm in ('fresh', 'reuse', 'stdlib')}}
receipt = {'binaries': {k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in binaries.items()},
           'scope': 'warm file cache, whole CLI process startup/read/decode/format/write; 8 edge-case records',
           'pairs': []}
with tempfile.TemporaryDirectory(prefix='simdjson-cli-') as directory:
    subprocess.run([str(build / 'tests/Sub0LogJsonlFixture'), directory], check=True, timeout=30)
    receipt['input_sha256'] = [hashlib.sha256(f.read_bytes()).hexdigest() for f in Path(directory).glob('*.s0l')]
    command = lambda arm: [str(binaries[arm]), '--format', 'jsonl', '--health-json', directory]
    expected = subprocess.run(command('control'), capture_output=True, check=True, timeout=30)
    assert len(expected.stdout.splitlines()) == 8
    for arm in ('fresh', 'reuse', 'stdlib'):
        actual = subprocess.run(command(arm), capture_output=True, check=True, timeout=30)
        assert (actual.stdout, actual.stderr) == (expected.stdout, expected.stderr), arm
    receipt['equivalent_stdout_bytes'] = len(expected.stdout)
    receipt['health'] = expected.stderr.decode()
    for variant in ('fresh', 'reuse', 'stdlib'):
        for pair in range(1, 6):
            for arm in (('control', variant) if pair % 2 else (variant, 'control')):
                time.sleep(1)
                row = {'variant': variant, 'pair': pair, 'arm': arm, 'samples': []}
                for sample in range(20):
                    start = time.perf_counter()
                    result = subprocess.run(command(arm), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
                    row['samples'].append({'seconds': time.perf_counter() - start, 'returncode': result.returncode})
                receipt['pairs'].append(row)
                (output / 'cli-processes.json').write_text(json.dumps(receipt, separators=(',', ':')) + '\n')

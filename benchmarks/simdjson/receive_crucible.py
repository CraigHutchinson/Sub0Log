"""Compile only the real consumer TU against each candidate; leave Crucible untouched.

Requires an already received Release Ninja build with diagnostics enabled. Retains
commands and binaries for a separate timing campaign; does not time compilation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('crucible_build', type=Path)
p.add_argument('evaluation_build', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--ninja', default='ninja')
a = p.parse_args()
build, evaluation, output = (x.resolve() for x in (a.crucible_build, a.evaluation_build, a.output))
output.mkdir(parents=True, exist_ok=True)
commands = json.loads((build / 'compile_commands.json').read_text())
entry, = [c for c in commands if c['file'].endswith('/src/runtime/RuntimeDiagnostics.cpp')]
source = Path(entry['file'])
base = shlex.split(entry['command'])
eval_commands = json.loads((evaluation / 'compile_commands.json').read_text())
eval_entry, = [c for c in eval_commands if c['file'].endswith('/benchmarks/simdjson/evaluate.cpp')]
includes = [v for v in shlex.split(eval_entry['command']) if v.startswith(('-I', '-DSIMDJSON_'))]
link_text = subprocess.check_output([a.ninja, '-C', str(build), '-t', 'commands',
    'crucible_diagnostics_json_fixture'], text=True).splitlines()[-1]
assert link_text.startswith(': && ') and link_text.endswith(' && :'), link_text
link_base = shlex.split(link_text[5:-5])
link_base = [v for v in link_base if not v.startswith('-Wl,--dependency-file=')]
records = []
for arm in ('control', 'fresh', 'reuse', 'stdlib'):
    text = source.read_text()
    if arm != 'control':
        text = '#include "formatter.hpp"\n#line 1 ' + json.dumps(str(source)) + '\n' + text
        call = 'sub0log::formatJson(record, merger.subsystemName(record.record_.site_->subsystem_))'
        assert text.count(call) == 1
        if arm == 'stdlib':
            text = text.replace('#include \"formatter.hpp\"', '#include \"formatter-stdlib.hpp\"')
            text = text.replace(call, 'sub0log::stdlib_eval::formatJson(record, merger.subsystemName(record.record_.site_->subsystem_))')
        elif arm == 'fresh':
            text = text.replace(call, 'sub0log::simdjson_eval::formatFresh(record, merger.subsystemName(record.record_.site_->subsystem_))')
        else:
            start = 'bool RuntimeDiagnostics::writeJson(std::ostream& output, std::ostream& health) const {'
            assert text.count(start) == 1
            text = text.replace(start, start + '\n    sub0log::simdjson_eval::Sink<true> sink;\n    sink.bulkControls_ = true;')
            text = text.replace(call, 'sub0log::simdjson_eval::formatJson(record, merger.subsystemName(record.record_.site_->subsystem_), sink)')
    else:
        text = '#line 1 ' + json.dumps(str(source)) + '\n' + text
    cpp, obj, exe = (output / (arm + suffix) for suffix in ('.cpp', '.o', ''))
    cpp.write_text(text)
    compile_cmd = base.copy()
    compile_cmd[compile_cmd.index('-o') + 1] = str(obj)
    compile_cmd[compile_cmd.index('-c') + 1] = str(cpp)
    compile_cmd += includes
    subprocess.run(compile_cmd, cwd=entry['directory'], check=True, timeout=120)
    link = link_base.copy()
    link[link.index('-o') + 1] = str(exe)
    # Supply all Diagnostics definitions before the archive to select this TU.
    link.insert(1, str(obj))
    if arm not in ('control', 'stdlib'):
        link.append(str(evaluation / '_deps/simdjson-build/libsimdjson.a'))
    subprocess.run(link, cwd=build, check=True, timeout=120)
    receiver = source.parents[2] / 'tests/runtime/receive_diagnostics.py'
    receipt = subprocess.run(['python', str(receiver), str(exe)], capture_output=True,
                             text=True, check=True, timeout=60)
    records.append({'arm': arm, 'compile': compile_cmd, 'link': link,
                    'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                    'receiver_stdout': receipt.stdout, 'receiver_stderr': receipt.stderr})
(output / 'build-receipts.json').write_text(json.dumps(records, indent=2) + '\n')

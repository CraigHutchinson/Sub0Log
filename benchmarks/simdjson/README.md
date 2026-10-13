# Optional JSON generation evaluation

This experiment compares the production formatter with simdjson v4.6.4 pinned to
`1bcf71bd85059ab6574ea1159de9298dcc1212c5`. It is off by default, is not installed,
and adds no dependency to producer or normal export targets. Python is required
only to generate the experimental schema adapters and run receiving scripts.

```sh
cmake -S . -B build-simd -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DSUB0LOG_BUILD_BENCHMARKS=ON -DSUB0LOG_BENCH_SIMDJSON=ON
cmake --build build-simd --config Release --parallel 2
ctest --test-dir build-simd -C Release --output-on-failure
python benchmarks/simdjson/campaign.py build-simd results
python benchmarks/simdjson/measure_cli.py build-simd results
```

Campaign scripts assume a single-configuration Linux build. CTest supports the
platform matrix, including multi-configuration Windows builds. Run campaigns
serially with no concurrent build/test workload; preserve the output directory.
Each variant gets five alternating AB/BA process pairs. Results include raw
elapsed/iteration samples, failures, compiler commands and binary/source hashes.
No hardware-counter data is collected. Owned output copies are timed.

| Microbenchmark arm | Change from production control |
| --- | --- |
| fresh / reuse | Scalar spelling with fresh / caller-owned builder |
| fast / fast-reuse | SIMD UTF8 validation/escaping, integer and bulk hex appends |
| buffered / buffered-reuse | Also buffer canonical six-byte control escapes |
| stdlib | Standard string reserve and bulk text/hex/control appends |

The generated CLI's `fresh` and `reuse` names refer to the final **buffered**
variants. Its `stdlib` variant does not link simdjson. All variants retain exact
floating-point spelling, invalid-UTF8 hex fallback, schema and integer strings.
The adapter owns its result and retains no input views. Reuse retains capacity.

`generate_adapter.py` adapts the current production schema at configure time;
source-shape assertions and equivalence tests make drift visible. These generated
headers are experimental, not a supported API. Tests include all byte values at
lengths 0–65, deterministic floating bit patterns, metadata extremes, actual CLI
JSON/follow receivers, and repeated builder use.

For an already configured and received Release Ninja Crucible build:

```sh
python benchmarks/simdjson/receive_crucible.py /path/to/Crucible/build/release \
  build-simd consumer-binaries
python benchmarks/simdjson/campaign.py build-simd results \
  --variants stdlib buffered buffered-reuse --crucible consumer-binaries
```

This compiles copies of the actual RuntimeDiagnostics translation unit with the
existing flags and runs Crucible's independent JSON receiver. It leaves Crucible
source untouched. The control is also recompiled; the original archive supplies
all other translation units. Whole-process timing includes startup, decode and
I/O and must be interpreted separately from formatter-only timing.

See [the evaluation record](../../docs/optimization/simdjson.md) for retained
measurements, limitations and the adoption decision.

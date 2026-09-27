# Integration tests

```
cmake --build build --target cab_check perf_bench
ctest --test-dir build --output-on-failure
```

| Test | What it checks |
|---|---|
| `cab_accuracy` | `tools/cab_check.cpp`: the realtime speaker cab (circuit + partitioned convolution, IR from the worker) against the frequency-domain chain it's built from. Fails above 1 dB. |
| `cpu_regression` | `tools/perf_bench.cpp` + `tests/perf_check.py`: the CPU cost of the realtime workloads against `tests/perf_baseline.json`. |

## CPU regressions

`perf_bench` times the thread's own CPU time (not wall clock), best of five runs:
- the speaker cab's realtime path, and its three IR rebuild stages (mic, cone, driver);
- the LA-2A as Opto runs it;
- Iron's output stage;
- both Phys Fuzz circuits.

Each result is also divided by a fixed reference workload (FFTs and complex
arithmetic) timed in the same run, so numbers compare across machines. Newton
iterations per sample are counted too; they're deterministic, so they catch a
solver getting slower even when timing noise would hide it.

`perf_check.py` fails when a benchmark's normalised cost is more than 25 % over the
baseline (`--tolerance`), or its Newton iterations more than 2 % over. The
baseline holds one entry per machine: this workstation (by hostname) and
`github-ubuntu-24.04` for CI (`.github/workflows/tests.yml`, results uploaded as an
artifact). A machine with no entry reports its numbers and passes with a warning.

Locally, run-to-run noise is about 1–4 %.

When a change is meant to cost more, or makes things faster and you want to keep
the gain, record a new baseline:

```
python3 tests/perf_check.py --bench build/perf_bench --update [--machine NAME]
```

To keep a local history for trends, add `--history tests/perf_history.jsonl`: it
appends one line per run with the git revision.

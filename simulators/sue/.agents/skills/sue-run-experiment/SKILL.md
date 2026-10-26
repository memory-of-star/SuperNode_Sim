---
name: sue-run-experiment
description: Execute approved SUE harness cases or parameter sweeps with input snapshots, bounded runtime, durable failure records and reproducible commands.
---

For v2 multi-Die/MPU topology tasks, read `harness/v2/README.md` and use `harness/v2/run.py` / `verify.py` / `render.py`. The single_switch/leaf_spine and import restrictions below describe the generic single-port CLI only. v2 supports 16 named topologies, single-thread SUE+LLR+CBFC and an abstract bounded UCIe ring; preserve its model and timing boundaries.


Read `harness/README.md`. Use `python3 harness/sue_harness.py validate <case>` then `run <case>`. If the backend source changed, run `build` first. Do not invoke the underlying executable on unvalidated task inputs. Runtime threads do not alter network or workload parameters.

For a planned sweep use the CLI `sweep` command; it records the matrix and per-case results, including failures. Do not fabricate predictions after seeing results. Every run gets a unique runs/<timestamp-id>/ containing input copies, generated SUE files, command manifest, execution status, CSVs, summary and report; latest.json is a pointer, not the evidence itself.

A simulator deadline that leaves tasks unfinished is a failed run, even if there was no process crash. Wall-clock timeout is exit code 124. Preserve failed outputs when making a justified adjustment; distinguish parameter changes from retries. Inspect stdout.log and task/port evidence before changing timers or credits. The supported workflow does not overwrite SUE protocol sources.

Hand off to analysis with the exact run directory and all failures/limitations. Running a small Kimi/GLM prefix does not authorize claiming a full training or inference simulation was completed.

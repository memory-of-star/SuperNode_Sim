---
name: sue-run-experiment
description: Execute approved SUE harness cases or parameter sweeps with input snapshots, bounded runtime, durable failure records and reproducible commands.
---

Read root AGENTS.md and simulators/sue/harness/README.md. From workspace root, run `bin/sue check --config PATH`, build with `bin/sue build` if necessary, then `bin/sue run --config PATH --output NEW_DIRECTORY`. Do not bypass validation or overwrite frozen evidence. workers controls independent case processes, not MTP inside a case.

The unified output contains expanded.json, validation.json, implementation.json, results.json, summary.csv and verification.json. Each conditions/<name>/ holds experiment.json, stdout.log, stderr.log and result.json; the result points to the underlying run. Report planned/completed counts and status, including capacity_rejected, failures and timeouts. A capacity rejection is not a measured performance point even if CLI exits successfully. Preserve failed outputs before adjustments. Do not launch historical matrices merely to initialize the harness.

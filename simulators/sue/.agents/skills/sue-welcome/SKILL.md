---
name: sue-welcome
description: Initialize and check this repository's SUE simulation harness, or run a bounded SUE smoke experiment.
---

Read root `AGENTS.md` and `simulators/sue/harness/README.md`. All commands below run from the workspace root, not the nested simulator directory. Check tool availability, then run `bin/sue check --config configs/experiments/glm-single.json`. For an authorized smoke run, build with `bin/sue build`, then `bin/sue run --config configs/experiments/glm-single.json --output results/sue/NEW_NAME`. Inspect verification.json and results.json before reporting success. Never overwrite an existing output directory.

The current multi-Die entry uses SUE AFH + LLR + CBFC, single-thread simulation. workers means independent cases. The older single-port CLI is only for explicitly requested legacy/PFC/MTP experiments; read harness/legacy-single-port.md under simulators/sue for that interface. No LLM API or plugin is required.

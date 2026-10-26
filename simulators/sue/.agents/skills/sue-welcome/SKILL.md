---
name: sue-welcome
description: Initialize and check this repository's SUE simulation harness, or run a bounded SUE smoke experiment.
---

Read `harness/README.md` relative to the repository root. Use `python3 harness/sue_harness.py` as the CLI. Establish that the source, CMake, compiler and Python are present; `build` compiles the targeted `scratch_sue-harness` backend. Do not build all scratch targets or enable all tests.

For an authorized quick trial, `init <fresh-case> --workload moe --hosts 4`, `validate <case>`, then `run <case>`. Resolve paths from the repository root. Use a fresh case name; each run creates a unique immutable snapshot automatically. Report the actual pass/failure and link the generated report, not the archive's old logs.

This is SUE AFH + LLR + CBFC/PFC/OFF, not UB. No UB protocol configuration is needed. The CLI does not call an LLM: the agent interprets natural language and writes an explicit experiment definition through the planning workflow. For a scientific comparison continue with `sue-plan-experiment`.

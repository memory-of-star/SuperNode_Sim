---
name: sue-plan-experiment
description: Turn natural-language SUE experiment requests into validated workload DAGs, configurations and controlled comparison plans.
---

Read root AGENTS.md and configs/experiments/README.md. Commands run from the workspace root. Convert the request into a unified experiment JSON, using a supported example and explicit hardware, model, parallel, deployment, workload, profiles, simulation, seeds and variants. File references resolve relative to the experiment JSON. Do not silently invent unsupported keys. Record the question, fixed controls, changed variables, expected behavior and measurement window before execution.

Use `bin/sue check --config PATH` before running. Inspect capacity admission and expanded settings. Current examples use event wakeups, 1 ps precision, 1 ns issue/notification intervals and 128 in-flight AXI requests per MPU at 190 GB/s one-way. IO packing, HBM sharing and transactional PCIe require their profiles. Bare compatibility CLI defaults are not equivalent. Use v2/run.py only for custom topology/DAG tasks outside the unified schema; read its README first. Representative blocks are not full inference. Existing authorization persists; ask only for material unresolved choices.

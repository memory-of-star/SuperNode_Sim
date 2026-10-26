---
name: sue-plan-experiment
description: Turn natural-language SUE experiment requests into validated workload DAGs, configurations and controlled comparison plans.
---

For v2 multi-Die/MPU topology tasks, read `harness/v2/README.md` and use `harness/v2/run.py` / `verify.py` / `render.py`. The single_switch/leaf_spine and import restrictions below describe the generic single-port CLI only. v2 supports 16 named topologies, single-thread SUE+LLR+CBFC and an abstract bounded UCIe ring; preserve its model and timing boundaries.


Read `harness/README.md` for supported fields and modeling boundaries. Bind the user's existing goal, topology, workload, changed variable and metrics. Ask only for information that changes the result and cannot be reasonably inferred; do not re-request execution authorization already given. State material assumptions.

Create a fresh case using `init` (p2p/ring/moe) or `import-ub`. For custom tasks write the documented `traffic.csv` schema and set workload.kind=custom. Edit `experiment.json` and `experiment-spec.md` together. Record a prediction before execution. Validate with the CLI before handing off.

Use `single_switch` or `leaf_spine`; custom topology is not yet a supported harness input. Consult DEFAULT and validate_spec in harness/sue_harness.py for the exposed parameter surface; unsupported keys must not be silently translated into stock SUE parser keys. Speed is a link parameter. Check credit window vs frame size, queue capacity, dependency DAG, and visibility_ns >= prop_ns.

For comparisons use `sweep` with one changed field, explicit values and a pre-run prediction. Multiple interacting changes require a separately explained experiment definition. Preserve negative cases. Thread-count comparison keeps visibility_ns and all traffic unchanged; exact per-task timestamps are not universally invariant with simultaneous events.

UB imports preserve logical one-way traffic and phase completion dependencies for URMA_WRITE/URMA_SEND only. They do not implement Jetty, RTP or UB memory/transaction semantics. Prefix imports must be dependency closed; label model snippets as synthetic prefixes, not full model inference.

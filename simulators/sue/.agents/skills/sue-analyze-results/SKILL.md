---
name: sue-analyze-results
description: Interpret SUE harness results, compare predicted and observed behavior, and explain task timing, LLR recovery and CBFC evidence.
---

For v2 multi-Die/MPU topology tasks, read `harness/v2/README.md` and use `harness/v2/run.py` / `verify.py` / `render.py`. The single_switch/leaf_spine and import restrictions below describe the generic single-port CLI only. v2 supports 16 named topologies, single-thread SUE+LLR+CBFC and an abstract bounded UCIe ring; preserve its model and timing boundaries.


Read `harness/README.md` and, for LLR/CBFC interpretation, `docs/api-reference.md`. Inspect the run snapshot, execution.json, summary.json and raw task/port CSVs. Regenerate with `analyze <run>` when needed. Use current run artifacts, not bundled historical logs.

Verify complete byte delivery and DAG ordering before presenting performance. end_ps is destination completion, last_enqueue_ps is queue admission, ready_ps is dependency visibility, start_ps is the end of fixed wait. Times in CSV are picoseconds; report plots use microseconds. Compute waits do not occupy a GPU resource. Dependencies incur a fixed non-network notification delay even in single-thread mode.

replay_frames counts retransmitted frames; replay_sessions counts completed REPLAY sessions. credit_exhausted_checks counts failed credit checks, not duration or packet loss. queue_blocked_polls is application queue polling, not CBFC itself. Per-host logical bytes exclude protocol/control/replay overhead; do not label that graph physical link utilization. makespan_us includes intervening computation waits and dependency notifications, and is not TTFT/TPOT.

For comparisons state changed and fixed controls and contrast the pre-run prediction with results. An incomplete tiny-credit case is evidence of a stall under those settings, not proof of a physical hardware defect. Investigate credit/window/queue counters before attributing root cause. Equal delivered bytes across MTP thread counts does not prove exact event/timing identity or general speedup.

Return concise Chinese findings and links to the standalone offline HTML report and raw evidence. Include failed/incomplete cases and the synthetic/model-prefix boundary.

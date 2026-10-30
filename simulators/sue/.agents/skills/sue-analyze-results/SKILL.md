---
name: sue-analyze-results
description: Interpret SUE harness results, compare predicted and observed behavior, and explain task timing, LLR recovery and CBFC evidence.
---

Read root AGENTS.md, docs/manual/05-results-validation.md and the selected run's input snapshots. Unified results.json and verification.json identify each condition and its status; use each result's run path to inspect raw statistics. Do not assume the old single-port report's filenames or timing semantics apply.

For the current multi-Die adapter, data_end_ps is destination data arrival; end_ps includes the transaction acknowledgement returning to source. deps waits for end_ps, receive_deps for data_end_ps, followed by configured notification delay. Resource completion includes modeled compute, memory or transfer service; it is not merely fixed waiting. The current IO AXI pipeline also has transaction-level statistics. Verify delivered bytes, dependencies and resource constraints before comparing performance.

Separate logical payload, wire bytes and utilization. Failed credit checks are counts, not congestion duration or loss. Event wakeups make retry counters incomparable to old polling counts as a proxy for delay. Use port sampling and timelines to substantiate bottlenecks. Current detailed plots require experiment-specific analysis tools; unified run itself emits summaries and evidence, not every report automatically. State seed coverage, window and configuration differences. Return Chinese findings and links to actual evidence; do not call representative-block latency TPOT or TTFT.

# SUE v2 resource and inference experiments

## Current resource entry

Use workspace-root `bin/sue check/run --config JSON`; see [unified input](../../../../configs/experiments/README.md) and [current behavior models](../../../../docs/manual/03-behavior-model.md). Profiles select the behavior; an omitted profile does not implicitly enable the current detailed model.

- Compute: per-Die FIFO service budget derived from FLOPs.
- HBM: configurable capped max-min sharing across traffic sources; unused capped bandwidth is redistributed. Legacy no-profile behavior remains FIFO.
- MPU with IO Die: individual 256 B AXI memory requests, configurable 128 in-flight requests, overlapping 200 ns access latency and FIFO bandwidth service. Current hardware profile is 190 GB/s one-way. Latency overlaps; bandwidth is not multiplied by concurrency.
- PCIe with its profile: finite DMA engines, tags, in-flight bytes, credits and receive buffers; shared host DDR/link resources and HBM backpressure. The kind-4 aggregate FIFO described below is the compatibility model, not the current chapter 9 transaction model.
- Current experiment examples use event wakeups, 1 ps simulation precision and 1 ns issue/dependency intervals. Resource/protocol service times remain separately configured.

The sections below document early resource interfaces and historical studies. Their FIFO-only description, independent PCIe links, 1 us setup and experiment counts must not be used as current chapter 8/9 defaults. Current experiments use explicit profiles and frozen expanded JSON. Full-model TPOT/TTFT and detailed GPU/DRAM microarchitecture remain outside scope.

## Historical aggregate resource interface

The optional `resources.json` input compiles to `resources.tsv` alongside the ordinary task DAG. Each row names a zero-byte local task using `id`, `kind`, `service_ns`, `units`, and `peer_die`. Kind 0 reserves a per-Die compute FIFO, kind 1 a separate per-Die HBM FIFO, kind 2 a shared MPU memory FIFO, and kind 3 transfers `units` bytes through the existing finite-buffer local UCIe ring to `peer_die`. A resource task finishes only when its service or local transfer finishes. Omitting the sidecar preserves the previous task behavior. See `verify_resources.py` and the archived legacy timestamp comparisons.

### Historical aggregate PCIe offload resource and controlled comparison

Kind 4 reserves one host-to-XPU PCIe FIFO **per XPU chip**, shared across its
four Die. Different chips have independent queues. The service duration is
the payload divided by that chip's configured PCIe rate; it does not model
PCIe TLPs, PHY, host DDR controllers, or a shared root-complex bottleneck.
Optional DMA setup is a dependency delay before queue admission and does not
occupy the link. `run.analyze` checks kind-4 non-overlap across all four Die.

`offload_experiments.py OUTPUT --workers 3` registers and runs the controlled
16-XPU study. It adds explicit remote-KV read dependencies to the existing
representative block through an optional `model_block(..., offload=...)`
hook; calls without this hook retain the previous DAG. The experiment uses
0/1/2/4/8/16 MPU on the documented experimental eight-rail topology, 190 GB/s
per MPU without a utilization discount, and independent 128 GB/s PCIe
links. Every MPU remains a shared server; aggregate pool bandwidth is not
assigned to each requester. `verify_offload.py` tests chip-wide PCIe sharing,
cross-chip concurrency, payload conservation, and unchanged legacy DAGs.

KV reads are split into 256 KiB transfers. Kimi's recurrent KDA state remains
local; its MLA KV is offloaded. Both transport paths stage bytes in HBM and
then charge the consumer's HBM read. KV append, dirty-page writeback, and full
attention QK/AV are outside this study. These are two sequential representative
block windows with integrated compute, HBM, TP/EP and offload dependencies,
**not full-model TPOT/TTFT**.

Hybrid uses disjoint backing partitions and joins both paths. Its default
MPU share is the explicitly uncalibrated service-rate heuristic
`190*M/(128*N+190*M)`; fixed-share sensitivity cases and the separate policy
"Engram on MPU, KV on PCIe" are included. Host-local availability of requested
Engram rows is assumed. Capacity associated with replication across physical
hosts is not claimed or inferred. Current study uses **1 us DMA setup per PCIe request in every arm**, including
PCIe-only, hybrid and tiered policies. Setup precedes queue admission and does
not occupy the link; concurrent setup waits can overlap. Historical 0 ns
results remain archived separately and must not be used as this study baseline.

Python translates FLOPs and bytes to service time; the adapter schedules resource contention. This is a coarse queue/roofline model, not a GPU instruction or DRAM-bank simulator. SUE AFH, LLR and CBFC remain in the original protocol implementation.

`inference_profile.py` defines hardware assumptions, frozen model metadata, capacity checks, TP replication and EP placement. The 20% reserve, effective utilization factors, MPU latency and random-read parallelism are assumptions. KV is page-sharded rather than TP-replicated; a replicated capacity alternative is reported. D2D chip aggregate is 2 TB/s one-way, allocated evenly across four Die and two outgoing ring edges per Die. Scale-up is 1.6 TB/s bidirectional, hence 0.8 TB/s outgoing. PCIe is not on the modeled SUE memory path.

Historical reproduction from `simulators/sue` (Python 3, C++ build toolchain, markdown-it-py for report):

```sh
python3 harness/v2/run.py build
python3 harness/v2/inference_experiments.py OUTPUT_DIRECTORY 3
python3 harness/v2/inference_restore.py OUTPUT_DIRECTORY 3
python3 harness/v2/inference_prefill.py OUTPUT_DIRECTORY 3
python3 harness/v2/inference_report.py OUTPUT_DIRECTORY
```

Use a fresh output directory. Read each script's registered design and plan before running. The main scan has 267 conditions; page restore adds 24 and prefill parameter chunks add 36. Capacity rejection is not a simulator failure. All performance windows are explicitly scoped: representative decode blocks, Engram lookups, page restores, or prefill parameter chunks excluding attention QK/AV and KV traffic. They are not complete-model TPOT or TTFT.

Remote expert experiments refetch selected expert weights each step with no persistent cache or prefetch. Engram reads compressed rows, using synthetic uniform addressing rather than a real text trace. Restore experiments restore fixed page sets, not entire model sessions. Requests and responses are separate SUE transactions with their own acknowledgements; this is not a complete AXI implementation.

Experimental `hw-N-M` topology names keep eight 256-port rail switches for controlled chip/MPU comparisons. They supplement the original named topology family. `BASE_TOPOLOGY_NAMES` preserves the original matrix's scope.

## Multi-Die MoE transport (default: tp-striped)

`moe_transport=tp-striped` now splits each token-to-expert activation along its feature dimension across every rank of its active TP group. The input is already replicated by the preceding TP AllReduce. Each sender transmits only its slice plus 8 bytes of fragment metadata. No free cross-Die gather or duplicate full activation is introduced. The expert remains on its owner Die and waits for every input slice before computing once.

The expert returns disjoint feature slices to those ranks. Each rank locally reduces its top-k result slices (coarse FP16 multiply/add and HBM service); a TP ring AllGather reconstructs the replicated result. Intra-chip movement uses the finite-bandwidth D2D ring; cross-chip movement uses the physical Die's SUE ports. This replaces the old output AllReduce, so A/B timing compares complete layouts, not only sender selection. The model remains a representative MoE block, not a numerically executed complete model.

`moe_transport=leader` explicitly selects the archived centralized layout for comparison. Existing frozen result directories are not regenerated. `moe_die_traffic.csv` records completed dispatch/combine payload TX/RX per Die, separately for fabric and D2D. Average payload bytes/window is not wire utilization. Rank-local copies are omitted from the network ledger. Every active TP rank participates; inactive DP groups need not send, local experts need no fabric, and port hashing/finite batch need not saturate every port. Expert return traffic still originates from the actual owner Die.

Verification: `python3 harness/v2/verify_moe_striping.py FRESH_OUTPUT_DIRECTORY`. Covers three models, GLM TP4/TP8, Kimi TP32, three seeds, leader/striped controls, and batch1. Checks all active ranks really send fabric bytes, task causality, delivery, unchanged expert choices and expert compute/weight bytes.

## JSON hardware selection

Hardware defaults now come from workspace `configs/hardware/profiles/current.json`; see `configs/hardware/README.md` and `harness/v2/hardware_run.py`. Compute, capacity, HBM/MPU service, XPU/MPU physical port rates and D2D ring rates follow the selected profile. Per-chip PCIe bandwidth is recorded but is not a simulated SUE data path. Four Die and eight ports per Die remain structural constraints. Existing cases without explicit port-rate settings retain 200 Gbit/s defaults.

## Configurable parallel groups

`--parallel` JSON now controls common TP/derived DP, EP group size, expert/token placement, MoE transport and decode expert-set reuse. Contiguous TP groups must fit within contiguous EP groups. Each EP group has a complete expert replica, with capacity and owner routing accounted for. Expert internal TP and pipeline PP remain 1 and larger values are rejected. See workspace `docs/manual/02-configuration.md`; old statements of mandatory global EP describe defaults and archived experiments.

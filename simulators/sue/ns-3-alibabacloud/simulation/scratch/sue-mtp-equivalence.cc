/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S8 MTP equivalence check (DESIGN.md §10.5, S5a): single-process multithreaded
 * adaptation of the SUE network. One run per process; the wrapper
 * (run-s8-mtp.sh) runs this binary twice per mode — S8_THREADS=1
 * (DefaultSimulatorImpl baseline) and S8_THREADS=N (NS3_MTP, auto-partition) —
 * and compares the aggregate result hash.
 *
 * Two equivalence modes (both deterministic, so the two thread counts must
 * produce byte-identical hashes):
 *   - NACK  (default): one corrupted frame (SetCorruptOnce 100th) -> one
 *     NACK -> one buffer replay -> clean convergence (lossless).
 *   - FLUSH (S8_FLUSH=1): the first 10 physical frames host0 sends to ToR8
 *     are corrupted (SetCorruptFirstN(10); the NACK-triggered replay
 *     retransmits are later physical frames, so 10 covers originals + replays)
 *     -> ToR8's RX stays in NACK_SENT (every copy of seq-0 is bad) and never
 *     ACKs; host0's in-flight buffer therefore stalls. In FLUSH mode only,
 *     LLR_DATA_AGE_TIMER_MAX_NS is cut to 4ms (the independent data-age
 *     timer, armed on the buffer empty->non-empty transition, §5.1.4), so the
 *     stalled oldest frame exceeds its age budget -> FLUSH discards the
 *     in-flight buffer -> cross-LP INIT/INIT_ECHO re-sync (INIT clears the
 *     peer's nackSent + re-syncs next_rx_seq) -> recovery (discard > 0).
 *     This is the most context-sensitive LLR path: cross-LP INIT/INIT_ECHO
 *     round trips + multiple timer cancellations, exactly what the residency
 *     assertion (§10.5 ①) guards. The assertion is live in the MTP run, so a
 *     latent off-LP arm here would abort rather than silently corrupt.
 *     (CBFC CF_Update generation is timer-gated to 1ms here — §12.2 — so the
 *     CtlOS pacing FIFO is stable and the spine links stay clean; only the
 *     host0<->ToR8 link FLUSHes.)
 *
 *   FLUSH cascade on the host0 link (documented UEC §5.1.7 next_rx_seq_vld
 *   semantics, DESIGN §5.1.7/§12): host0's data-age FLUSH also clears host0's
 *   RX vld (the reverse direction), so host0 filters (drops) everything ToR8
 *   sends it — node4's flow — without ACKing; ToR8's TX toward host0 then
 *   hits its own data-age budget and FLUSHes too, discarding ~199 in-flight
 *   frames and re-INITing (which re-arms host0's RX). Deterministic cascade:
 *   delivered ~1398/1600, spine links clean (disc 0).
 *
 * Topology: 8 hosts + 2 ToR + 2 spines (2-plane ECMP), every link CBFC+LLR.
 * Deterministic receiver-side corruption forces LLR NACK/replay across the
 * network, so the equivalence covers the retransmit path too.
 *
 * The thread count is written into a SimAI.conf-style file as SUE_MTP_THREADS
 * and read back through SueReadConf, so the config key drives the run exactly
 * as the docs (§9) describe. When >1 the scratch enables the fork's MTP module
 * and swaps in the MultithreadedSimulatorImpl BEFORE the network is built; at
 * Simulator::Run() the impl auto-partitions the whole network (zero-delay SUE
 * links -> lookahead 0 -> every node its own LP) and every node-owned timer is
 * context-routed so the node's state stays single-owner on one logical process.
 *
 * Run:
 *   cmake --build build -j8 --target scratch_sue-s8-mtp
 *   S8_THREADS=1 ./build/scratch/ns3.36.1-sue-mtp-equivalence-default
 *   S8_THREADS=8 ./build/scratch/ns3.36.1-sue-mtp-equivalence-default
 *   # or ./run-s8-mtp.sh  (runs both, diffs HASH)
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-cbfc-counters.h"
#include "ns3/sue-config.h"
#include "ns3/sue-header.h"
#include "ns3/sue-error-model.h"
#include "sue-scratch-util.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"
#include "ns3/channel.h"
#include "ns3/node.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

// S8_FLUSH=1 -> FLUSH+INIT resync equivalence scenario (DESIGN §10.5 收尾清单 ②):
// CorruptFirstN(10) keeps ToR8's RX stuck (every copy of seq-0 is bad, so it
// never ACKs); FLUSH mode cuts LLR_DATA_AGE_TIMER_MAX_NS to 4ms so host0's
// stalled in-flight buffer exceeds its data-age budget -> FLUSH (tx_discard) ->
// cross-LP INIT/INIT_ECHO re-sync -> recovery. NACK mode (default) =
// SetCorruptOnce(100) + data-age/replay timers 1s (never FLUSHes).
static bool g_flush = false;


static std::atomic<uint64_t> g_delivered{0};
static std::atomic<uint64_t> g_bytes{0};
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet> p, SueHeader)
{
  ++g_delivered;
  g_bytes += p->GetSize ();
}

/** Burst injector: srcNode sends `n` frames spaced SEND_SPACING apart to
 *  dstHostIdx. Runs under a node context (ScheduleWithContext) so the whole TX
 *  pipeline inherits the owner LP; the self-chained Simulator::Schedule keeps
 *  that context, so each frame's timers stay on the node's LP.
 *
 *  The spacing matters: CBFC refunds one CF_UPDATE per drained frame and all
 *  CtlOS (ACK/NACK/INIT_ECHO + CF_UPDATE) share the one pacing gate
 *  (LLR_CTLOS_TARGET_SPACING 2048B -> 164ns @100Gbps, DESIGN §5.6/§5.7). A
 *  line-rate back-to-back burst queues 200 CF_UPDATEs ahead of the real ACKs,
 *  pushing the ACK return past the data-age budget -> FLUSH -> re-INIT ->
 *  duplicate re-delivery. Spacing the frames (20us >> the 164ns drain slot)
 *  keeps the control queue near-empty, so NACK-driven LLR replay completes
 *  cleanly and the link never FLUSHes. At 100G the drain slot is 164ns, so
 *  the 20us spacing is far more conservative than it needs to be. */
static void SendOne (uint32_t srcNode, uint32_t dstHostIdx, uint32_t remaining);
static void SendOne (uint32_t srcNode, uint32_t dstHostIdx, uint32_t remaining)
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h;
  h.SetDst (SueMacFromXpuId (SueGetHostXpuId (dstHostIdx)));
  h.SetEtherType (SueConfig::EthType0 ());
  p->AddHeader (h);
  SueGetDevice (srcNode, 0)->SwitchSend (0, p);
  // Exactly `remaining` frames: this call consumes one, then only re-schedule
  // while more remain (remaining > 1 — the off-by-one would otherwise send
  // N+1 frames, since remaining hits 0 and still sends).
  if (remaining > 1)
    {
      Simulator::Schedule (MicroSeconds (20), &SendOne, srcNode, dstHostIdx, remaining - 1);
    }
}

/** Deterministic receiver-side corruption (LLR NACK/replay across LPs). */
static void ArmCorruptor (uint32_t rxNode, uint32_t devIdx, uint32_t every)
{
  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  if (every > 0) { em->SetBadEvery (every); }
  else { em->SetCorruptOnce (100); }   // 100th received data frame, once
  SueGetDevice (rxNode, devIdx)->SetErrorModel (em);
}

/** Aggregate result hash: every device's LLR+CBFC counters + delivered stats.
 *  The two runs must produce identical strings.
 *
 *  behavioral=true masks the ACK/NACK CtlOS tallies that are driven by the
 *  periodic cumulative-ACK cadence + coalescing + dup classification
 *  (tx_ack, tx_nack, rx_ack, rx_nack, rx_ack_nack_seq_error). These counts are
 *  order-sensitive: when a cross-LP frame arrival shares a timestamp with the
 *  local ACK timer, Default (FIFO heap) and MTP (per-LP queue) tie-break the
 *  collision differently and the cumulative ACK seq stream shifts by one level
 *  (DESIGN §12.20 "S8 等价性刀锋" — any order-sensitive behavior diverges;
 *  §12.27 residual ACK-clock tie). The behavioral hash (equivalence criterion)
 *  covers the order-INSENSITIVE core — frame delivery / replay / discard /
 *  FLUSH recovery / init handshake / CBFC — which must be byte-identical. */
static std::string ResultHash (bool behavioral = false)
{
  std::ostringstream o;
  SueScratchAppendDeviceCounters (o, behavioral);   // shared core (sue-scratch-util.h)
  o << "delivered=" << g_delivered.load () << ",bytes=" << g_bytes.load ();
  return o.str ();
}

// Aggregate the five order-sensitive ACK/NACK CtlOS tallies (masked in the
// behavioral hash) into one line so run-s8-mtp.sh can assert a per-field
// |delta| <= 1 across the two simulators: a real "ACKs swallowed/duplicated"
// MTP bug would show a large delta here while the behavioral hash stays
// identical (§12.28 criterion hardening).
static std::string AckSumHash ()
{
  uint64_t ta = 0, tn = 0, ra = 0, rn = 0, se = 0;
  for (uint32_t n = 0; n < SueGetNNodes (); ++n)
    {
      Ptr<Node> node = SueGetDevice (n, 0)->GetNode ();
      for (uint32_t d = 0; d < node->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = DynamicCast<SueNetDevice> (node->GetDevice (d));
          if (!dev || !dev->GetLlrEngine ()) { continue; }
          const SueLlrCounters &c = dev->GetLlrEngine ()->GetCounters ();
          ta += c.tx_ack_ctl_os; tn += c.tx_nack_ctl_os; ra += c.rx_ack_ctl_os;
          rn += c.rx_nack_ctl_os; se += c.rx_ack_nack_seq_error;
        }
    }
  std::ostringstream o;
  o << "MTP ACKSUM tx_ack=" << ta << " tx_nack=" << tn << " rx_ack=" << ra
    << " rx_nack=" << rn << " seq_err=" << se;
  return o.str ();
}

int
main ()
{
  // ---- config-driven thread count (SUE_MTP_THREADS key, §9) ----
  uint32_t threads = 1;
  if (const char *e = std::getenv ("S8_THREADS")) { threads = std::atoi (e); }
  if (threads == 0) { threads = 1; }
  if (const char *e = std::getenv ("S8_FLUSH")) { g_flush = std::atoi (e) != 0; }

  const std::string topo = "/tmp/sue-s8-mtp-topo.txt";
  const std::string conf = "/tmp/sue-s8-mtp.conf";
  std::ofstream (topo) << "type=SUE\n"
                       << "nodes 12\n"
                       << "switch 8 9 10 11\n"
                       << "link 0 8 100Gbps 1 CBFC 0\n"
                       << "link 1 8 100Gbps 1 CBFC 0\n"
                       << "link 2 8 100Gbps 1 CBFC 0\n"
                       << "link 3 8 100Gbps 1 CBFC 0\n"
                       << "link 4 9 100Gbps 1 CBFC 0\n"
                       << "link 5 9 100Gbps 1 CBFC 0\n"
                       << "link 6 9 100Gbps 1 CBFC 0\n"
                       << "link 7 9 100Gbps 1 CBFC 0\n"
                       << "link 8 10 100Gbps 1 CBFC 0\n"
                       << "link 8 11 100Gbps 1 CBFC 0\n"
                       << "link 9 10 100Gbps 1 CBFC 0\n"
                       << "link 9 11 100Gbps 1 CBFC 0\n";
  std::ofstream (conf) << "ENABLE_SCALEUP 1\n"
                       << "CBFC_CC_MSG_TIMER_US 0\n"
                       // CF_Update generation is timer-gated (UEC §5.2.6.1,
                       // DESIGN §5.6/§12.2): CF_min = 125000 wire B = 10us @100Gbps
                       // (one-shot flush of dirty R_VC_CF, trigger 1), CF_max =
                       // 1250000 B = 10ms (periodic full update of ALL VCs,
                       // trigger 2, loss resilience). CF_max is 10x CF_min so
                       // the full-update boundary never coincides with the
                       // dirty-trigger cadence — the generation is then
                       // order-independent across thread counts (MTP
                       // equivalence, §10.5). Without this gating, OnDrain
                       // refunded one CF_UPDATE per drained frame on the
                       // return-direction CtlOS FIFO. With
                       // LLR_CTLOS_TARGET_SPACING 2048B the cumulative-ACK
                       // cadence (ackPeriod == pacing slot) alone fills that
                       // FIFO, so every per-frame CF_UPDATE queued a growing
                       // backlog ahead of the real ACKs -> ACK return grew
                       // ~164ns per frame -> spine links (4 flows converging)
                       // hit multi-us ACK return and FLUSHed spuriously
                       // (disc numbers measured at 1G; re-measure via run-s8-mtp).
                       // Gating CF_Update to 10us drops the rate to ~0.02/slot:
                       // the FIFO stays stable, ACK return is ~164ns, and only
                       // host0's genuinely broken link
                       // (peer never ACKs) reaches its data-age budget.
                       //
                       // NACK mode (default): SetCorruptOnce(100) -> one NACK ->
                       // one replay -> clean lossless convergence. Replay /
                       // data-age / init timers are left non-binding (1s, no
                       // fire within the 15ms run) so the NACK (the immediate
                       // OnAckNack -> DoReplay path) is the only driver of the
                       // retransmit machinery.
                       //
                       // FLUSH mode (S8_FLUSH=1, §10.5 收尾清单 ②): corrupt the
                       // FIRST 10 physical frames host0 sends (SetCorruptFirstN
                       // (10), applied on ToR8 device (8,0)). The replay
                       // retransmits are LATER physical frames, so K=10 covers
                       // originals + replays and ToR8's RX LLR stays in
                       // NACK_SENT — the peer never advances/ACKs and host0's
                       // in-flight buffer stalls. (replay_ct exhaustion is NOT
                       // reachable: OnAckNack resets m_replayCt=0 on every
                       // ACK/NACK and the 1s replay_timer never fires.) FLUSH
                       // mode CUTS LLR_DATA_AGE_TIMER_MAX_NS to 4ms (replay /
                       // init stay non-binding at 1s): the independent data-age
                       // timer (armed on the buffer empty->non-empty transition
                       // in TxStore, §5.1.4) expires when the stalled oldest
                       // frame reaches its age budget -> FLUSH discards the
                       // in-flight buffer (tx_discard) -> FlushAndInit cancels
                       // the replay / data-age / pcs-lost timers and re-INITs
                       // across LPs (INIT -> clears peer nackSent + re-syncs
                       // next_rx_seq -> INIT_ECHO) -> ADVANCE -> remaining
                       // frames deliver. This is the most context-sensitive LLR
                       // path (cross-LP INIT/INIT_ECHO round trip + multiple
                       // timer cancellations), so its MTP equivalence exercises
                       // exactly what the residency assertion (§10.5 ①) guards.
                       // The 4ms budget is ~10000x the stable ACK return on the
                       // clean links, so the data-age timer never fires there
                       // (no spurious FLUSH); it fires only on host0's
                       // corrupted link, whose peer never ACKs.
                       //
                       // Expected FLUSH-mode outcome (deterministic):
                       //  - host0->ToR8: data-age FLUSH at 4ms (intended),
                       //    discards ~3 in-flight, re-INIT/INIT_ECHO recovery;
                       //  - ToR8->host0 (reverse, node4's flow): host0's FLUSH
                       //    cleared host0's RX next_rx_seq_vld (UEC §5.1.7
                       //    filtered) -> host0 drops node4's frames without
                       //    ACKing -> ToR8's TX data-age FLUSHes at ~8ms,
                       //    discards ~199 -> re-INIT re-arms host0's RX;
                       //  - spine links: clean (disc 0) — the CF gating keeps
                       //    their return FIFOs stable.
                       // delivered counts measured at 1G; 100G values re-measured
                       // by run-s8-mtp (the equivalence hash carries them).
                       << "CBFC_CF_MIN_TIMER_BYTES 125000\n"
                       << "CBFC_CF_MAX_TIMER_BYTES 1250000\n"
                       << "LLR_REPLAY_TIMER_MAX_NS 1000000000\n"
                       << "LLR_INIT_TIMER_MAX_NS 1000000000\n"
                       << "LLR_DATA_AGE_TIMER_MAX_NS " << (g_flush ? 4000000 : 1000000000) << "\n"
                       << "LLR_REPLAY_CT_MAX " << (g_flush ? 2 : 255) << "\n"
                       << "SUE_MTP_THREADS " << threads << "\n";

  // Read the key back through the parser, then enable MTP if requested. This
  // must happen before any network building: MtpInterface::Enable binds the
  // SimulatorImplementationType and Simulator::SetImplementation swaps the
  // default impl in for the placeholder the static default would create.
  SueReadConf (conf);
  UintegerValue mtpTv;
  GlobalValue::GetValueByName ("SUE_MTP_THREADS", mtpTv);
  const uint32_t mtpThreads = mtpTv.Get ();
  std::cout << "S8 " << (g_flush ? "FLUSH" : "NACK") << " threads=" << mtpThreads
            << (mtpThreads > 1 ? " (NS3_MTP auto-partition)" : " (DefaultSimulatorImpl)") << "\n";
  if (mtpThreads > 1)
    {
      MtpInterface::Enable (mtpThreads);
      Simulator::SetImplementation (CreateObject<MultithreadedSimulatorImpl> ());
    }

  // ---- build the network + wire corruption + receive observers ----
  SueEntryDispatch (topo, conf, nullptr);
  CHECK (SueGetNNodes () == 12, "12 nodes built (got " << SueGetNNodes () << ")");
  CHECK (SueGetNHosts () == 8, "8 hosts built (got " << SueGetNHosts () << ")");

  // receiver-side corruption: ONE first-hop link (host0->ToR8 direction) ->
  // LLR NACK + replay across LPs. A single isolated hop keeps the recovery
  // local. NACK mode: one corrupted frame (the 100th) -> one NACK -> one
  // buffer replay -> clean convergence. FLUSH mode: the FIRST 30 frames
  // corrupted (SetCorruptFirstN(30)) so the retransmitted seq-0 frames are
  // corrupted too -> ToR8 stays in NACK_SENT (no ACK) -> host0's 10us replay
  // timer drives replay_ct to max=2 -> FLUSH + re-INIT resync. Both are
  // deterministic, so the two runs must hash identically.
  if (g_flush)
    {
      Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
      em->SetCorruptFirstN (10);
      SueGetDevice (8, 0)->SetErrorModel (em);
    }
  else
    {
      ArmCorruptor (8, 0, 0);   // SetCorruptOnce (100th frame): see below
    }

  for (uint32_t h = 0; h < 8; ++h)
    {
      SueGetDevice (h, 0)->SetReceiveUpcall (MakeCallback (&RxObs));
    }

  // ---- traffic: each host sends N frames (20us spaced, SendOne) to the
  // far-ToR host (cross-spine); host h starts at t=1000*h us, so the last
  // burst (host7) ends at 7000 + 200*20 = 11000us < the 15ms Stop. ----
  // Dither (§10.5 MTP equivalence): with plain 1000us offsets, host h's
  // frame#(k+50) departs at the same instant as host (h+1)'s frame#k
  // (1000us = 50x20us), so two cross-LP frames co-arrive at a shared ToR
  // egress at one timestamp and the Default (FIFO) vs MTP (LP-order)
  // processing assigns the two egress wire slots differently. A distinct
  // sub-spacing start offset per host makes every frame departure unique,
  // giving both simulators a single total event order -> byte-identical.
  const uint32_t N = 200;
  const int64_t ditherNs[8] = { 0, 137, 274, 411, 548, 685, 822, 959 };
  for (uint32_t h = 0; h < 8; ++h)
    {
      uint32_t dst = (h + 4) % 8;
      Simulator::ScheduleWithContext (h, MicroSeconds (1000 * h) + NanoSeconds (ditherNs[h]),
                                      &SendOne, h, dst, N);
    }
  // S8_STOP_MS override: lets a boundary-sensitivity experiment extend the
  // run past the 15ms default (tail ACKs / in-flight CtlOS racing Stop).
  const int64_t stopMs = [] () { if (const char *e = std::getenv ("S8_STOP_MS")) { return (int64_t) std::atoll (e); } return (int64_t) 15; } ();
  Simulator::Stop (MilliSeconds (stopMs));

  auto t0 = std::chrono::steady_clock::now ();
  Simulator::Run ();
  auto wall = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();

  // ---- checks (mode-dependent) ----
  const uint32_t totalSent = 8 * N;
  bool sawDiscard = false, stuckFlush = false;
  for (uint32_t n = 0; n < SueGetNNodes (); ++n)
    {
      Ptr<Node> node = SueGetDevice (n, 0)->GetNode ();
      for (uint32_t d = 0; d < node->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = DynamicCast<SueNetDevice> (node->GetDevice (d));
          if (!dev || !dev->GetLlrEngine ()) { continue; }
          if (dev->GetLlrEngine ()->GetCounters ().tx_discard > 0) { sawDiscard = true; }
          if (dev->GetLlrEngine ()->GetTxState () == SueLlrEngine::TxState::FLUSH) { stuckFlush = true; }
        }
    }
  if (g_flush)
    {
      // FLUSH mode: the in-flight buffer is discarded at FLUSH (delivered <
      // total), tx_discard > 0, and every engine must have recovered out of
      // FLUSH via the cross-LP INIT/INIT_ECHO re-sync (no stuck FLUSH).
      CHECK (g_delivered.load () < totalSent,
             "FLUSH discarded in-flight frames (delivered " << g_delivered.load () << "/" << totalSent << ")");
      CHECK (sawDiscard, "LLR tx_discard > 0 (data-age FLUSH)");
      CHECK (!stuckFlush, "no LLR engine stuck in FLUSH (re-INIT resync completed)");
    }
  else
    {
      // NACK mode (baseline S8): lossless, no discard, no FLUSH.
      CHECK (g_delivered.load () == totalSent,
             "lossless: delivered " << g_delivered.load () << "/" << totalSent);
      CHECK (!sawDiscard, "no LLR tx_discard (no FLUSH discard path)");
      CHECK (!stuckFlush, "no LLR engine stuck in FLUSH");
    }

  // ---- 2-plane ECMP: BFS installs equal-cost multi-port prefixes; ToR8's
  // cross-spine traffic to hosts 4..7 must use BOTH spines (the default-seed
  // parity hash splits even/odd host ids across the two ports). ----
  {
    Ptr<SUESwitchNode> sp10 = DynamicCast<SUESwitchNode> (SueGetDevice (10, 0)->GetNode ());
    Ptr<SUESwitchNode> sp11 = DynamicCast<SUESwitchNode> (SueGetDevice (11, 0)->GetNode ());
    CHECK (sp10 && sp11, "nodes 10/11 are spines (switch nodes)");
    if (sp10 && sp11)
      {
        CHECK (sp10->GetCntTxPkts () > 0 && sp11->GetCntTxPkts () > 0,
               "2-plane ECMP: both spines forwarded (spine10 tx " << sp10->GetCntTxPkts ()
                                                                    << ", spine11 tx " << sp11->GetCntTxPkts () << ")");
      }
  }

  // ---- MTP partition table (informational) ----
  if (mtpThreads > 1)
    {
      std::set<uint32_t> systems;
      std::cout << "  partition (node -> system/LP):\n";
      for (uint32_t n = 0; n < SueGetNNodes (); ++n)
        {
          uint32_t s = SueGetDevice (n, 0)->GetNode ()->GetSystemId ();
          systems.insert (s);
          std::cout << "    node " << n << " -> system " << s << "\n";
        }
      CHECK (systems.size () > 1, "auto-partition produced " << systems.size () << " logical processes");
    }

  std::string hash = ResultHash ();
  std::cout << "MTP WALL_MS " << wall << "\n";
  std::cout << "MTP HASH " << hash << "\n";
  // Equivalence criterion: behavioral hash (ACK/NACK CtlOS tallies masked —
  // order-sensitive by design, §12.20/§12.27). The full MTP HASH stays printed
  // for diagnostics; run-s8-mtp.sh diffs MTP BHASH.
  std::cout << "MTP BHASH " << ResultHash (true) << "\n";
  // Masked-tally aggregates for the |delta|<=1 hardening check (§12.28).
  std::cout << AckSumHash () << "\n";
  std::cout << (g_ok ? "MTP PASS" : "MTP FAIL") << std::endl;
  Simulator::Destroy ();
  return g_ok ? 0 : 1;
}

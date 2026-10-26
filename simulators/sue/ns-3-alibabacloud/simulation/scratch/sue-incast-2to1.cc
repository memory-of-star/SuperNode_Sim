/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S5e 2:1 CBFC incast (DESIGN §12.29): three XPU hosts, all links 200Gbps
 * LLR+CBFC lossless. host0 and host1 are both line-rate senders into host2 —
 * a genuine 2:1 incast (2 × ~190G offered into one 200G egress). We observe:
 *   (a) the receiver's aggregate delivery rate (must be pinned at the single
 *       200G link ceiling, NOT the 380G offered), and its per-source split;
 *   (b) lossless end-to-end: with a FINITE burst the switch egress queue toward
 *       host2 absorbs the 2:1 excess and drains it after the burst, so
 *       delivered == sent in the end and nothing is ever dropped / replayed;
 *   (c) sender-side CBFC credit exhaustion (credit_exhausted) MUST appear and
 *       the switch MUST hold frames in its pending (GetCntPendPkts > 0): the
 *       egress→ingress backpressure coupling (§12.30, CBFC_EGRESS_BUF_BYTES
 *       0 = auto) defers the switch-ingress credit refund until the frame is
 *       admitted onto the (bounded) egress queue toward host2, so while that
 *       queue is saturated 2:1 the refund stalls, the CF_Update the senders see
 *       reports no freed credit, and the senders genuinely throttle to ~100G
 *       each — the real backpressure, replacing the pre-fix behavior where the
 *       unbounded egress queue absorbed the whole 2:1 excess silently.
 *
 * Control-plane / pacing mirror sue-s5c-thpt.cc: endpoint offered interval =
 * 0.95 × wire cycle of one frame (line-rate offer with headroom), replay_timer
 * auto = 10× LLR RTT, data-age maxed (4s) so a slow CBFC refund can never
 * spuriously FLUSH, CF_min/CF_max batch timers as shipped in the 200G preset.
 *
 * Env knobs:
 *   INCAST_RATE      link rate string ("200Gbps")
 *   INCAST_GBITS     per-sender payload (default 1 Gbit -> 61k frames @ 2048B)
 *   INCAST_PAYLOAD   frame payload bytes (2048)
 *   INCAST_CREDITS   CBFC credit limit per VC (default 1024; 512 caps ~91.75%)
 *   INCAST_INTERVAL_NS  0=auto(0.95×wire)
 *   INCAST_THREADS   SUE_MTP_THREADS (1)
 *
 * Multi-VC incast (DESIGN §7, end-to-end VC mapping): INCAST_DSCP0/INCAST_DSCP1
 * assign each sender a shim-DSCP flow class (6-bit; >0 enables the shim). The
 * wire VC is the DSCP_TO_VC_MAP result — by default direct (dscp0=0 -> VC0,
 * dscp1=3 -> VC3). INCAST_DSCP_TO_VC_MAP injects a custom mapping line into the
 * inline conf, so host send queue, switch decode and host2 receive attribution
 * all read the SAME table (non-identity proof). The multiVc CHECKs run only
 * when a VC split is requested, keeping the default run byte-identical.
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-lite-endpoint.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-cbfc-counters.h"
#include "ns3/sue-error-model.h"
#include "ns3/sue-config.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-header.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "ns3/channel.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

// host2 receive observer: payload frames/bytes + per-source attribution (via the
// frame's MAC SA, SueXpuFromMac) + first/last arrival for the delivery window.
// The upcall packet still carries the SueHeader (device strips only the LLR
// preamble), so count payload bytes = GetSize() minus the SueHeader.
static std::atomic<uint64_t> g_rxFrames{0}, g_rxBytes{0};
static std::atomic<uint64_t> g_rxBySrc[3]{0,0,0};
static std::atomic<uint64_t> g_rxBySrcBytes[3]{0,0,0};
static std::atomic<uint64_t> g_rxByVc[8]{0,0,0,0,0,0,0,0};   // §7: per-VC RX attribution (mapped VC)
static uint64_t g_firstRxNs = 0, g_lastRxNs = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet> p, SueHeader h)
{
  ++g_rxFrames;
  const uint64_t payloadBytes = p->GetSize () - h.GetSerializedSize ();
  g_rxBytes += payloadBytes;
  uint32_t src = SueXpuFromMac (h.GetSrc ());
  if (src < 3)
    {
      g_rxBySrc[src].fetch_add (1);
      g_rxBySrcBytes[src].fetch_add (payloadBytes);
    }
  // Same decode as SueNetDevice::ComputeVc / the switch (§7): shim.dscp > vlan.pcp
  // > DEFAULT_VC, through the configured map. Proves host2 attributes the same VC
  // the sender queued and the switch forwarded.
  uint8_t vc = h.HasShim () ? SueConfig::DscpToVc (h.GetShim ().dscp)
              : (h.HasVlan () ? SueConfig::PcpToVc (h.GetPcp ()) : SueConfig::DefaultVc ());
  g_rxByVc[vc].fetch_add (1);
  const uint64_t now = Simulator::Now ().GetNanoSeconds ();
  if (g_rxFrames == 1) { g_firstRxNs = now; }
  g_lastRxNs = now;
}

// Delivery curve sampler: prints (frames received so far) vs (cumulative frames
// sent so far, from the endpoints) at a few sim times. inflight = sent-not-yet-
// received = the frames the switch egress queue toward host2 is absorbing during
// the burst (2:1 excess), draining back to 0 after the burst ends. This makes
// "host2 pinned at 200G while the queue absorbs, then drains" visible instead of
// inferred from the final counters.
static Ptr<SueLiteEndpoint> g_ep0, g_ep1;   // filled after endpoints are created
static void Sample (const char *label)
{
  const uint64_t cumSent = g_ep0->GetTxCount () + g_ep1->GetTxCount ();
  const uint64_t rx = g_rxFrames.load ();
  std::cout << "    t=" << Simulator::Now ().GetNanoSeconds () << "ns rx=" << rx
            << " cumSent=" << cumSent << " bytes=" << g_rxBytes.load ()
            << " inflight=" << (cumSent - rx) << "  (" << label << ")\n";
}

// Per-source + egress instantaneous rate curve: at each fixed sample instant,
// (bytes now - bytes at previous sample) x 8 / dt = Gbps (1 bit/ns = 1 Gbps).
// src0/src1 = the two ingress (host0/host1 → host2) delivered rates — during the
// incast they must sit flat at ~97.9 Gbps each (backpressure-throttled halves of
// the ~195.9G egress ceiling), NOT ~190G offered; egress = the host2 aggregate
// (the single 200G bottleneck). The drain tail after the burst shows the buffered
// 2:1 excess still flowing at 200G until the switch pending + source queues empty.
static uint64_t g_prevSrcBytes[3]{0,0,0}, g_prevEgressBytes = 0, g_prevRateNs = 0;
static void RateSample (const char *label)
{
  const uint64_t now = Simulator::Now ().GetNanoSeconds ();
  const uint64_t srcBytes[3] = { g_rxBySrcBytes[0].load (), g_rxBySrcBytes[1].load (),
                                 g_rxBySrcBytes[2].load () };
  const uint64_t egressBytes = g_rxBytes.load ();
  const double dtNs = (now > g_prevRateNs) ? (double) (now - g_prevRateNs) : 1.0;
  const double g0 = (double) (srcBytes[0] - g_prevSrcBytes[0]) * 8.0 / dtNs;
  const double g1 = (double) (srcBytes[1] - g_prevSrcBytes[1]) * 8.0 / dtNs;
  const double ge = (double) (egressBytes - g_prevEgressBytes) * 8.0 / dtNs;
  std::cout << "    RATES t=" << now << "ns src0=" << g0
            << "Gbps src1=" << g1 << "Gbps egress=" << ge << "Gbps  (" << label << ")\n";
  for (int i = 0; i < 3; ++i) { g_prevSrcBytes[i] = srcBytes[i]; }
  g_prevEgressBytes = egressBytes;
  g_prevRateNs = now;
}

// Captured at the incast-window end (burst still in progress): host2's received
// byte count up to that instant, for the steady incast-window rate.
static uint64_t g_rxBytesAtWinEnd = 0;
static void CaptureWinEnd () { g_rxBytesAtWinEnd = g_rxBytes.load (); }

// Fine-grained CBFC credit trace during the incast burst: with the egress→
// ingress backpressure coupling (§12.30) the switch ingress port from host0
// (sw(3,0)) withholds its credit refund — R_VC_CF stays low while the egress
// toward host2 is saturated — so host0/host1's sender CU climbs to the limit
// and credit_exhausted increments: the real 2:1 backpressure signature. The
// egress device sw(3,2) is port 2 (link "3 2"); ports 0/1 are the host0/host1
// ingress links.
static void ProbeCredit (const char *label)
{
  std::cout << "  PROBE t=" << Simulator::Now ().GetNanoSeconds () << "ns (" << label << ")\n";
  Ptr<SueNetDevice> swToH2 = SueGetDevice (3, 2);   // switch egress port -> host2 (bottleneck)
  Ptr<SueNetDevice> swFrH0 = SueGetDevice (3, 0);   // switch ingress from host0 (backpressure point)
  Ptr<SueNetDevice> h0 = SueGetDevice (0, 0);
  Ptr<SueNetDevice> h1 = SueGetDevice (1, 0);
  std::cout << "    sw(3,2)->host2 S_CC=" << swToH2->GetCbfcEngine ()->GetSvcCc (0)
            << " (on the 200G wire)\n"
            << "    sw(3,0)->h0  R_CF=" << swFrH0->GetCbfcEngine ()->GetRvcCf (0)
            << " (credit refunded to host0; held while congested)\n"
            << "    h0.CU=" << h0->GetCbfcEngine ()->GetSvcCu (0)
            << " h0.CC=" << h0->GetCbfcEngine ()->GetSvcCc (0)
            << "   h1.CU=" << h1->GetCbfcEngine ()->GetSvcCu (0)
            << " h1.CC=" << h1->GetCbfcEngine ()->GetSvcCc (0) << "\n";
}

int
main ()
{
  // ---- env knobs ----
  std::string rateStr = "200Gbps";
  if (const char *e = std::getenv ("INCAST_RATE")) { rateStr = e; }
  uint32_t gbits = 1;
  if (const char *e = std::getenv ("INCAST_GBITS")) { gbits = std::atoi (e); }
  if (gbits == 0) { gbits = 1; }
  uint32_t payload = 2048;
  if (const char *e = std::getenv ("INCAST_PAYLOAD")) { payload = std::atoi (e); }
  if (payload == 0) { payload = 2048; }
  int64_t intervalNs = 0;
  if (const char *e = std::getenv ("INCAST_INTERVAL_NS")) { intervalNs = std::atoi (e); }
  uint32_t threads = 1;
  if (const char *e = std::getenv ("INCAST_THREADS")) { threads = std::atoi (e); }
  if (threads == 0) { threads = 1; }
  uint32_t creditLimit = 1024;
  if (const char *e = std::getenv ("INCAST_CREDITS")) { creditLimit = std::atoi (e); }
  if (creditLimit == 0) { creditLimit = 1024; }
  // Multi-VC flow-class knobs (§7): per-source shim-DSCP + optional map override.
  uint32_t dscp0 = 0, dscp1 = 0;
  if (const char *e = std::getenv ("INCAST_DSCP0")) { dscp0 = (uint32_t) std::strtoul (e, nullptr, 0) & 0x3F; }
  if (const char *e = std::getenv ("INCAST_DSCP1")) { dscp1 = (uint32_t) std::strtoul (e, nullptr, 0) & 0x3F; }
  std::string dscpToVcMap;
  if (const char *e = std::getenv ("INCAST_DSCP_TO_VC_MAP")) { dscpToVcMap = e; }
  const bool multiVc = (dscp0 != 0 || dscp1 != 0 || !dscpToVcMap.empty ());

  const DataRate rate (rateStr);
  const double bps = rate.GetBitRate ();

  // Per-frame wire cycle: SueHeader(14B) + payload + LLR preamble(8B) + IFG(12B).
  // Offered interval = 0.95 × this (line-rate offer with headroom).
  const Time dataWire = rate.CalculateBytesTxTime (14 + payload + 8);
  const Time ifgWire = rate.CalculateBytesTxTime (12);
  const Time wireCycle = dataWire + ifgWire;
  if (intervalNs == 0) { intervalNs = wireCycle.GetNanoSeconds () * 95 / 100; }
  const Time ackPeriod = rate.CalculateBytesTxTime (2048);
  const Time rtt = 2 * wireCycle + 2 * MicroSeconds (1) + ackPeriod;
  const uint32_t replayNs = 10 * rtt.GetNanoSeconds ();

  const uint64_t targetBytes = (uint64_t) gbits * 1000000000ULL / 8ULL;
  const uint64_t frames = (targetBytes + payload - 1) / payload;   // per sender
  std::cout << "INCAST rate=" << rateStr << " payload=" << payload << "B"
            << " per-sender=" << gbits << " Gbit (" << frames << " frames) -> 2:1 into host2"
            << " interval=" << intervalNs << "ns (wire cycle " << wireCycle.GetNanoSeconds () << "ns)"
            << " replay=" << replayNs << "ns threads=" << threads << "\n";

  // ---- topo: host0 --sw-- host2, host1 --sw-- host2 (2:1 incast), all LLR+CBFC ----
  const std::string topo = "/tmp/sue-s5e-incast-topo.txt";
  const std::string conf = "/tmp/sue-s5e-incast.conf";
  std::ofstream (topo) << "type=SUE\n"
                       << "nodes 4\n"
                       << "switch 3\n"
                       << "link 0 3 " << rateStr << " 1 CBFC 0\n"
                       << "link 1 3 " << rateStr << " 1 CBFC 0\n"
                       << "link 3 2 " << rateStr << " 1 CBFC 0\n";
  // conf/sue-200g.conf preset mirrored inline (explicit LLR timers = 10×RTT
  // computed values; data-age maxed so a slow CBFC refund never FLUSHes).
  // CBFC keys follow the 2026-08-14 THU-SPEC alignment (threshold 2075,
  // CF_MIN 0, CC 102µs, §12.37).
  std::ofstream (conf) << "ENABLE_SCALEUP 1\n"
                       << "ENABLE_SCALEUP_WORKLOAD 0\n"
                       << "SUE_MTP_THREADS " << threads << "\n"
                       << "SUE_ETH_TYPE_0 0xFCF1\n"
                       << "SUE_ETH_TYPE_0_HAS_SHIM 0\n"
                       << "SUE_ETH_TYPE_1 0xFCF2\n"
                       << "SUE_ETH_TYPE_1_HAS_SHIM 1\n"
                       << "SUE_IFG_BYTES 12\n"
                       << "LLR_MODE_LOCAL 1\n"
                       << "LLR_MODE_REMOTE 1\n"
                       << "LLR_INIT_BEHAVIOR BLOCK\n"
                       << "LLR_FLUSH_BEHAVIOR BLOCK\n"
                       << "LLR_RE_INIT_ON_DISCARD 1\n"
                       << "LLR_OUTSTANDING_SEQ_MAX 65536\n"
                       << "LLR_OUTSTANDING_DATA_MAX 1048576\n"
                       << "LLR_REPLAY_BUF_MAX 1048576\n"
                       << "LLR_REPLAY_TIMER_MAX_NS " << replayNs << "\n"
                       << "LLR_REPLAY_CT_MAX 5\n"
                       << "LLR_INIT_TIMER_MAX_NS " << replayNs << "\n"
                       << "LLR_INIT_RETRY_MAX 3\n"
                       << "LLR_DATA_AGE_TIMER_MAX_NS 4000000000\n"
                       << "LLR_CTLOS_TARGET_SPACING 2048\n"
                       << "LLR_PCS_LOST_STATUS_TIMER_MAX_NS 0\n"
                       << "FC_MODE CBFC\n"
                       << "CBFC_CREDIT_SIZE 256\n"
                       << "CBFC_VC_CREDIT_LIMIT 0:" << creditLimit << ",1:" << creditLimit
                       << ",2:" << creditLimit << ",3:" << creditLimit << ",4:" << creditLimit
                       << ",5:" << creditLimit << ",6:" << creditLimit << ",7:" << creditLimit << "\n"
                       << "CBFC_CC_MSG_TIMER_US 102\n"
                       << "CBFC_CF_THRESHOLD_BYTES 2075\n"
                       << "CBFC_CF_MIN_TIMER_BYTES 0\n"
                       << "CBFC_CF_MAX_TIMER_BYTES 1250000\n"
                       << "CBFC_EGRESS_BUF_BYTES 0\n"    // 0 = auto (creditLimit x creditSize / VC)
                       << (dscpToVcMap.empty () ? "" : "DSCP_TO_VC_MAP " + dscpToVcMap + "\n");

  SueReadConf (conf);
  SueEntryDispatch (topo, conf, nullptr);
  CHECK (SueGetNNodes () == 4, "4 nodes built (3 XPU + 1 switch, got " << SueGetNNodes () << ")");

  // ---- two senders: host0 -> host2 and host1 -> host2, simultaneous 100µs ----
  Ptr<SueLiteEndpoint> ep0 = CreateObject<SueLiteEndpoint> ();
  ep0->SetSourceXpu (0);
  ep0->SetDevice (SueGetDevice (0, 0));
  ep0->SetDestinationXpu (2);
  ep0->SetPayloadSize (payload);
  ep0->SetFrameCount ((uint32_t) frames);
  ep0->SetInterval (NanoSeconds (intervalNs));
  ep0->SetStartTime (MicroSeconds (100));
  if (dscp0 > 0) { ep0->SetUseShim (true); ep0->SetDscp ((uint8_t) dscp0); }   // §7 flow class
  ep0->Start ();

  Ptr<SueLiteEndpoint> ep1 = CreateObject<SueLiteEndpoint> ();
  ep1->SetSourceXpu (1);
  ep1->SetDevice (SueGetDevice (1, 0));
  ep1->SetDestinationXpu (2);
  ep1->SetPayloadSize (payload);
  ep1->SetFrameCount ((uint32_t) frames);
  ep1->SetInterval (NanoSeconds (intervalNs));
  ep1->SetStartTime (MicroSeconds (100));
  if (dscp1 > 0) { ep1->SetUseShim (true); ep1->SetDscp ((uint8_t) dscp1); }   // §7 flow class
  ep1->Start ();

  g_ep0 = ep0;
  g_ep1 = ep1;

  // ---- receiver: host2 per-source counting upcall ----
  SueGetDevice (2, 0)->SetReceiveUpcall (MakeCallback (&RxObs));

  // The bottleneck (200G link to host2) needs totalBits/bps to deliver both
  // bursts; the run stops a little after that (switch egress queue drained).
  const double totalBits = 2.0 * gbits * 1.0e9;
  const Time stop = MicroSeconds (100) + MicroSeconds ((uint64_t) (totalBits / bps * 1.0e6) + 2000);
  // Delivery curve samples across the burst + drain.
  uint64_t tU[] = { 200, 500, 1000, 1500, 2000, 2500, 3000, 4000, 5000, 7000, 10000 };
  for (uint64_t t : tU) { if (MicroSeconds (t) < stop) { Simulator::Schedule (MicroSeconds (t), &Sample, "sample"); } }
  // Incast-window end: 2.5ms is mid-burst (senders run until ~burstUs=5ms), so
  // host2 has been continuously saturated; capture its byte count there.
  if (MicroSeconds (2500) < stop) { Simulator::Schedule (MicroSeconds (2500), &CaptureWinEnd); }
  // Credit probes during the steady incast window (110µs..310µs, every 40µs).
  for (uint64_t t = 110; t <= 310; t += 40)
    {
      Simulator::Schedule (MicroSeconds (t), &ProbeCredit, "incast");
    }
  // Per-source + egress rate curve: every 200µs across burst + drain.
  for (uint64_t t = 200; MicroSeconds (t) < stop; t += 200)
    {
      Simulator::Schedule (MicroSeconds (t), &RateSample, "rates");
    }
  Simulator::Stop (stop);
  auto t0 = std::chrono::steady_clock::now ();
  Simulator::Run ();
  const double wall = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();

  // ---- results ----
  const uint64_t sent0 = ep0->GetTxCount ();
  const uint64_t sent1 = ep1->GetTxCount ();
  const uint64_t sentTotal = sent0 + sent1;
  const uint64_t recv = g_rxFrames.load ();
  const uint64_t bytes = g_rxBytes.load ();
  // Delivery-window rate = full incast window (both senders active + drain), i.e.
  // what the 200G bottleneck actually delivered end-to-end.
  const uint64_t durNs = (g_lastRxNs > g_firstRxNs) ? (g_lastRxNs - g_firstRxNs) : 0;
  const double gbps = (bytes > 0 && durNs > 0) ? (double) bytes * 8.0 / (double) durNs : 0.0;
  // Steady incast rate: bytes captured at the 2.5ms mid-burst mark, divided by
  // the elapsed window (first Rx -> 2.5ms). Must be pinned at ~195.8G (the 200G
  // ceiling), NOT ~380G (the offered 2×~190G).
  const double gbpsIncast = (g_rxBytesAtWinEnd > 0)
                            ? (double) g_rxBytesAtWinEnd * 8.0
                              / (double) (2500000ULL - g_firstRxNs) : 0.0;
  const uint64_t perSrc[3] = { g_rxBySrc[0].load (), g_rxBySrc[1].load (), g_rxBySrc[2].load () };
  const double split0 = (recv > 0) ? (double) perSrc[0] / (double) recv : 0.0;
  const double split1 = (recv > 0) ? (double) perSrc[1] / (double) recv : 0.0;

  // per-device LLR/CBFC counters
  uint64_t sumReplay = 0, sumNackTx = 0, sumNackRx = 0, sumDiscard = 0, sumAckTx = 0, sumCreditExh = 0;
  uint64_t creditExhByVc[8]{0,0,0,0,0,0,0,0};   // §7: per-VC credit exhaustion (independent backpressure)
  std::cout << "  per-device counters:\n";
  for (uint32_t n = 0; n < SueGetNNodes (); ++n)
    {
      Ptr<Node> node = SueGetDevice (n, 0)->GetNode ();
      for (uint32_t d = 0; d < node->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = DynamicCast<SueNetDevice> (node->GetDevice (d));
          if (!dev) { continue; }
          std::cout << "    dev[" << n << "][" << d << "]";
          if (dev->GetLlrEngine ())
            {
              const SueLlrCounters &c = dev->GetLlrEngine ()->GetCounters ();
              std::cout << " llr{tx_ok=" << c.tx_ok << " rx_ok=" << c.rx_ok
                        << " replay=" << c.tx_replay << " discard=" << c.tx_discard
                        << " nack_tx=" << c.tx_nack_ctl_os << " nack_rx=" << c.rx_nack_ctl_os << "}";
              sumReplay += c.tx_replay; sumNackTx += c.tx_nack_ctl_os; sumNackRx += c.rx_nack_ctl_os;
              sumDiscard += c.tx_discard; sumAckTx += c.tx_ack_ctl_os;
            }
          if (dev->GetCbfcEngine ())
            {
              const SueCbfcCounters &c = dev->GetCbfcEngine ()->GetCounters ();
              std::cout << " cbfc{cf_tx=" << c.cf_update_tx << " cf_rx=" << c.cf_update_rx
                        << " cc_tx=" << c.cc_update_tx << " cc_rx=" << c.cc_update_rx
                        << " cred_exh=" << c.credit_exhausted[0];
              for (int v = 1; v < 8; ++v) { std::cout << "+" << c.credit_exhausted[v]; }
              std::cout << "}";
              for (int v = 0; v < 8; ++v) { sumCreditExh += c.credit_exhausted[v]; creditExhByVc[v] += c.credit_exhausted[v]; }
            }
          std::cout << "\n";
        }
    }
  // Switch pending (backpressure gate engagement, §12.30): frames held at the
  // switch while the egress VC queue toward host2 was at its depth limit.
  Ptr<SUESwitchNode> sw = DynamicCast<SUESwitchNode> (SueGetDevice (3, 0)->GetNode ());
  const uint64_t pendPkts = sw ? sw->GetCntPendPkts () : 0;
  const uint64_t pendMax = sw ? sw->GetCntPendMax () : 0;
  std::cout << "    switch(3) pending: total=" << pendPkts << " maxDepth=" << pendMax << "\n";

  CHECK (sent0 == frames && sent1 == frames, "both senders delivered their full burst (sent "
         << sent0 << "+" << sent1 << "=" << sentTotal << "/" << (2 * frames) << ")");
  CHECK (recv == sentTotal, "host2 received ALL frames end-to-end (rx " << recv
         << "/" << sentTotal << ") — lossless, switch egress queue drained");
  CHECK (bytes == sentTotal * payload, "payload bytes complete (" << bytes
         << "/" << (sentTotal * payload) << ")");
  CHECK (sumReplay == 0, "no LLR replay (clean links, replay=" << sumReplay << ")");
  CHECK (sumDiscard == 0, "no LLR discard / FLUSH (discard=" << sumDiscard << ")");
  // The post-fix backpressure signature (§12.30): the senders are genuinely
  // throttled by egress congestion — sender CBFC credit exhausts (they'd block
  // with CU == CL) and the switch holds frames in its pending instead of
  // absorbing the whole 2:1 excess in an unbounded egress queue.
  CHECK (sumCreditExh > 0, "sender CBFC credit exhausted by egress congestion (real 2:1 backpressure, sum="
         << sumCreditExh << ")");
  CHECK (pendPkts > 0, "switch held frames in pending while the egress VC queue was full (total="
         << pendPkts << ", maxDepth=" << pendMax << ")");
  // The incast signature: receiver pinned at the single link ceiling, NOT the
  // 2× offered. At 2048B payload the measured ceiling is ~97.9% of the link
  // rate (protocol overhead: IFG 12B + preamble 8B + ACK CtlOS) — bounds scale
  // with the configured rate so 100G/200G/400G runs all check against their
  // own ceiling (200G ≈ 195.8G, 400G ≈ 391.7G, 100G ≈ 97.9G).
  CHECK (gbpsIncast > 0.90 * bps / 1.0e9 && gbpsIncast < 1.05 * bps / 1.0e9,
         "incast window rate pinned at link ceiling ("
         << gbpsIncast << " Gbps, not the 2× offered)");
  CHECK (split0 > 0.30 && split0 < 0.70 && split1 > 0.30 && split1 < 0.70,
         "per-sender split roughly balanced through the shared VC0 FIFO (src0 "
         << (100.0 * split0) << "%, src1 " << (100.0 * split1) << "%)");

  // Multi-VC (different-VC incast) verification (§7, end-to-end mapping). Only
  // when a VC split was requested, so the default run's output stays byte-
  // identical. host send queue, switch decode and host2 receive attribution all
  // read the SAME DSCP_TO_VC_MAP table, so per-VC counts must match per-source.
  if (multiVc)
    {
      const uint8_t vc0 = SueConfig::DscpToVc ((uint8_t) dscp0);
      const uint8_t vc1 = SueConfig::DscpToVc ((uint8_t) dscp1);
      const uint64_t swVc0Rx = sw ? sw->GetCntVcRx (vc0) : 0;
      const uint64_t swVc1Rx = sw ? sw->GetCntVcRx (vc1) : 0;
      std::cout << "  INCAST VC dscp0=" << dscp0 << "->vc" << (uint32_t) vc0
                << " dscp1=" << dscp1 << "->vc" << (uint32_t) vc1
                << " host2-rxByVc0=" << g_rxByVc[vc0].load ()
                << " host2-rxByVc1=" << g_rxByVc[vc1].load ()
                << " switch-rxByVc0=" << swVc0Rx << " switch-rxByVc1=" << swVc1Rx
                << " creditExhVc0=" << creditExhByVc[vc0]
                << " creditExhVc1=" << creditExhByVc[vc1] << "\n";
      CHECK (vc0 != vc1, "the two sources map to distinct VCs (vc0=" << (uint32_t) vc0
             << ", vc1=" << (uint32_t) vc1 << ")");
      CHECK (g_rxByVc[vc0].load () == perSrc[0] && g_rxByVc[vc1].load () == perSrc[1],
             "host2 RX per-VC attribution matches per-source (vc" << (uint32_t) vc0 << "="
             << g_rxByVc[vc0].load () << "==src0 " << perSrc[0] << ", vc" << (uint32_t) vc1 << "="
             << g_rxByVc[vc1].load () << "==src1 " << perSrc[1] << ") — end-to-end same table");
      CHECK (swVc1Rx == perSrc[1],
             "switch decoded the same VC as host2 (GetCntVcRx(vc" << (uint32_t) vc1 << ")="
             << swVc1Rx << " == src1 " << perSrc[1] << ")");
      CHECK (creditExhByVc[vc0] > 0 && creditExhByVc[vc1] > 0,
             "per-VC independent backpressure (credit_exhausted vc" << (uint32_t) vc0 << "="
             << creditExhByVc[vc0] << ", vc" << (uint32_t) vc1 << "=" << creditExhByVc[vc1] << ")");
    }

  std::cout << "INCAST RESULT sent=" << sentTotal << " rx=" << recv << " payload_B=" << bytes
            << " t_firstRx=" << g_firstRxNs << "ns t_lastRx=" << g_lastRxNs << "ns"
            << " dur=" << durNs << "ns"
            << " rate=" << gbps << "Gbps (full window) / " << gbpsIncast << "Gbps (incast window)\n"
            << "INCAST SPLIT src0=" << perSrc[0] << " (" << (100.0 * split0) << "%, avg "
            << ((recv > 0) ? (double) perSrc[0] * payload * 8.0 / (double) durNs : 0.0)
            << "Gbps)  src1=" << perSrc[1] << " (" << (100.0 * split1) << "%, avg "
            << ((recv > 0) ? (double) perSrc[1] * payload * 8.0 / (double) durNs : 0.0)
            << "Gbps)  other=" << perSrc[2] << "\n"
            << "INCAST COUNTERS replay=" << sumReplay << " nack_tx=" << sumNackTx
            << " nack_rx=" << sumNackRx << " ack_tx=" << sumAckTx << " discard=" << sumDiscard
            << " credit_exhausted=" << sumCreditExh << " pend=" << pendPkts
            << " pendMax=" << pendMax << " wall_ms=" << wall << "\n";
  std::cout << (g_ok ? "INCAST PASS" : "INCAST FAIL") << std::endl;
  Simulator::Destroy ();
  return g_ok ? 0 : 1;
}

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * ScaleUp supernode smoke (docs/scaleup-topology.md): build the N x 8-XPU
 * machine network (internal switch chip + 2-plane spine), run intra-machine
 * ring + cross-machine point-to-point traffic through SueLiteEndpoint, and
 * assert lossless delivery + both ECMP spine planes utilized.
 *
 * Env: SCALE_N (machines, default 64 = 512 XPUs, the project target),
 *      SCALE_FRAMES (per endpoint, default 20), SCALE_THREADS (>1 = NS3_MTP).
 * Run:
 *   cmake --build build -j8 --target scratch_sue-scale-smoke
 *   ./build/scratch/ns3.36.1-sue-scale-smoke-default
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-config.h"
#include "ns3/sue-scale-topo.h"
#include "ns3/sue-lite-endpoint.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-switch-node.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include "ns3/simulator.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/node.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

using namespace ns3;

static bool g_ok = true;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    bool c = (cond);                                                           \
    std::cout << "  [" << (c ? "ok" : "FAIL") << "] " << msg << "\n";          \
    g_ok = g_ok && c;                                                          \
  } while (0)

int
main ()
{
  uint32_t n = 64, frames = 20, threads = 1;
  if (const char *e = std::getenv ("SCALE_N")) { n = std::atoi (e); }
  if (const char *e = std::getenv ("SCALE_FRAMES")) { frames = std::atoi (e); }
  if (const char *e = std::getenv ("SCALE_THREADS")) { threads = std::atoi (e); }
  if (n < 1) { n = 64; }
  if (frames == 0) { frames = 20; }
  const SueScaleUpTopoStats st = SueScaleUpTopoPlan (n);
  const uint32_t nh = st.xpus;

  const char *topo = "/tmp/sue-scale-smoke-topo.txt";
  const char *conf = "/tmp/sue-scale-smoke.conf";
  {
    std::ofstream t (topo);
    SueWriteScaleUpTopo (t, n, "400Gbps");
    std::ofstream c (conf);
    c << "ENABLE_SCALEUP 1\n"
      << "CBFC_CC_MSG_TIMER_US 0\n"
      << "CBFC_CF_MIN_TIMER_BYTES 125000\n"
      << "CBFC_CF_MAX_TIMER_BYTES 1250000\n"
      // No corruptor -> NACK/FLUSH never trigger; LLR timers non-binding.
      << "LLR_REPLAY_TIMER_MAX_NS 1000000000\n"
      << "LLR_INIT_TIMER_MAX_NS 1000000000\n"
      << "LLR_DATA_AGE_TIMER_MAX_NS 1000000000\n"
      << "LLR_REPLAY_CT_MAX 255\n"
      // ACK cadence at UEC max spacing (16384B): at 400G that is ~328ns per
      // cumulative ACK — with 640 LLR engines the per-engine cadence dominates
      // wall time; the default 2048B (41ns) would fire ~24M ACK timer events
      // per simulated ms (measured: 83s wall for this smoke).
      << "LLR_CTLOS_TARGET_SPACING 16384\n"
      // §12.46:传播时延 100ns 预设(同 stress)——MTP lookahead>0,避免
      // lookahead=0 轮数暴雨;历史 prop=0 口径见 docs/scaleup-topology.md。
      << "SUE_LINK_PROPAGATION_NS 100\n"
      << "SUE_MTP_THREADS " << threads << "\n";
  }

  SueReadConf (conf);
  UintegerValue mtpTv;
  GlobalValue::GetValueByName ("SUE_MTP_THREADS", mtpTv);
  const uint32_t mtpThreads = mtpTv.Get ();
  std::cout << "SCALE smoke machines=" << n << " xpus=" << nh << " nodes=" << st.totalNodes
            << " links=" << st.totalLinks << " frames/ep=" << frames
            << " threads=" << mtpThreads
            << (mtpThreads > 1 ? " (NS3_MTP auto-partition)" : " (DefaultSimulatorImpl)") << "\n";
  if (mtpThreads > 1)
    {
      MtpInterface::Enable (mtpThreads);
      Simulator::SetImplementation (CreateObject<MultithreadedSimulatorImpl> ());
    }

  auto tSetup0 = std::chrono::steady_clock::now ();
  SueEntryDispatch (topo, conf, nullptr);
  auto setupMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - tSetup0).count ();
  std::cout << "  build+route wall_ms=" << setupMs << "\n";
  CHECK (SueGetNNodes () == st.totalNodes, "total nodes built (got " << SueGetNNodes () << ")");
  CHECK (SueGetNHosts () == nh, "XPU hosts built (got " << SueGetNHosts () << ")");

  // ---- traffic: intra-machine ring + cross-machine point-to-point ----
  // XPU (machine m, slot k) = host index m*8+k.
  //   slot 0: cross-machine -> slot 0 of machine (m+1)%N (even dst XPU-id)
  //   slot 7: cross-machine -> slot 7 of machine (m+1)%N (odd dst XPU-id)
  //            — the ECMP hash entropy is currently XPU-id parity only
  //            (AGENTS 推迟项), so BOTH slots are needed to exercise both
  //            spine planes end to end.
  //   slot 1..6: intra-machine ring -> slot k+1 of the same machine
  const int64_t ditherNs[8] = { 0, 137, 274, 411, 548, 685, 822, 959 };
  std::vector<Ptr<SueLiteEndpoint>> eps = SueInstallLiteEndpoints (
    [&] (Ptr<SueLiteEndpoint> ep, uint32_t h) {
      const uint32_t m = h / 8, k = h % 8;
      const uint32_t dst = (k == 0 || k == 7)
                             ? (((m + 1) % n) * 8 + k)  // cross-machine
                             : (m * 8 + (k + 1));       // intra-machine ring
      ep->SetDestinationXpu (dst);
      ep->SetInterval (MicroSeconds (20));
      ep->SetFrameCount (frames);
      ep->SetDither (NanoSeconds (ditherNs[h % 8]));
      ep->SetStartTime (MicroSeconds (10) + NanoSeconds (700) * h);
      ep->Start ();
    });

  // Sim time budget: last start ~10us + 700ns*nh; per-endpoint burst ~frames*interval(20us).
  Simulator::Stop (MicroSeconds (10) + NanoSeconds (700) * nh + MicroSeconds (20) * (frames + 8));
  auto t0 = std::chrono::steady_clock::now ();
  Simulator::Run ();
  auto runMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();

  // ---- verdicts ----
  const uint64_t totalExpected = (uint64_t) frames * nh;
  uint64_t totalTx = 0, totalRx = 0;
  for (auto &ep : eps) { totalTx += ep->GetTxCount (); totalRx += ep->GetRxCount (); }
  CHECK (totalTx == totalExpected, "every XPU sent (tx " << totalTx << "/" << totalExpected << ")");
  CHECK (totalRx == totalExpected, "lossless delivery (rx " << totalRx << "/" << totalExpected << ")");
  if (totalRx != totalExpected)
    {
      std::cout << "  [dbg] per-machine rx shortfall:";
      for (uint32_t m = 0; m < n; ++m)
        {
          uint64_t rx = 0;
          for (uint32_t k = 0; k < 8; ++k) { rx += eps[m * 8 + k]->GetRxCount (); }
          if (rx != 8 * frames) { std::cout << " m" << m << "=" << rx << "/" << 8 * frames; }
        }
      std::cout << "\n";
    }

  bool sawDiscard = false, stuckFlush = false;
  for (uint32_t nd = 0; nd < SueGetNNodes (); ++nd)
    {
      Ptr<Node> node = SueGetDevice (nd, 0)->GetNode ();
      for (uint32_t d = 0; d < node->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = DynamicCast<SueNetDevice> (node->GetDevice (d));
          if (!dev || !dev->GetLlrEngine ()) { continue; }
          if (dev->GetLlrEngine ()->GetCounters ().tx_discard > 0) { sawDiscard = true; }
          if (dev->GetLlrEngine ()->GetTxState () == SueLlrEngine::TxState::FLUSH) { stuckFlush = true; }
        }
    }
  CHECK (!sawDiscard, "no LLR discard (lossless)");
  CHECK (!stuckFlush, "no engine stuck in FLUSH");

  // Both spine planes must carry cross-machine traffic (ECMP, 2-plane).
  {
    uint64_t plane0Tx = 0, plane1Tx = 0;
    for (uint32_t s = 0; s < st.spinesPerPlane; ++s)
      {
        Ptr<SUESwitchNode> sp0 = DynamicCast<SUESwitchNode> (SueGetDevice (SueScaleUpSpineNodeId (0, s, n), 0)->GetNode ());
        Ptr<SUESwitchNode> sp1 = DynamicCast<SUESwitchNode> (SueGetDevice (SueScaleUpSpineNodeId (1, s, n), 0)->GetNode ());
        if (sp0) { plane0Tx += sp0->GetCntTxPkts (); }
        if (sp1) { plane1Tx += sp1->GetCntTxPkts (); }
      }
    CHECK (plane0Tx > 0 && plane1Tx > 0,
           "both ECMP spine planes forwarded (plane0 " << plane0Tx << ", plane1 " << plane1Tx << ")");
  }

  std::cout << "SCALE WALL_MS setup=" << setupMs << " run=" << runMs << "\n";
  std::cout << (g_ok ? "SCALE SMOKE PASS" : "SCALE SMOKE FAIL") << std::endl;
  Simulator::Destroy ();
  return g_ok ? 0 : 1;
}

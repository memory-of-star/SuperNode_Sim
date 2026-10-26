/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * E7 满负载压测 (docs/scaleup-topology.md, DESIGN §11 S5b 验收): N x 8-XPU
 * machine network (internal switch + 2-plane fat spine), ALL endpoints send
 * continuously cross-machine (ring to the same slot of the next machine) for
 * a bounded sim window, then drain — measures the 512-XPU operating envelope:
 *   - setup wall (BFS routing superlinearity, 64/128/256 measured separately)
 *   - sustained aggregate throughput (delivered bytes / window)
 *   - event count + wall-clock per simulated µs
 *   - CBFC backpressure (credit_exhausted) under oversubscription
 *   - lossless verdict (no discard / no NACK / no FLUSH; full drain rx==tx)
 *   - both ECMP spine planes utilized; MTP wall vs single-thread
 *   - peak RSS (no-load mode: partition + memory + ACK-timer-only event rate)
 *
 * Env: SCALE_N (machines, default 64 = 512 XPU), SCALE_DURATION_US (default
 *      50), SCALE_INTERVAL_NS (per-endpoint send interval, default 128 =
 *      ~125G demand per XPU -> 64T aggregate > 51.2T fabric cross-section,
 *      saturating), SCALE_PAYLOAD (default 2000B), SCALE_THREADS (>1 = MTP),
 *      SCALE_LOAD (0 = no-load measurement), SCALE_SPACING (ACK cadence wire
 *      bytes, default 16384 = UEC max).
 * Run:
 *   cmake --build build -j8 --target scratch_sue-scale-stress
 *   ./build/scratch/ns3.36.1-sue-scale-stress-default
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-config.h"
#include "ns3/sue-scale-topo.h"
#include "ns3/sue-lite-endpoint.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-switch-node.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/simulator.h"

#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

static std::vector<Ptr<SueLiteEndpoint>> g_eps;
static void
DrainStopAll ()
{
  for (auto &ep : g_eps) { ep->Stop (); }
}

// Rx bytes sampled at the injection-stop instant: the honest sustained-rate
// basis (the drain tail would otherwise inflate the window aggregate above
// the physical cross-section).
static uint64_t g_rxWindowBytes;
static void
SampleRxWindow ()
{
  uint64_t rx = 0;
  for (auto &ep : g_eps) { rx += ep->GetRxBytes (); }
  g_rxWindowBytes = rx;
}

int
main ()
{
  const uint32_t n = [] { const char *e = std::getenv ("SCALE_N"); return e ? (uint32_t) std::atoi (e) : 64u; } ();
  const uint32_t durationUs = [] { const char *e = std::getenv ("SCALE_DURATION_US"); return e ? (uint32_t) std::atoi (e) : 50u; } ();
  const uint32_t intervalNs = [] { const char *e = std::getenv ("SCALE_INTERVAL_NS"); return e ? (uint32_t) std::atoi (e) : 0u; } ();
  const uint32_t payload = [] { const char *e = std::getenv ("SCALE_PAYLOAD"); return e ? (uint32_t) std::atoi (e) : 2000u; } ();
  const uint32_t threads = [] { const char *e = std::getenv ("SCALE_THREADS"); return e ? (uint32_t) std::atoi (e) : 1u; } ();
  const uint32_t load = [] { const char *e = std::getenv ("SCALE_LOAD"); return e ? (uint32_t) std::atoi (e) : 1u; } ();
  const uint32_t sat = [] { const char *e = std::getenv ("SCALE_SAT"); return e ? (uint32_t) std::atoi (e) : 0u; } ();
  const uint32_t spacing = [] { const char *e = std::getenv ("SCALE_SPACING"); return e ? (uint32_t) std::atoi (e) : 16384u; } ();
  // §12.46:链路传播时延预设 100ns(铜/光传输量级)——MTP lookahead>0,
  // 否则轮数=全局不同时间戳数(58 万轮)致 7.5× 倒退;env 可回 0 复现历史口径。
  const uint32_t propNs = [] { const char *e = std::getenv ("SCALE_PROP_NS"); return e ? (uint32_t) std::atoi (e) : 100u; } ();
const uint32_t intervalUsable = intervalNs ? intervalNs : (sat ? 96u : 256u);

  SueScaleUpTopoStats st = SueScaleUpTopoPlan (n);
  const uint32_t nh = st.xpus;

  const char *topo = "/tmp/sue-scale-stress-topo.txt";
  const char *conf = "/tmp/sue-scale-stress.conf";
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
      // ACK cadence: the per-engine ACK timer is the dominant event source at
      // scale (640 engines @41ns = 24M events/ms-sim at the 2048B default;
      // docs/scaleup-topology.md). UEC max spacing keeps the density bounded.
      << "LLR_CTLOS_TARGET_SPACING " << spacing << "\n"
      << "SUE_LINK_PROPAGATION_NS " << propNs << "\n"
      << "SUE_MTP_THREADS " << threads << "\n";
  }

  SueReadConf (conf);
  UintegerValue mtpTv;
  GlobalValue::GetValueByName ("SUE_MTP_THREADS", mtpTv);
  const uint32_t mtpThreads = mtpTv.Get ();
  std::cout << "SCALE STRESS machines=" << n << " xpus=" << nh << " nodes=" << st.totalNodes
            << " links=" << st.totalLinks << " window=" << durationUs << "us"
            << " interval=" << intervalUsable << "ns sat=" << sat << " payload=" << payload
            << " load=" << load << " threads=" << mtpThreads
            << (mtpThreads > 1 ? " (NS3_MTP auto-partition)" : " (DefaultSimulatorImpl)") << "\n";
  if (mtpThreads > 1)
    {
      MtpInterface::Enable (mtpThreads);
      Simulator::SetImplementation (CreateObject<MultithreadedSimulatorImpl> ());
    }

  auto tSetup0 = std::chrono::steady_clock::now ();
  SueEntryDispatch (topo, conf, nullptr);
  auto setupMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - tSetup0).count ();
  std::cout << "  setup wall_ms=" << setupMs << "\n";

  // ---- traffic: every slot sends cross-machine to the same slot of the next
  // machine (ring). 512 flows total; each flow's egress crosses both ECMP
  // planes by flow hash (§12.33). Clean mode (SCALE_SAT=0, interval default
  // 256ns @ 2000B = ~63G per XPU = 32 Tbps aggregate): no hot link exceeds
  // ~252G < 400G -> full acceptance, exact lossless verdict. Saturation mode
  // (SCALE_SAT=1, interval default 96ns = ~168G per XPU = 86 Tbps vs 51.2
  // Tbps cross-section): CBFC credits throttle, egress queues reject the
  // excess (endpoint txCount counts every attempt), lossless holds for every
  // ACCEPTED frame. Start stagger is per-slot dither only (<= 1us) — the
  // smoke's 700ns-per-host stagger would push half the endpoints past the
  // stress window.
  std::vector<Ptr<SueLiteEndpoint>> eps;
  if (load)
    {
      const int64_t ditherNs[8] = { 0, 137, 274, 411, 548, 685, 822, 959 };
      eps = SueInstallLiteEndpoints (
        [&] (Ptr<SueLiteEndpoint> ep, uint32_t h) {
          const uint32_t m = h / 8, k = h % 8;
          ep->SetDestinationXpu ((m + 1) % n * 8 + k);   // cross-machine ring
          ep->SetPayloadSize (payload);
          ep->SetInterval (NanoSeconds (intervalUsable));
          ep->SetFrameCount (0);                         // unlimited
          ep->SetDither (NanoSeconds (ditherNs[h % 8]));
          ep->SetStartTime (MicroSeconds (10) + NanoSeconds (ditherNs[h % 8]));
          ep->Start ();
        });
      // Stop injecting at the window end, then drain the fabric so every sent
      // frame arrives (exact lossless verdict): in-flight <= credit windows +
      // egress queues; 500us of drain at 51.2Tbps clears ~3.2GB, far above
      // the worst-case backlog.
      g_eps = eps;
      Simulator::Schedule (MicroSeconds (10 + durationUs), &SampleRxWindow);   // runs first (FIFO)
      Simulator::Schedule (MicroSeconds (10 + durationUs), &DrainStopAll);
      Simulator::Stop (MicroSeconds (10 + durationUs + 500));
    }
  else
    {
      // No-load: partition + memory + ACK-timer-only event rate.
      Simulator::Stop (MicroSeconds (10 + durationUs));
    }

  auto t0 = std::chrono::steady_clock::now ();
  Simulator::Run ();
  auto runMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();
  const uint64_t ev = Simulator::GetEventCount ();
  struct rusage ru;
  getrusage (RUSAGE_SELF, &ru);
  std::cout << "  run wall_ms=" << runMs << " events=" << ev
            << " events/ms-sim=" << (double) ev / (10 + durationUs + (load ? 500 : 0)) * 1000.0
            << " maxrss_kb=" << ru.ru_maxrss << "\n";

  if (!load)
    {
      std::cout << (g_ok ? "SCALE STRESS PASS" : "SCALE STRESS FAIL") << std::endl;
      return g_ok ? 0 : 1;
    }

  // ---- aggregate accounting ----
  uint64_t totalTx = 0, totalRx = 0, totalRxBytes = 0;
  for (auto &ep : eps)
    {
      totalTx += ep->GetTxCount ();
      totalRx += ep->GetRxCount ();
      totalRxBytes += ep->GetRxBytes ();
    }
  // Zero-loss signals across every device's engines.
  uint64_t creditExh = 0;
  bool anyNack = false, anyDiscard = false, anyFlush = false;
  for (uint32_t nd = 0; nd < SueGetNNodes (); ++nd)
    {
      Ptr<SueNetDevice> dev = SueGetDevice (nd, 0);
      if (!dev) { continue; }
      if (dev->GetCbfcEngine ())
        {
          const auto &c = dev->GetCbfcEngine ()->GetCounters ();
          for (int v = 0; v < 8; ++v) { creditExh += c.credit_exhausted[v]; }
        }
      if (dev->GetLlrEngine ())
        {
          const auto &c = dev->GetLlrEngine ()->GetCounters ();
          anyNack = anyNack || (c.tx_nack_ctl_os > 0) || (c.rx_nack_ctl_os > 0);
          anyDiscard = anyDiscard || (c.tx_discard > 0);
          anyFlush = anyFlush || (c.tx_replay > 0);   // replay exit => FLUSH chain started
        }
    }
  // Spine distribution (both ECMP planes, §12.33).
  uint64_t plane0Tx = 0, plane1Tx = 0;
  {
    Ptr<SUESwitchNode> sp0 = DynamicCast<SUESwitchNode> (SueGetDevice (SueScaleUpSpineNodeId (0, 0, n), 0)->GetNode ());
    Ptr<SUESwitchNode> sp1 = DynamicCast<SUESwitchNode> (SueGetDevice (SueScaleUpSpineNodeId (1, 0, n), 0)->GetNode ());
    plane0Tx = sp0->GetCntTxPkts ();
    plane1Tx = sp1->GetCntTxPkts ();
  }

  std::cout << "  sent=" << totalTx << " delivered=" << totalRx
            << " bytes=" << totalRxBytes << "\n";
  std::cout << "  credit_exhausted=" << creditExh
            << " nack=" << anyNack << " discard=" << anyDiscard
            << " flush=" << anyFlush << "\n";
  // Sustained fabric rate = bytes that arrived BY the injection stop (the
  // drain tail only empties the backlog; counting it in the window would
  // report above the physical cross-section).
  const double crossGbps = 2.0 * n * 400.0;   // 2 planes x N uplinks x 400G
  const double aggGbps = (double) g_rxWindowBytes * 8.0 / (double) durationUs / 1000.0;
  std::cout << "  sustained aggregate=" << aggGbps << " Gbps (fabric cross-section " << crossGbps << ")"
            << " spine plane0 tx=" << plane0Tx << " plane1 tx=" << plane1Tx << "\n";

  CHECK (totalTx > 0, "traffic injected (" << totalTx << " frames sent)");
  CHECK (!anyNack && !anyDiscard && !anyFlush, "no NACK / no discard / no FLUSH");
  CHECK (plane0Tx > 0 && plane1Tx > 0, "both ECMP spine planes carried traffic (" << plane0Tx << "/" << plane1Tx << ")");
  if (sat)
    {
      // Saturation: demand (86T) > cross-section (51.2T). CBFC credit windows
      // absorb the excess into the endpoint/egress queues (no rejection at
      // this oversubscription ratio) and the fabric must run AT the
      // cross-section for the whole window.
      CHECK (creditExh > 0, "CBFC credits throttled under oversubscription (credit_exhausted=" << creditExh << ")");
      CHECK (totalRx == totalTx, "every injected frame accepted + delivered (rx " << totalRx << "/" << totalTx << ")");
      CHECK (aggGbps > 0.9 * crossGbps, "fabric saturated at >=90% cross-section ("
                                          << aggGbps << "/" << crossGbps << " Gbps)");
    }
  else
    {
      CHECK (totalRx == totalTx, "lossless: full drain delivered every frame (rx " << totalRx << "/" << totalTx << ")");
      CHECK (creditExh == 0, "no credit exhaustion below the fabric ceiling (credit_exhausted=" << creditExh << ")");
    }

  std::cout << (g_ok ? "SCALE STRESS PASS" : "SCALE STRESS FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

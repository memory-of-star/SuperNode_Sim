/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S5b host-side endpoint scenario (DESIGN.md §6.12/§11 S5b, §12.20/§12.23):
 * the full SUE network driven by SueLiteEndpoint — every host runs the
 * self-simulated traffic generator (ring or all-to-all) over the 8-host
 * 2-plane ECMP topo, lossless LLR+CBFC, and the run self-checks that every
 * frame sent is received (ΣRx == ΣTx == N·frames), no LLR discard, no stuck
 * FLUSH, and per-VC / per-destination attribution.
 *
 * This is the S5b blocker that was previously "unimplemented": it replaces the
 * hand-written SendOne burst loops (s8) with the configurable endpoint, so the
 * S5b 512-card stress has a uniform application-level traffic driver.
 *
 * MTP equivalence (user-required this round): run-s5b.sh runs this binary with
 * ENDPOINT_THREADS=1 (DefaultSimulatorImpl baseline) and ENDPOINT_THREADS=N (NS3_MTP,
 * auto-partition) and diffs the ENDPOINT HASH — both runs must produce byte-identical
 * aggregate results. Determinism needs every frame departure unique across
 * hosts: per-host start offsets are mutually-distinct non-interval-multiple
 * dithers (§12.20, same 0/137/274/411/548/685/822/959ns table as s8), so the
 * two simulators share a single total event order. ENDPOINT_DITHER=0 disables the
 * dither (start at exact 1000us multiples) for debugging; hash equality across
 * thread counts is then NOT expected.
 *
 * Run:
 *   cmake --build build -j8 --target scratch_sue-s5b-endpoint
 *   ENDPOINT_THREADS=1 ./build/scratch/ns3.36.1-sue-endpoint-ring-default
 *   ENDPOINT_THREADS=8 ./build/scratch/ns3.36.1-sue-endpoint-ring-default
 *   # or ./run-s5b.sh  (runs both, diffs HASH)
 *
 * Env knobs: ENDPOINT_THREADS, ENDPOINT_MODE (ring|alltoall), ENDPOINT_FRAMES (100),
 *            ENDPOINT_INTERVAL_US (20), ENDPOINT_VC (0), ENDPOINT_PAYLOAD (86),
 *            ENDPOINT_DITHER (1).
 */
#include "ns3/sue-entry.h"
#include "sue-scratch-util.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-lite-endpoint.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-cbfc-counters.h"
#include "ns3/sue-config.h"
#include "ns3/sue-header.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "ns3/channel.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

/** Aggregate result hash: every device's LLR+CBFC counters + every endpoint's
 *  tx/rx/attribution. Must be byte-identical across thread counts. */
static std::string ResultHash (const std::vector<Ptr<SueLiteEndpoint>> &eps)
{
  std::ostringstream o;
  SueScratchAppendDeviceCounters (o, false, true);   // shared core; mask CF cadence tallies (§10.5)
  uint64_t delivered = 0, bytes = 0;
  for (size_t h = 0; h < eps.size (); ++h)
    {
      delivered += eps[h]->GetRxCount ();
      bytes += eps[h]->GetRxBytes ();
      o << ",ep" << h << "{tx=" << eps[h]->GetTxCount ()
        << ",rx=" << eps[h]->GetRxCount ()
        << ",vc=" << eps[h]->GetRxByVc (0) << "," << eps[h]->GetRxByVc (1) << ","
        << eps[h]->GetRxByVc (2) << "," << eps[h]->GetRxByVc (3) << ","
        << eps[h]->GetRxByVc (4) << "," << eps[h]->GetRxByVc (5) << ","
        << eps[h]->GetRxByVc (6) << "," << eps[h]->GetRxByVc (7)
        << ",bySrc=";
      for (auto &kv : eps[h]->GetRxBySrcMap ()) { o << kv.first << ":" << kv.second << ","; }
      o << "}";
    }
  o << "delivered=" << delivered << ",bytes=" << bytes;
  return o.str ();
}

int
main ()
{
  // ---- env knobs ----
  uint32_t threads = 1;
  if (const char *e = std::getenv ("ENDPOINT_THREADS")) { threads = std::atoi (e); }
  if (threads == 0) { threads = 1; }
  std::string mode = "ring";
  if (const char *e = std::getenv ("ENDPOINT_MODE"))
    {
      mode = e;
      if (mode != "ring" && mode != "alltoall")
        { std::cout << "ENDPOINT_MODE=" << mode << " invalid (ring|alltoall); using ring\n"; mode = "ring"; }
    }
  uint32_t frames = 100;
  if (const char *e = std::getenv ("ENDPOINT_FRAMES")) { frames = std::atoi (e); }
  if (frames == 0) { frames = 100; }
  uint32_t intervalUs = 20;
  if (const char *e = std::getenv ("ENDPOINT_INTERVAL_US")) { intervalUs = std::atoi (e); }
  if (intervalUs == 0) { intervalUs = 20; }
  uint32_t vc = 0;
  if (const char *e = std::getenv ("ENDPOINT_VC")) { vc = std::atoi (e); }
  uint32_t payload = 86;
  if (const char *e = std::getenv ("ENDPOINT_PAYLOAD")) { payload = std::atoi (e); }
  bool dither = true;
  if (const char *e = std::getenv ("ENDPOINT_DITHER")) { dither = std::atoi (e) != 0; }

  const uint32_t NH = 8;
  const std::string topo = "/tmp/sue-s5b-endpoint-topo.txt";
  const std::string conf = "/tmp/sue-s5b-endpoint.conf";
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
                       << "ENABLE_SCALEUP_WORKLOAD 0\n"
                       << "CBFC_CC_MSG_TIMER_US 0\n"
                       // CF_Update timer-gated (UEC §5.2.6.1, DESIGN §5.6/§12.2)
                       // so the CtlOS pacing FIFO stays stable (ACK + sparse
                       // CF_Update < drain rate); CF_max is 10x CF_min so the
                       // periodic full update never coincides with the dirty
                       // cadence — order-independent across thread counts.
                       << "CBFC_CF_MIN_TIMER_BYTES 125000\n"
                       << "CBFC_CF_MAX_TIMER_BYTES 1250000\n"
                       // No corruptor -> NACK/FLUSH never trigger; replay /
                       // data-age / init timers are non-binding (1s) so the
                       // only traffic driver is the endpoint generator.
                       << "LLR_REPLAY_TIMER_MAX_NS 1000000000\n"
                       << "LLR_INIT_TIMER_MAX_NS 1000000000\n"
                       << "LLR_DATA_AGE_TIMER_MAX_NS 1000000000\n"
                       << "LLR_REPLAY_CT_MAX 255\n"
                       << "SUE_MTP_THREADS " << threads << "\n";

  // Read the key back through the parser, then enable MTP before building.
  SueReadConf (conf);
  UintegerValue mtpTv;
  GlobalValue::GetValueByName ("SUE_MTP_THREADS", mtpTv);
  const uint32_t mtpThreads = mtpTv.Get ();
  std::cout << "ENDPOINT mode=" << mode << " vc=" << vc << " frames=" << frames
            << " interval=" << intervalUs << "us threads=" << mtpThreads
            << (mtpThreads > 1 ? " (NS3_MTP auto-partition)" : " (DefaultSimulatorImpl)") << "\n";
  if (mtpThreads > 1)
    {
      MtpInterface::Enable (mtpThreads);
      Simulator::SetImplementation (CreateObject<MultithreadedSimulatorImpl> ());
    }

  SueEntryDispatch (topo, conf, nullptr);
  CHECK (SueGetNNodes () == 12, "12 nodes built (got " << SueGetNNodes () << ")");
  CHECK (SueGetNHosts () == NH, "8 hosts built (got " << SueGetNHosts () << ")");

  // ---- install + configure every host's endpoint (bound, not yet started) ----
  // Dither (§12.20): mutually-distinct non-20us-multiple start offsets so every
  // frame departure is unique across hosts -> both simulators share one total
  // event order (MTP hash equivalence). Start at 1000*(h+1) us so the LLR INIT
  // handshake (t=0) is long settled before traffic; last burst ends at
  // 8*1000 + 100*20 = 10000us < the 15ms Stop.
  const int64_t ditherNs[8] = { 0, 137, 274, 411, 548, 685, 822, 959 };
  std::vector<Ptr<SueLiteEndpoint>> eps = SueInstallLiteEndpoints (
    [&] (Ptr<SueLiteEndpoint> ep, uint32_t h) {
      ep->SetInterval (MicroSeconds (intervalUs));
      ep->SetFrameCount (frames);
      ep->SetPayloadSize (payload);
      if (vc > 0) { ep->SetUseShim (true); ep->SetVc (vc); }
      if (mode == "ring")
        {
          ep->SetDestinationXpu ((h + 1) % NH);
        }
      else // alltoall: per-frame cycle over the other 7 hosts
        {
          std::vector<uint32_t> cyc;
          for (uint32_t x = 0; x < NH; ++x) { if (x != h) { cyc.push_back (x); } }
          ep->SetDestinationCycle (cyc);
        }
      ep->SetDither (dither ? NanoSeconds (ditherNs[h]) : NanoSeconds (0));
      ep->SetStartTime (MicroSeconds (1000 * (h + 1)));
      ep->Start ();
    });

  Simulator::Stop (MilliSeconds (15));
  auto t0 = std::chrono::steady_clock::now ();
  Simulator::Run ();
  auto wall = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - t0).count ();

  // ---- self-checks ----
  const uint64_t perHost = frames;
  const uint64_t totalExpected = perHost * NH;
  uint64_t totalTx = 0, totalRx = 0;
  for (auto &ep : eps) { totalTx += ep->GetTxCount (); totalRx += ep->GetRxCount (); }
  CHECK (totalTx == totalExpected,
         "every host sent " << frames << " frames (tx " << totalTx << "/" << totalExpected << ")");
  CHECK (totalRx == totalExpected,
         "every frame delivered (rx " << totalRx << "/" << totalExpected << ")");

  // LLR cleanliness: no discard (lossless), no engine stuck in FLUSH.
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
  CHECK (!sawDiscard, "no LLR tx_discard (lossless)");
  CHECK (!stuckFlush, "no LLR engine stuck in FLUSH");

  // Per-endpoint: VC attribution + destination completeness.
  for (uint32_t h = 0; h < NH; ++h)
    {
      if (vc > 0)
        {
          CHECK (eps[h]->GetRxByVc (vc) == eps[h]->GetRxCount (),
                 "ep" << h << " all rx frames attributed to VC" << vc
                      << " (" << eps[h]->GetRxByVc (vc) << "/" << eps[h]->GetRxCount () << ")");
        }
      if (mode == "ring")
        {
          uint32_t partner = (h + NH - 1) % NH;
          CHECK (eps[h]->GetRxBySrc (partner) == perHost,
                 "ep" << h << " received " << perHost << " from ring partner xpu" << partner
                      << " (got " << eps[h]->GetRxBySrc (partner) << ")");
          CHECK (eps[h]->GetRxBySrcMap ().size () == 1,
                 "ep" << h << " single source (got " << eps[h]->GetRxBySrcMap ().size () << ")");
        }
      else // alltoall
        {
          uint32_t partners = 0;
          for (uint32_t x = 0; x < NH; ++x)
            {
              if (x != h && eps[h]->GetRxBySrc (x) > 0) { ++partners; }
            }
          CHECK (partners == NH - 1,
                 "ep" << h << " received from all " << (NH - 1) << " partners (got " << partners << ")");
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

  std::string hash = ResultHash (eps);
  std::cout << "ENDPOINT WALL_MS " << wall << "\n";
  std::cout << "ENDPOINT HASH " << hash << "\n";
  std::cout << (g_ok ? "ENDPOINT PASS" : "ENDPOINT FAIL") << std::endl;
  Simulator::Destroy ();
  return g_ok ? 0 : 1;
}

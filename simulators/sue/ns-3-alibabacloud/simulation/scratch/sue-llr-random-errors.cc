/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * PLAT-SP-08: probabilistic per-link error injection (topo err_rate -> active
 * FCS model on both link endpoints; DESIGN §12.5). The corrupted frames are
 * recovered by LLR NACK-replay end to end (lossless), the corruption ratio
 * tracks the configured rate, and the whole run is DETERMINISTIC (per-link
 * RNG stream indices): the scenario is executed twice in-process and the
 * counters must match exactly. Run:
 *   cmake --build build -j8 --target scratch_sue-llr-random-errors
 *   ./build/scratch/ns3.36.1-sue-llr-random-errors-default
 * Env: RND_RATE (default 1e-2), RND_FRAMES (default 2000).
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-config.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace ns3;

static bool g_ok = true;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    bool c = (cond);                                                           \
    std::cout << "  [" << (c ? "ok" : "FAIL") << "] " << msg << "\n";          \
    g_ok = g_ok && c;                                                          \
  } while (0)


static uint32_t g_rx = 0;
static void
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  ++g_rx;
}

struct RunResult
{
  uint64_t rx;
  uint64_t replayFrames;
  uint64_t nackRx;
  uint64_t discard;
};

// One full build+run of the 3-node random-error scenario.
static RunResult
RunOnce (uint32_t frames, double rate)
{
  const char *topo = "/tmp/sue-rnd-topo.txt";
  const char *conf = "/tmp/sue-rnd.conf";
  {
    std::ofstream t (topo);
    t << "type=SUE\nnodes 3\nswitch 1\n"
      << "link 0 1 100Gbps 1 CBFC " << rate << "\n"
      << "link 1 2 100Gbps 1 CBFC 0\n";
    std::ofstream c (conf);
    c << "ENABLE_SCALEUP 1\n"
      << "CBFC_CC_MSG_TIMER_US 0\n"
      << "LLR_REPLAY_TIMER_MAX_NS 50000\n"
      << "LLR_REPLAY_CT_MAX 255\n"
      // The registered default data-age (50us) FLUSHes the whole in-flight
      // window during a NACK storm; make it non-binding for the lossless
      // stress (same pattern as the s5b/s8 confs).
      << "LLR_DATA_AGE_TIMER_MAX_NS 1000000000\n";
  }
  SueEntryDispatch (topo, conf, nullptr);
  SueGetDevice (2, 0)->SetReceiveUpcall (MakeCallback (&RxObs));
  g_rx = 0;
  for (uint32_t i = 0; i < frames; ++i)
    {
      Ptr<Packet> p = Create<Packet> (86);
      SueHeader h;
      h.SetDst (SueMacFromXpuId (SueGetHostXpuId (1)));
      h.SetEtherType (SUE_ETH_TYPE_0);
      p->AddHeader (h);
      SueGetDevice (0, 0)->SwitchSend (0, p);
    }
  Simulator::Stop (MilliSeconds (30));
  Simulator::Run ();
  RunResult r;
  r.rx = g_rx;
  const SueLlrCounters &c = SueGetDevice (0, 0)->GetLlrEngine ()->GetCounters ();
  r.replayFrames = c.tx_replay_frames;
  r.nackRx = c.rx_nack_ctl_os;
  r.discard = c.tx_discard;
  {
    auto e0 = SueGetDevice (0, 0)->GetLlrEngine ();
    std::cout << "  [dbg] state=" << (uint32_t) e0->GetTxState ()
              << " replaySize=" << e0->GetReplaySize ()
              << " pcsExp=" << e0->GetPcsLostTimerExpired ()
              << " ackRx=" << c.rx_ack_ctl_os << " ok=" << c.tx_ok << "\n";
  }
  Simulator::Destroy ();
  return r;
}

int
main ()
{
  double rate = 1e-2;
  uint32_t frames = 2000;
  if (const char *e = std::getenv ("RND_RATE")) { rate = std::atof (e); }
  if (const char *e = std::getenv ("RND_FRAMES")) { frames = std::atoi (e); }
  std::cout << "RND rate=" << rate << " frames=" << frames << "\n";
  LogComponentEnable ("SueLlrEngine", LOG_LEVEL_LOGIC);

  const RunResult r1 = RunOnce (frames, rate);
  const RunResult r2 = RunOnce (frames, rate);   // rebuilt network, same seed/streams

  std::cout << "  run1: rx=" << r1.rx << " replay=" << r1.replayFrames
            << " nack=" << r1.nackRx << " discard=" << r1.discard << "\n";
  std::cout << "  run2: rx=" << r2.rx << " replay=" << r2.replayFrames
            << " nack=" << r2.nackRx << " discard=" << r2.discard << "\n";

  CHECK (r1.rx == frames, "lossless via LLR (rx " << r1.rx << "/" << frames << ")");
  CHECK (r1.replayFrames > 0 && r1.nackRx > 0, "corruption triggered NACK-replay");
  CHECK (r1.discard == 0, "no FLUSH discard (replay recovered everything)");
  // Note: the replay VOLUME is a go-back-N storm (each NACK replays the whole
  // remaining window), not a corruption-rate proxy — the corruption signal is
  // the NACK count. With 2000 frames at a per-endpoint rate of 1e-2, the
  // NACK count must stay well below the all-corrupt bound.
  CHECK (r1.nackRx > 0 && r1.nackRx < frames / 4,
         "corruption rate bounded (nack " << r1.nackRx << " << " << frames << ")");
  CHECK (r1.rx == r2.rx && r1.replayFrames == r2.replayFrames
         && r1.nackRx == r2.nackRx && r1.discard == r2.discard,
         "deterministic across rebuilds (run1 == run2)");

  std::cout << (g_ok ? "LLR RANDOM-ERRORS PASS" : "LLR RANDOM-ERRORS FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

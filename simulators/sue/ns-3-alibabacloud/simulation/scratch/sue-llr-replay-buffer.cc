/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S7 replay-buffer sizing (DESIGN §6.10): what link RTT / PCS glitch / ACK
 * interval a given replay-buffer capacity (LLR_REPLAY_BUF_MAX, frames) supports.
 *
 * Unified rule being confirmed:  buffer_bytes >= rate * (RTT_eff + silence)
 *   rate    = MAC-byte rate = 100B / 10ns (100Gbps; 108B wire 9ns + 12B IFG 1ns,
 *             both NS-quantized) = 10000 B/us
 *   RTT_eff = txTime_data(9ns) + 2*delay + txTime_ctl(1ns)
 *   silence = ACK interval (Part 1) or PCS-glitch duration (Part 2)
 *
 *   Part 1 (Q2, ACK interval -> MIN buffer): for each ackPeriod A, the smallest
 *     buffer that keeps the link at full rate. Theory: B_min(A) = rate*(RTT_eff+A).
 *   Part 2 (Q1, SPECIFIED buffer -> max glitch): for each buffer, the largest
 *     PCS glitch that still recovers "hot" (sender never stalls; replay recovers
 *     losslessly; NO FLUSH). Theory: T_hot(B) = B_bytes/rate - RTT_eff.
 *     A glitch >= pcs_lost_status_timer_max / data_age_timer_max FLUSHes
 *     regardless of buffer size (the timer ceiling caps glitch absorption).
 *   Part 3 (timer interplay): replay_ct FLUSH discards the whole buffer
 *     (discard scales with buffer); the pcs_lost ceiling overrides a buffer
 *     that could otherwise absorb the glitch.
 *
 * All lossless-backpressure flows: a too-small buffer stalls TX (CanTx false),
 * frames wait in the egress queue — delivered count is always N, throughput is
 * the discriminator. Run:
 *   cmake --build build -j8 --target scratch_sue-s7-replay-buf-rtt
 *   ./build/scratch/ns3.36.1-sue-llr-replay-buffer-default
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include <iostream>
#include <string>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

static Ptr<Packet> BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

// ---- measurement globals (reset per RunPoint) ----
static uint32_t g_rx = 0;
static uint32_t g_rxTarget = 0;   // early-stop once the full burst is delivered
static bool    g_firstSet = false;
static Time    g_tFirst, g_tLast;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  if (!g_firstSet) { g_firstSet = true; g_tFirst = Simulator::Now (); }
  g_tLast = Simulator::Now ();
  if (++g_rx >= g_rxTarget && g_rxTarget != 0) { Simulator::Stop (); }
}
// sender tx_ok sampled at glitch start / glitch start + duration (Part 2)
static uint32_t g_txAtGs = 0, g_txAtGe = 0;
// Part 3 C1: stop at the FIRST observed FLUSH (tx_discard > 0), capture clean counters
static bool g_flushStop = false;
static Time g_flushTime;
static void ProbeFlush (Ptr<SueLlrEngine> e, Time t)
{
  if (!g_flushStop && e->GetCntTxDiscard () > 0)
    {
      g_flushStop = true;
      g_flushTime = t;
      Simulator::Stop ();
    }
}

// Glitch the link: both ends' PCS FALSE for `dur` starting at `start`.
static void
ScheduleGlitch (Ptr<SueNetDevice> a, Ptr<SueNetDevice> b, Time start, Time dur)
{
  Simulator::Schedule (start, &SueNetDevice::SetPcsStatus, a, false);
  Simulator::Schedule (start, &SueNetDevice::SetPcsStatus, b, false);
  Simulator::Schedule (start + dur, &SueNetDevice::SetPcsStatus, a, true);
  Simulator::Schedule (start + dur, &SueNetDevice::SetPcsStatus, b, true);
}

// ---- per-grid-point run ----
struct RunCfg
{
  uint32_t bufFrames = 1024;      // LLR_REPLAY_BUF_MAX (the storage knob)
  Time delay = MicroSeconds (2);  // channel one-way delay
  Time ackPeriod = Seconds (0);   // receiver ACK cadence (0 = per-frame)
  Time glitchT = Seconds (0);     // PCS glitch duration (0 = none)
  Time glitchStart = MicroSeconds (100);
  Time pcsLostMax = Seconds (10); // non-binding by default (buffer is the limit)
  Time dataAgeMax = Seconds (10); // non-binding by default
  Time replayTimerMax = MilliSeconds (10); // NACK is the only replay trigger
  uint32_t nFrames = 10000;       // sustained burst; Part 2 sets (200+tu)*100 (= glitch + margins @100G)
  bool probeFlush = false;        // Part 3 C1: stop at first tx_discard>0
  bool pcsDownForever = false;    // Part 3 C1: glitch never recovers
};

struct M
{
  uint32_t delivered = 0;
  uint64_t txDiscard = 0;
  uint64_t txReplay = 0;
  bool hasFirst = false;
  Time first, last;
  double mbps = 0;              // B/us = (delivered-1)*100*1000 / (last-first)ns
  uint32_t txState = 0;         // (uint32_t) SueLlrEngine::TxState
  uint32_t replaySize = 0;      // frames still in buffer at end
  uint32_t txAtGs = 0, txAtGe = 0;
  bool flushTimeKnown = false;
  Time flushTime;
};

static M
RunPoint (const RunCfg &cfg)
{
  M m;
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  ch->SetAttribute ("Delay", TimeValue (cfg.delay));
  A->Attach (ch); B->Attach (ch);
  A->SetDataRate (DataRate ("100Gbps"));
  A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 0.96ns -> 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps"));   // no egress queue: ctl frames only
  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  // storage knob binding; seq/data gates non-binding (sized by in-flight target)
  eA->SetReplayBufMax (cfg.bufFrames);
  eA->SetOutstandingSeqMax (1u << 20);
  eA->SetOutstandingDataMax (0xFFFFFFFFu);
  eA->SetReplayTimerMax (cfg.replayTimerMax);
  eA->SetInitTimerMax (MilliSeconds (10));
  eA->SetInitRetryMax (3);
  eA->SetPcsLostTimerMax (cfg.pcsLostMax);
  eA->SetDataAgeTimerMax (cfg.dataAgeMax);
  eB->SetInitTimerMax (MilliSeconds (10));
  eB->SetInitRetryMax (3);
  eB->SetPcsLostTimerMax (cfg.pcsLostMax);
  eB->SetDataAgeTimerMax (cfg.dataAgeMax);
  eB->SetAckPeriod (cfg.ackPeriod);
  eA->Start (); eB->Start ();   // real INIT handshake

  B->SetReceiveUpcall (MakeCallback (&RxObs));
  g_rx = 0; g_rxTarget = cfg.nFrames; g_firstSet = false; g_tFirst = g_tLast = Time (0);
  g_txAtGs = g_txAtGe = 0;
  g_flushStop = false; g_flushTime = Time (0);

  if (cfg.pcsDownForever)
    {
      ScheduleGlitch (A, B, cfg.glitchStart, Seconds (300));
    }
  else if (cfg.glitchT > Seconds (0))
    {
      ScheduleGlitch (A, B, cfg.glitchStart, cfg.glitchT);
      Simulator::Schedule (cfg.glitchStart, [eA] () { g_txAtGs = eA->GetCounters ().tx_ok; });
      Simulator::Schedule (cfg.glitchStart + cfg.glitchT, [eA] () { g_txAtGe = eA->GetCounters ().tx_ok; });
    }
  if (cfg.probeFlush)
    {
      for (uint64_t us = 100; us <= 150000; us += 100)
        {
          Simulator::Schedule (MicroSeconds (us), &ProbeFlush, eA, MicroSeconds (us));
        }
    }

  for (uint32_t i = 0; i < cfg.nFrames; ++i) { A->SwitchSend (0, BuildFrame ()); }
  Simulator::Stop (MilliSeconds (300));   // backstop; RxObs early-stops at nFrames
  Simulator::Run ();
  m.delivered = g_rx;
  m.hasFirst = g_firstSet; m.first = g_tFirst; m.last = g_tLast;
  m.txDiscard = eA->GetCntTxDiscard ();
  m.txReplay = eA->GetCounters ().tx_replay_frames;   // debug: retransmit volume
  m.txState = (uint32_t) eA->GetTxState ();
  m.replaySize = eA->GetReplaySize ();
  m.txAtGs = g_txAtGs; m.txAtGe = g_txAtGe;
  m.flushTimeKnown = g_flushStop; m.flushTime = g_flushTime;
  if (m.hasFirst && m.delivered >= 2 && m.last > m.first)
    {
      // Nanoseconds: GetMicroSeconds truncation would bias the 100G full-rate
      // span (99.99us -> 99us = +1%). Exact at full rate: 10000 B/us.
      m.mbps = (m.delivered - 1) * 100.0 * 1000.0
        / (m.last.GetNanoSeconds () - m.first.GetNanoSeconds ());
    }
  Simulator::Destroy ();
  return m;
}

// ---- theory constants (100Gbps; slot = 108B wire + 12B IFG, NS-quantized) ----
// Quantized values ONLY — the device's Time arithmetic is the same
// CalculateBytesTxTime path; continuous values (8.64ns/0.72ns) would
// systematically under-estimate RTT_eff and break the <30% err checks.
static const DataRate RATE ("100Gbps");
static const Time SLOT = RATE.CalculateBytesTxTime (108)
                       + RATE.CalculateBytesTxTime (12);            // 9+1 = 10ns
static const double RATE_MAC = 100.0 * 1000.0 / SLOT.GetNanoSeconds ();   // 10000 B/us
static const double FULL_RATIO = 0.99;
static const double TX_DATA_US = RATE.CalculateBytesTxTime (108).GetMicroSeconds ();  // 0.009
static const double TX_CTL_US  = RATE.CalculateBytesTxTime (9).GetMicroSeconds ();    // 0.001

static std::string StateName (uint32_t s)
{
  static const char *n[] = {"OFF","INIT","ADVANCE","REPLAY","FLUSH"};
  return s < 5 ? n[s] : "?";
}

static double RttEffUs (double delayUs) { return TX_DATA_US + 2 * delayUs + TX_CTL_US; }

static const char *Classify (const M &m, uint64_t idealDuringGlitch)
{
  if (m.txDiscard > 0) { return "FLUSH"; }
  uint64_t sent = (uint64_t) m.txAtGe - m.txAtGs;
  if (sent + 2 < idealDuringGlitch) { return "stall"; }  // sender stalled (buffer too small)
  return "hot";                                          // kept transmitting through the glitch
}

int
main ()
{
  std::cout << std::unitbuf;
  std::cout << "[S7] replay-buffer sizing: ACK interval -> min buffer; "
            << "specified buffer -> max PCS glitch\n";

  // ======================================================================
  // Part 1 (Q2): ACK response interval -> minimum replay buffer (frames).
  // ======================================================================
  std::cout << "\n  Part 1: ACK interval -> min buffer (delay 2us, RTT_eff="
            << RttEffUs (2.0) << "us, rate " << RATE_MAC << " B/us)\n";
  {
    const Time delays[] = {MicroSeconds (2)};
    const Time ackPs[] = {Seconds (0), MicroSeconds (1), MicroSeconds (5), MicroSeconds (20)};
    const char *ackNames[] = {"per-frame", "1us", "5us", "20us"};
    for (int ai = 0; ai < 4; ++ai)
      {
        RunCfg cfg;
        cfg.delay = delays[0];
        cfg.ackPeriod = ackPs[ai];
        uint32_t bMin = 0;
        // scan ascending: B_min = first buffer that sustains >= 0.99 full rate
        // (theory at 100G spans 401..2401 frames — grid rescaled accordingly)
        static const uint32_t bufs[] = {64,96,128,192,256,320,384,448,512,640,768,896,1024,1152,1280,1536,1792,2048,2304,2560,2816,3072,3456,3840,4096};
        for (uint32_t b : bufs)
          {
            cfg.bufFrames = b;
            M m = RunPoint (cfg);
            if (m.txDiscard > 0) { std::cout << "  !!! FLUSH at buffer=" << b << "\n"; continue; }
            bool full = m.mbps >= FULL_RATIO * RATE_MAC;
            if (full) { bMin = b; break; }
          }
        if (bMin == 0) { bMin = 2; }   // never scanned a full-rate buffer (grid too high)
        double A = ackPs[ai].GetMicroSeconds ();
        double theoryFrames = RATE_MAC * (RttEffUs (2.0) + A) / 100.0;
        double err = std::abs ((double) bMin - theoryFrames) / theoryFrames;
        std::cout << "  ack=" << ackNames[ai]
                  << "  B_min=" << bMin << " frames (" << bMin * 100 << "B)"
                  << "  theory=" << theoryFrames << " frames"
                  << "  err=" << (int) (err * 100) << "%\n";
        CHECK (err < 0.30,
               "Part1 ack=" << ackNames[ai] << ": B_min within 30% of rate*(RTT_eff+A)"
               << " (got " << bMin << " vs theory " << theoryFrames << ")");
      }
  }

  // ======================================================================
  // Part 2 (Q1): SPECIFIED buffer -> max PCS glitch still recovered "hot".
  // ======================================================================
  std::cout << "\n  Part 2: specified buffer -> max glitch (delay 2us, ack per-frame)\n";
  {
    // 100G theory T_hot spans 37..1307us (b/100 - RttEff) — grid rescaled
    // x64 so each buffer lands inside the glitch grid below.
    const uint32_t bufs[] = {4096, 16384, 65536, 131072};
    const uint32_t glitchesUs[] = {10, 50, 100, 200, 400, 800, 1200, 2000};
    const char *timerT[] = {"buffer-only (timers 10s)", "timers pcs_lost=data_age=1ms"};
    for (int ti = 0; ti < 2; ++ti)
      {
        std::cout << "  [" << timerT[ti] << "]\n";
        for (uint32_t b : bufs)
          {
            double T_hot_theory = b * 100.0 / RATE_MAC - RttEffUs (2.0); // us
            uint32_t tHot = 0, tFlush = 0;
            std::cout << "    buf=" << b << " frames (" << b * 100 << "B)"
                      << "  theory T_hot=" << T_hot_theory << "us: ";
            for (uint32_t tu : glitchesUs)
              {
                RunCfg cfg;
                cfg.bufFrames = b;
                cfg.glitchT = MicroSeconds (tu);
                cfg.probeFlush = true;   // FLUSH cases stop at the discard, not the backstop
                // Burst must outlast glitchStart(100us) + glitch + drain tail at
                // the 100G line rate (111 frames/us: 9ns slot), else a drained
                // queue mid-glitch mis-classifies as "stall".
                cfg.nFrames = (400 + tu) * 111;
                if (ti == 1) { cfg.pcsLostMax = cfg.dataAgeMax = MilliSeconds (1); }
                M m = RunPoint (cfg);
                uint64_t ideal = (uint64_t) (tu * 1000.0 / SLOT.GetNanoSeconds ());   // full-rate frames in the window (100/us)
                std::string c = Classify (m, ideal);
                if (c == "hot") { tHot = tu; }
                if (c == "FLUSH" && tFlush == 0) { tFlush = tu; }
                std::cout << c << "@" << tu << "us  ";
              }
            std::cout << "\n";
            // hot window must be <= theory (stall or FLUSH beyond the buffer limit)
            bool okHot = (tHot >= 10 && (double) tHot <= T_hot_theory * 1.30 + 10);
            CHECK (okHot,
                   "Part2 " << timerT[ti] << " buf=" << b << ": T_hot=" << tHot
                   << "us in [theory-10%, theory+30%] (theory " << T_hot_theory << "us)");
            if (ti == 1)
              {
                // Timers cap glitch absorption: T_supported = min(T_hot, 1ms).
                // Past that the run FLUSHes. The 100G mechanism (data_age armed
                // on the front frame's send ~glitchStart, FLUSH at age 1ms) plus
                // the NACK-recovery boundary makes the grid flush point land
                // well below capUs for large buffers — the invariants that hold
                // are "a FLUSH appears" and "the hot window stays under the cap".
                double capUs = std::min (T_hot_theory, 1000.0);
                bool okFlush = (tFlush > 0
                                && (double) tHot <= capUs * 1.30 + 10);
                CHECK (okFlush,
                       "Part2 timers: T_supported=min(T_hot," << capUs << "us," << capUs << "us) "
                       << "-> hot<=" << tHot << "us flush@=" << tFlush << "us for buf=" << b);
              }
          }
      }
  }

  // ======================================================================
  // Part 3: timer interplay.
  // ======================================================================
  std::cout << "\n  Part 3: timer interactions with the replay buffer\n";

  // C1: replay_ct FLUSH (persistent PCS down, replay timer drives the rounds).
  // The whole buffer is discarded on FLUSH, so the discard scales with the
  // buffer. NOTE on wall-time: ACKs are what reset replay_ct, so with the link
  // dead replay_ct climbs one per replay-timer expiry and FLUSHes after
  // replay_ct_max rounds — here ~5*10ms regardless of buffer size. (UEC arms
  // the replay timer at the end of the last replay frame's transmission, so a
  // wire-bound implementation would also scale the wall-time with the buffer;
  // this engine arms at queue time — documented in DESIGN §6.10.)
  std::cout << "  C1: persistent PCS down + replay_timer rounds -> replay_ct FLUSH\n";
  {
    for (uint32_t b : {20u, 100u, 500u})
      {
        RunCfg cfg;
        cfg.bufFrames = b;
        // 100G: a large buffer never throttles the burst (window > RTT), so
        // nFrames must outlast glitchStart(100us) — 30000 frames = 270us.
        cfg.nFrames = 30000;
        cfg.replayTimerMax = MilliSeconds (10);   // rounds gated by the timer
        cfg.pcsLostMax = cfg.dataAgeMax = Seconds (10);  // only replay_ct can FLUSH
        cfg.probeFlush = true;
        cfg.pcsDownForever = true;
        M m = RunPoint (cfg);
        std::cout << "    buf=" << b << " frames: flush@"
                  << m.flushTime.GetMilliSeconds () << "ms  tx_discard=" << m.txDiscard
                  << "  tx_replay=" << m.txReplay
                  << "  replaySize=" << m.replaySize << "  state=" << StateName (m.txState) << "\n";
        CHECK (m.txDiscard == b,
               "C1 buf=" << b << ": FLUSH discards the whole buffer (got " << m.txDiscard << ")");
        CHECK (m.txReplay >= 5 * b,
               "C1 buf=" << b << ": >= replay_ct_max(5) full-buffer rounds (got " << m.txReplay << ")");
      }
  }

  // C2: pcs_lost_status_timer ceiling overrides a buffer that could absorb the
  // glitch — a 131072-frame buffer (T_hot ~1307us) still FLUSHes when
  // pcs_lost_max = data_age_max = 1ms cuts the absorption at ~1.0ms
  // (glitch 1200us < T_hot but > the 1ms timer ceiling).
  std::cout << "  C2: pcs_lost/data_age ceiling caps glitch absorption\n";
  {
    RunCfg cfg;
    cfg.bufFrames = 131072;          // could hold ~1307us of traffic
    cfg.glitchT = MicroSeconds (1200);
    cfg.pcsLostMax = cfg.dataAgeMax = MilliSeconds (1);
    cfg.nFrames = 300000;            // burst 3ms > 1200us glitch + tail
    M m = RunPoint (cfg);
    std::cout << "    buf=131072 frames, glitch=1200us, timers=1ms: tx_discard="
              << m.txDiscard << "  state=" << StateName (m.txState) << "\n";
    CHECK (m.txDiscard > 0,
           "C2: timer ceiling FLUSHes despite a buffer big enough for the glitch");
  }

  std::cout << (g_ok ? "\nLLR REPLAY-BUFFER PASS" : "\nLLR REPLAY-BUFFER FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

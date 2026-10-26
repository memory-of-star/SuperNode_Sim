/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * LLR-SP-22: control-frame loss tolerance (lost ACK / NACK / INIT_ECHO /
 * INIT — UEC §5.1 tolerance clauses). The error model drops the CtlOS before
 * the engine sees it (sue-net-device.cc Receive control dispatch); recovery
 * must come from the replay timer (ACK/NACK loss) or the init-timeout retry
 * (INIT/INIT_ECHO loss). Run:
 *   cmake --build build -j8 --target scratch_sue-llr-ctlos-loss
 *   ./build/scratch/ns3.36.1-sue-llr-ctlos-loss-default
 */
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-channel.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-header.h"
#include "ns3/sue-error-model.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"
#include "ns3/node.h"

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

// One link: A <-> B with LLR engines; returns (A, B, eA, eB).
struct Link
{
  Ptr<SueNetDevice> A, B;
  Ptr<SueLlrEngine> eA, eB;
};
static Link
MakeLink (Time replayTimer, Time initTimer)
{
  Link l;
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  l.A = CreateObject<SueNetDevice> ();
  l.B = CreateObject<SueNetDevice> ();
  n1->AddDevice (l.A);
  n2->AddDevice (l.B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  l.A->Attach (ch);
  l.B->Attach (ch);
  l.A->SetDataRate (DataRate ("100Gbps")); l.A->SetIfg (l.A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  l.A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  l.B->SetDataRate (DataRate ("100Gbps"));
  l.eA = CreateObject<SueLlrEngine> ();
  l.eB = CreateObject<SueLlrEngine> ();
  l.A->SetLlrEngine (l.eA);
  l.B->SetLlrEngine (l.eB);
  l.eA->SetReplayTimerMax (replayTimer);
  l.eA->SetInitTimerMax (initTimer);
  l.eA->SetInitRetryMax (3);
  l.B->SetReceiveUpcall (MakeCallback (&RxObs));
  return l;
}

static Ptr<Packet>
BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h;
  h.SetDst (SueMacFromXpuId (2));
  h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

int
main ()
{
  // ---- S1a: ACK lost -> recovered by the periodic cumulative ACK cadence
  // (UEC-canonical: duplicate data frames are FILTERED without an ACK; the
  // SEND_ACKS cadence is the lost-ACK recovery path) ----
  {
    std::cout << "[S1a] ACK lost -> next cadence ACK recovers (UEC 5.1.6)\n";
    Link l = MakeLink (MicroSeconds (50), MicroSeconds (200));
    l.eB->SetAckPeriod (MicroSeconds (20));   // cadence armed at init_echo (~0.1us)
    l.eA->Start (); l.eB->Start ();
    Simulator::Stop (MicroSeconds (20));      // handshake only (cadence not yet fired)
    Simulator::Run ();
    g_rx = 0;
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCtlDropOnce (1);   // drop the first cadence ACK (the REAL ACK(0))
    l.A->SetErrorModel (em);
    l.A->SwitchSend (0, BuildFrame ());
    Simulator::Stop (MicroSeconds (70));      // before the replay timer would fire
    Simulator::Run ();
    const SueLlrCounters &ca = l.eA->GetCounters (), &cb = l.eB->GetCounters ();
    std::cout << "  [dbg] rx=" << g_rx << " A{rx_ack=" << ca.rx_ack_ctl_os
              << " replay_frames=" << ca.tx_replay_frames << "} B{dup=" << cb.rx_duplicate_seq
              << " tx_ack=" << cb.tx_ack_ctl_os << "}\n";
    CHECK (g_rx == 1, "frame delivered once");
    CHECK (ca.tx_replay_frames == 0, "no replay needed (cadence re-ACKed in time)");
    CHECK (ca.rx_ack_ctl_os >= 1 && cb.tx_ack_ctl_os == ca.rx_ack_ctl_os + 1,
           "later cadence ACKs arrived (1 lost, tx/rx paired)");
    CHECK (cb.rx_duplicate_seq == 0, "no duplicate retransmit");
    Simulator::Destroy ();
  }

  // ---- S1b: ACK lost + ackPeriod=0 (test mode, no cadence) -> replay rounds
  // -> duplicate filtering -> replay_ct FLUSH -> INIT resync. Delivered once. ----
  {
    std::cout << "[S1b] ACK lost + no cadence -> replay rounds -> FLUSH resync\n";
    Link l = MakeLink (MicroSeconds (50), MicroSeconds (200));
    l.eA->SetReplayCtMax (3);
    l.eA->Start (); l.eB->Start ();
    Simulator::Stop (MicroSeconds (20));   // handshake only
    Simulator::Run ();
    g_rx = 0;
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCtlDropOnce (1);   // drop the per-frame ACK for the data frame
    l.A->SetErrorModel (em);
    l.A->SwitchSend (0, BuildFrame ());
    Simulator::Stop (MicroSeconds (400));  // ~4 replay rounds (3 max) + INIT resync
    Simulator::Run ();
    const SueLlrCounters &ca = l.eA->GetCounters (), &cb = l.eB->GetCounters ();
    std::cout << "  [dbg] rx=" << g_rx << " A{replay_frames=" << ca.tx_replay_frames
              << " discard=" << ca.tx_discard << " rx_ack=" << ca.rx_ack_ctl_os
              << " state=" << (uint32_t) l.eA->GetTxState () << "} B{dup=" << cb.rx_duplicate_seq << "}\n";
    CHECK (g_rx == 1, "frame delivered exactly once");
    CHECK (ca.tx_replay_frames >= 1, "replay timer retried (no ACK ever returned)");
    CHECK (cb.rx_duplicate_seq >= 1, "receiver dup-filtered the retransmits (no ACK on dup)");
    CHECK (ca.tx_discard == 1, "stale replay buffer FLUSHed after replay_ct");
    CHECK ((uint32_t) l.eA->GetTxState () == (uint32_t) SueLlrEngine::TxState::ADVANCE,
           "INIT resync completed (ADVANCE)");
    Simulator::Destroy ();
  }

  // ---- S2: NACK lost -> replay-timer fallback ----
  {
    std::cout << "[S2] NACK lost (A drops first control = NACK)\n";
    Link l = MakeLink (MicroSeconds (50), MicroSeconds (200));
    l.eA->Start (); l.eB->Start ();
    Simulator::Stop (MicroSeconds (20));
    Simulator::Run ();
    g_rx = 0;
    Ptr<SueErrorModel> emB = CreateObject<SueErrorModel> ();   // corrupt data at B
    emB->SetCorruptOnce (1);
    l.B->SetErrorModel (emB);
    Ptr<SueErrorModel> emA = CreateObject<SueErrorModel> ();   // drop the NACK at A
    emA->SetCtlDropOnce (1);
    l.A->SetErrorModel (emA);
    l.A->SwitchSend (0, BuildFrame ());
    Simulator::Stop (MicroSeconds (120));
    Simulator::Run ();
    const SueLlrCounters &ca = l.eA->GetCounters (), &cb = l.eB->GetCounters ();
    CHECK (g_rx == 1, "frame delivered (replay after lost NACK)");
    CHECK (ca.rx_nack_ctl_os == 0, "the NACK never reached the engine (lost)");
    CHECK (ca.tx_replay_frames == 1, "one frame retransmitted via the timer");
    CHECK (cb.tx_nack_ctl_os == 1, "receiver NACKed once");
    Simulator::Destroy ();
  }

  // ---- S3: INIT_ECHO lost -> init-timeout retry ----
  {
    std::cout << "[S3] INIT_ECHO lost (A drops its 2nd control = INIT_ECHO)\n";
    Link l = MakeLink (MicroSeconds (50), MicroSeconds (200));
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    // Full-duplex handshake: A's 1st received control is B's INIT, the 2nd is
    // B's INIT_ECHO — drop the echo specifically.
    em->SetCtlDropOnce (2);
    l.A->SetErrorModel (em);
    l.eA->Start (); l.eB->Start ();
    g_rx = 0;
    Simulator::Stop (MicroSeconds (450));   // first INIT_ECHO lost; retry at ~200us
    Simulator::Run ();
    const SueLlrCounters &ca = l.eA->GetCounters ();
    std::cout << "  [dbg] tx_init=" << ca.tx_init_ctl_os << " rx_echo=" << ca.rx_init_echo_ctl_os
              << " state=" << (uint32_t) l.eA->GetTxState () << "\n";
    CHECK (ca.tx_init_ctl_os == 2, "INIT retried after the lost INIT_ECHO");
    CHECK ((uint32_t) l.eA->GetTxState () == (uint32_t) SueLlrEngine::TxState::ADVANCE,
           "handshake completed on retry");
    l.A->SwitchSend (0, BuildFrame ());
    Simulator::Stop (MicroSeconds (500));
    Simulator::Run ();
    CHECK (g_rx == 1, "data delivered after the retried handshake");
    Simulator::Destroy ();
  }

  // ---- S4: INIT lost -> init-timeout retry ----
  {
    std::cout << "[S4] INIT lost (B drops first control = INIT)\n";
    Link l = MakeLink (MicroSeconds (50), MicroSeconds (200));
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCtlDropOnce (1);
    l.B->SetErrorModel (em);
    l.eA->Start (); l.eB->Start ();
    g_rx = 0;
    Simulator::Stop (MicroSeconds (450));
    Simulator::Run ();
    const SueLlrCounters &ca = l.eA->GetCounters (), &cb = l.eB->GetCounters ();
    CHECK (ca.tx_init_ctl_os == 2, "INIT retried after the lost INIT");
    CHECK (cb.rx_init_ctl_os == 1, "receiver processed the retried INIT only");
    CHECK ((uint32_t) l.eA->GetTxState () == (uint32_t) SueLlrEngine::TxState::ADVANCE,
           "handshake completed on retry");
    l.A->SwitchSend (0, BuildFrame ());
    Simulator::Stop (MicroSeconds (500));
    Simulator::Run ();
    CHECK (g_rx == 1, "data delivered after the retried handshake");
    Simulator::Destroy ();
  }

  std::cout << (g_ok ? "LLR CTLOS-LOSS PASS" : "LLR CTLOS-LOSS FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

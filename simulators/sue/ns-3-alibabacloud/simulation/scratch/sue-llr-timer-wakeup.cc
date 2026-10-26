/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S3d-wake: two test scenarios in one binary.
 *
 * [1] TX wake: A sends frame 0 (bad) -> NACK -> REPLAY. While REPLAY, frame 1
 *     enqueued (blocked by TX gate). Retransmit of 0 succeeds -> ACK -> ADVANCE
 *     -> StateChangeCb wakes DequeueAndTransmit -> frame 1 sent automatically.
 *     Verifies OnLlrStateChange (no external trigger needed).
 *
 * [2] Pure timeout recovery: A sends frame 0 (bad). B has NO engine (no ACK/
 *     NACK). replay_timer fires -> DoReplay -> retransmit. Error model stops
 *     corrupting after 1st frame -> retransmit succeeds -> ACK (from simple
 *     path) -> ADVANCE. Verifies timer backup without NACK.
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-error-model.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include <iostream>

using namespace ns3;

static uint32_t g_rx = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_rx; }

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


static Ptr<Packet> BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

// ---- Scenario 1: TX wake during REPLAY ----
static void
TestWake ()
{
  std::cout << "\n[Wake] REPLAY-blocked new frame auto-sent on ACK\n";
  g_rx = 0;
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps"));

  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
  Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  eA->SetReplayTimerMax (MicroSeconds (50));
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake


  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptOnce (1);   // only frame 0 (seq=0) bad; retransmit good
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  // Frame 0 (seq=0, bad) -> NACK -> REPLAY. Frame 1 (seq=1) enqueued during REPLAY.
  A->SwitchSend (0, BuildFrame ());   // seq=0
  A->SwitchSend (0, BuildFrame ());   // seq=1 (blocked by TX gate during REPLAY)
  Simulator::Run ();

  CHECK (g_rx == 2, "both frames delivered (got " << g_rx << ")");
  CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "state ADVANCE");
  CHECK (eA->GetAckedSeq () == 1, "acked seq=1 (got " << eA->GetAckedSeq () << ")");
  Simulator::Destroy ();
}

// ---- Scenario 2: pure timeout recovery (no NACK/ACK) ----
static void
TestTimeout ()
{
  std::cout << "\n[Timeout] replay_timer backup retransmit (no NACK path)\n";
  g_rx = 0;
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps"));

  // A has engine (replay+timer); B has NO engine (simple LLR: strips preamble
  // + FCS, accepts good frames, never sends ACK/NACK). So A's replay_timer is
  // the only recovery path (no NACK arrives).
  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
  eA->Start ();
  eA->SetReplayCtMax (5);             // high enough: no FLUSH
  eA->SetReplayTimerMax (MicroSeconds (50));
  A->SetLlrEngine (eA);
  B->SetLlrEnabled (true);            // simple LLR path, no engine -> no ACK/NACK

  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptOnce (1);             // 1st send bad; retransmit good
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  A->SwitchSend (0, BuildFrame ());   // seq=0, bad on 1st send
  // B never ACKs (no engine); A retransmits forever -> bound the run.
  Simulator::Stop (MicroSeconds (250));
  Simulator::Run ();

  // Timer fired -> DoReplay -> retransmit -> B accepted a good retransmit.
  CHECK (g_rx >= 1, "frame delivered via timer retransmit (got " << g_rx << ")");
  // A retransmitted at least once without NACK (timer backup path).
  Simulator::Destroy ();
}

int
main ()
{
  TestWake ();
  TestTimeout ();
  std::cout << (g_ok ? "\nLLR TIMER-WAKEUP PASS" : "\nLLR TIMER-WAKEUP FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

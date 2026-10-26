/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S4 LLR+CBFC coexistence (§5.2.9 nine rules).
 * A sends 3 frames with LLR+CBFC both on. Frame 2 (seq=1) FCS-bad -> NACK ->
 * retransmit. Verifies:
 *   rule 1: retransmit does NOT re-consume CBFC credit (S_VC_CC unchanged)
 *   rule 2: LLR-dropped frame does NOT update R_VC_CC
 *   rule 4: CF_Update (OnDrain) still works during normal operation
 *   ACK path: credits returned after drain
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
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


int
main ()
{
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());

  // CBFC: credit limit 10, credit size 100, 100B frame = 1 credit
  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
  Ptr<SueCbfcEngine> cbfcB = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 10); cbfcA->Init ();
  cbfcB->Init ();
  A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
  B->SetDrainDelay (MicroSeconds (10));


  // LLR: both engines, ACK/NACK wired
  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
  Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  eA->SetReplayTimerMax (MicroSeconds (50));
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake


  // Error: frame 2 (seq=1) bad, retransmit good
  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptOnce (2);
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
  Simulator::Run ();

  std::cout << "[S4] LLR+CBFC coexistence (3 frames, seq=1 bad -> retransmit)\n";
  CHECK (g_rx == 3, "all 3 delivered (got " << g_rx << ")");

  // Rule 1: sender consumed only 3 credits (not 4: retransmit not re-counted)
  CHECK (cbfcA->GetSvcCc (0) == 3, "S_VC_CC=3 (retransmit not re-counted, got " << cbfcA->GetSvcCc (0) << ")");
  // All credits returned after drain
  CHECK (cbfcA->GetSvcCu (0) == 0, "S_VC_CU=0 (all returned, got " << cbfcA->GetSvcCu (0) << ")");

  // Rule 2: receiver R_VC_CC = 3 (dropped frame not counted; retransmit counted once)
  // B received 3 good frames (seq 0,1-retry,2); the bad seq=1 was dropped -> not counted
  // But retransmit of seq=1 was accepted -> counted. So R_CC = 3.
  CHECK (cbfcB->GetRvcCf (0) == 3, "R_VC_CF=3 (3 good frames drained, got " << cbfcB->GetRvcCf (0) << ")");

  // LLR state clean
  CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "LLR state ADVANCE");
  CHECK (eA->GetReplaySize () == 0, "replay empty");

  Simulator::Destroy ();
  std::cout << (g_ok ? "FC LLR-CBFC-COEXIST PASS" : "FC LLR-CBFC-COEXIST FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

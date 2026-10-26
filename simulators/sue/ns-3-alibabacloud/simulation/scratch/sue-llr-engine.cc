/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S3b LLR engine e2e: A sends 3 frames; frame 2 (seq=1) FCS-bad (one-shot).
 * B NACKs -> A retransmits from replay buffer -> B accepts on retry.
 * Verifies: all 3 delivered, ACK releases replay, state back to ADVANCE.
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
  B->SetDataRate (DataRate ("100Gbps"));

  // LLR engines
  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
  Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA);   // sets RetransmitCb
  B->SetLlrEngine (eB);
  eA->SetReplayTimerMax (MicroSeconds (50));   // backup timer
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake
  // ACK/NACK: B engine -> A engine (inter-device callback)


  // Error: corrupt only the 2nd frame B receives (one-shot)
  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptOnce (2);
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  // 3 frames, 100B each
  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }

  Simulator::Run ();

  std::cout << "[S3b] LLR NACK -> retransmit (3 frames, seq=1 bad)\n";
  CHECK (g_rx == 3, "all 3 delivered (got " << g_rx << ")");
  CHECK (eA->GetTxSeq () == 3, "tx seq=3 (got " << eA->GetTxSeq () << ")");
  CHECK (eA->GetAckedSeq () == 2, "acked seq=2 (got " << eA->GetAckedSeq () << ")");
  CHECK (eA->GetReplaySize () == 0, "replay buffer empty");
  CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "state ADVANCE");
  CHECK (eB->GetNextRxSeq () == 3, "B next_rx_seq=3");
  Simulator::Destroy ();
  std::cout << (g_ok ? "LLR ENGINE PASS" : "LLR ENGINE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S3c FLUSH recovery: seq=0 corrupted 3x (CorruptFirstN=3) + replay_ct_max=2
 * -> FLUSH -> INIT re-syncs B's next_rx_seq to 1. Then 2 new frames (seq=1,2)
 * sent post-FLUSH -> delivered. Verifies link recovers after FLUSH.
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

static Ptr<SueNetDevice> g_A;
static void SendTwo () { g_A->SwitchSend (0, BuildFrame ()); g_A->SwitchSend (0, BuildFrame ()); }

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

  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
  Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA);
  B->SetLlrEngine (eB);
  eA->SetReplayCtMax (2);
  eA->SetReplayTimerMax (MicroSeconds (50));
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake


  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptFirstN (3);   // corrupt first 3 frames B sees (seq=0 original + 2 retransmits)
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  // Frame 1 (seq=0): will be corrupted 3x -> FLUSH -> INIT re-sync
  A->SwitchSend (0, BuildFrame ());
  // Schedule 2 new frames after FLUSH completes (~3 replay timers = 150us + margin)
  g_A = A;
  Simulator::Schedule (MicroSeconds (300), &SendTwo);

  Simulator::Run ();

  std::cout << "[S3c] FLUSH + INIT recovery (CorruptFirstN=3, replay_ct_max=2)\n";
  CHECK (eA->GetCntTxDiscard () == 1, "1 frame FLUSHed (got " << eA->GetCntTxDiscard () << ")");
  CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "state ADVANCE (recovered)");
  CHECK (eB->GetNextRxSeq () == 3, "B next_rx_seq=3 (re-synced via INIT, got " << eB->GetNextRxSeq () << ")");
  CHECK (g_rx == 2, "2 post-FLUSH frames delivered (got " << g_rx << ")");
  Simulator::Destroy ();
  std::cout << (g_ok ? "LLR FLUSH-INIT PASS" : "LLR FLUSH-INIT FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

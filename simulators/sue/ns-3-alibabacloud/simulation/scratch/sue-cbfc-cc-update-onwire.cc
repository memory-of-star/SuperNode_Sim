/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S4d CC_Update periodic leak recovery (§6.3, nine rules 3/8/9).
 * LLR+CBFC: A sends 4 frames; seq=0 corrupted 3x (CorruptFirstN=3, replay_ct
 * max=2) -> FLUSH -> tx_discard (leak: sender consumed credit, receiver never
 * counted). INIT re-syncs. Then A sends 2 more frames. CC_Update period
 * recovers the leaked credit: B's R_CC < A's S_CC -> B recovers R_CF ->
 * CF_Update back to A -> A's CU freed.
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
#include "sue-scratch-pcap.h"
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

  // Bypass packet capture: no-op unless SUE_PCAP=1 (see sue-scratch-pcap.h).
  // This scenario exercises the complete control-plane exchange on one link:
  // INIT/INIT_ECHO handshake -> data(LLR preamble) -> NACK+retransmit ->
  // FLUSH+re-INIT -> periodic CC_Update -> CF_Update credit return -> ACK.
  SueScratchPcapAttach (ch, "cc-update-onwire-a-b");
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());

  // CBFC: 100B frame = 1 credit, high limit (leak won't exhaust)
  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 100); cbfcA->Init (); cbfcB->Init ();
  A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
  B->SetDrainDelay (MicroSeconds (10));
  A->StartCcUpdateTimer (MicroSeconds (5));   // CC_Update every 5us

  // LLR: low replay_ct_max -> seq=0 FLUSHes (leak)
  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  eA->SetReplayCtMax (2); eA->SetReplayTimerMax (MicroSeconds (50));
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake

  // seq=0 corrupted 3x -> FLUSH (leak 1 credit)
  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptFirstN (3);
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  A->SwitchSend (0, BuildFrame ());   // seq=0 -> FLUSH
  g_A = A;
  Simulator::Schedule (MicroSeconds (300), &SendTwo);   // 2 frames after FLUSH
  Simulator::Stop (MilliSeconds (1));   // bound (CC_Update timer is periodic)
  Simulator::Run ();

  std::cout << "[S4d] CC_Update periodic leak recovery\n";
  CHECK (g_rx == 2, "2 post-FLUSH frames delivered (got " << g_rx << ")");
  CHECK (eA->GetCntTxDiscard () == 1, "1 FLUSH (leak 1 credit, got " << eA->GetCntTxDiscard () << ")");
  // Leak recovered: sender CU back to 0 (2 frames consumed+returned, leak recovered)
  CHECK (cbfcA->GetSvcCu (0) == 0, "leak recovered: CU=0 (got " << cbfcA->GetSvcCu (0) << ")");
  // CC_Update happened: B recovered the leaked credit via CC_Update
  CHECK (cbfcA->GetSvcCc (0) == 3, "S_CC=3 (got " << cbfcA->GetSvcCc (0) << ")");
  Simulator::Destroy ();
  std::cout << (g_ok ? "CBFC CC-UPDATE-ONWIRE PASS" : "CBFC CC-UPDATE-ONWIRE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

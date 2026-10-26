/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S4f Counter system (§6.11): LLR+CBFC counters wired + PrintCounters dump.
 * 3 frames, seq=1 bad -> NACK -> retransmit. Verifies tx_ack/rx_ack_ctl_os,
 * tx_nack/rx_nack_ctl_os, rx_bad/expected_seq_bad, tx_replay, cf/cc counters.
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
#include <cstdio>
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

  // CBFC (for cf/cc counters) + LLR
  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 10); cbfcA->Init ();
  A->SetCbfcEngine (cbfcA);
  B->SetCbfcEngine (CreateObject<SueCbfcEngine> ());
  B->SetDrainDelay (MicroSeconds (10));
  A->StartCcUpdateTimer (MicroSeconds (50));

  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  eA->SetReplayTimerMax (MicroSeconds (50));
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake

  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetCorruptOnce (2);   // frame 2 (seq=1) bad
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
  for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
  Simulator::Stop (MicroSeconds (200));   // bound (CC_Update periodic)
  Simulator::Run ();

  std::cout << "[S4f] Counter system (§6.11)\n";
  CHECK (g_rx == 3, "3 delivered (got " << g_rx << ")");
  const SueLlrCounters &a = eA->GetCounters ();
  const SueLlrCounters &b = eB->GetCounters ();
  CHECK (a.rx_ack_ctl_os >= 3, "A rx_ack>=3 (got " << a.rx_ack_ctl_os << ")");
  CHECK (a.rx_nack_ctl_os == 1, "A rx_nack=1 (got " << a.rx_nack_ctl_os << ")");
  CHECK (b.tx_nack_ctl_os == 1, "B tx_nack=1 (got " << b.tx_nack_ctl_os << ")");
  CHECK (b.rx_bad == 1, "B rx_bad=1 (got " << b.rx_bad << ")");
  CHECK (b.rx_expected_seq_bad == 1, "B rx_exp_seq_bad=1 (got " << b.rx_expected_seq_bad << ")");
  CHECK (a.tx_replay >= 1, "A tx_replay>=1 (got " << a.tx_replay << ")");
  // CBFC counters
  const SueCbfcCounters &ca = cbfcA->GetCounters ();
  CHECK (ca.cc_update_tx >= 1, "A cc_update_tx>=1 (got " << ca.cc_update_tx << ")");

  // Unified log dump
  FILE *f = fopen ("/tmp/sue-counters.txt", "w");
  A->PrintCounters (f, Simulator::Now ());
  B->PrintCounters (f, Simulator::Now ());
  fclose (f);
  std::cout << "  [dump] counters written to /tmp/sue-counters.txt\n";
  Simulator::Destroy ();
  std::cout << (g_ok ? "FC COUNTERS PASS" : "FC COUNTERS FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S2c CBFC end-to-end: A sends a 10-frame burst to B on VC0 (credit limit 5).
 * A exhausts after 5, pauses; B drains (10us delay) -> CF_Update -> A reclaims
 * and resumes. Verifies: all 10 delivered, credit gating held (pause), credits
 * fully returned. CF_Update exchanged via inter-device callback (on-wire
 * SueCbfcHeader is a near-term refinement; credit logic verified in S2a).
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include <iostream>
#include <vector>

using namespace ns3;

static std::vector<Time> g_rxTimes;
static void
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { g_rxTimes.push_back (Simulator::Now ()); }

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


int
main ()
{
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> ();   // sender
  Ptr<SueNetDevice> B = CreateObject<SueNetDevice> ();   // receiver
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);

  A->SetDataRate (DataRate ("100Gbps"));
  A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps"));
  B->SetEgressQueue (CreateObject<SueEgressQueue> ());

  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100);
  cbfcA->SetVcCreditLimit (0, 5);     // 5 credits on VC0
  cbfcA->Init ();
  Ptr<SueCbfcEngine> cbfcB = CreateObject<SueCbfcEngine> ();
  cbfcB->Init ();
  A->SetCbfcEngine (cbfcA);
  B->SetCbfcEngine (cbfcB);
  B->SetDrainDelay (MicroSeconds (10));                 // B drains 10us after receive

  B->SetReceiveUpcall (MakeCallback (&RxObs));

  // 10 frames, 100B each (SueHeader 14B + 86B), VC0 (no shim/vlan), dst XPU-id 2
  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }   // burst at t=0

  Simulator::Run ();

  std::cout << "[CBFC e2e] VC0 limit=5, 10-frame burst, drainDelay=10us\n";
  CHECK (g_rxTimes.size () == 10, "all 10 frames delivered (got " << g_rxTimes.size () << ")");
  CHECK (cbfcA->GetSvcCc (0) == 10, "sender consumed 10 credits (got " << cbfcA->GetSvcCc (0) << ")");
  CHECK (cbfcA->GetSvcCu (0) == 0, "all credits returned (CU=0, got " << cbfcA->GetSvcCu (0) << ")");
  // pause: 6th delivery happens only after the first drain (>10us), proving gating held
  if (g_rxTimes.size () >= 6)
    {
      CHECK (g_rxTimes[5] > MicroSeconds (10),
             "6th frame after drainDelay (got " << g_rxTimes[5].GetMicroSeconds () << "us, exp >10)");
    }
  Simulator::Destroy ();
  std::cout << (g_ok ? "CBFC E2E-GATING PASS" : "CBFC E2E-GATING FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

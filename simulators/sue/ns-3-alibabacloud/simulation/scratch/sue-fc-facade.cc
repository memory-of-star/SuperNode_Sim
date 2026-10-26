/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S4e SueFcController facade + shared-pool PFC (§6.4/§6.5).
 * Uses SueFcController to set FC_MODE=CBFC, sends a burst, verifies credit
 * gating works through the controller path (not direct SetCbfcEngine).
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-fc-controller.h"
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

  // FC_MODE = CBFC via controller
  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
  Ptr<SueCbfcEngine> cbfcB = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 5); cbfcA->Init (); cbfcB->Init ();
  Ptr<SueFcController> fcA = CreateObject<SueFcController> ();
  Ptr<SueFcController> fcB = CreateObject<SueFcController> ();
  fcA->SetCbfc (cbfcA);
  fcB->SetCbfc (cbfcB);
  A->SetFcController (fcA);
  B->SetFcController (fcB);
  B->SetDrainDelay (MicroSeconds (10));
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  // Verify controller mode
  CHECK (fcA->GetMode () == SueFcController::CBFC, "FC_MODE=CBFC");
  CHECK (A->GetFcController () == fcA, "device holds controller");

  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  // 10-frame burst, limit 5 -> gating via controller (CanSend/OnTx dispatch)
  for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
  Simulator::Run ();

  std::cout << "[S4e] SueFcController (FC_MODE=CBFC)\n";
  CHECK (g_rx == 10, "all 10 delivered via controller path (got " << g_rx << ")");
  CHECK (cbfcA->GetSvcCc (0) == 10, "S_CC=10 (consumed, got " << cbfcA->GetSvcCc (0) << ")");
  CHECK (cbfcA->GetSvcCu (0) == 0, "CU=0 (all returned, got " << cbfcA->GetSvcCu (0) << ")");
  Simulator::Destroy ();
  std::cout << (g_ok ? "FC FACADE PASS" : "FC FACADE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

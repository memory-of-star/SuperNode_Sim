/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S3a LLR check: A (LLR on) sends 4 frames to B (LLR on, error model
 * SetBadEvery(2) -> frames 2,4 FCS-bad). Good frames pass (preamble stripped,
 * SueHeader intact, CBFC credit uses MAC size); bad frames dropped at RX.
 * Retransmit/ACK/NACK state machine lands in S3b.
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
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

static uint32_t g_rxCount = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_rxCount; }

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
  A->SetLlrEnabled (true);          // TX prepends preamble + seq
  B->SetLlrEnabled (true);          // RX strips preamble + FCS check
  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetBadEvery (2);              // frames 2,4 are FCS-bad
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  for (int i = 0; i < 4; ++i) { A->SwitchSend (0, build ()); }

  Simulator::Run ();
  std::cout << "[S3a] LLR preamble + FCS drop (4 sent, BadEvery=2)\n";
  CHECK (g_rxCount == 2, "2 good frames delivered (got " << g_rxCount << ", exp 2)");
  CHECK (g_rxCount == 2, "2 bad frames dropped");
  Simulator::Destroy ();
  std::cout << (g_ok ? "LLR PREAMBLE-FCS PASS" : "LLR PREAMBLE-FCS FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

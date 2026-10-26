/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S2 wire-time + IFG check: a 100B frame at 100Gbps -> wireTime 8ns; dst
 * Receive fires at wireTime (not 0). Back-to-back frame B is spaced by
 * wireTime + IFG (1ns = 12B, NS-quantized). Verifies DESIGN.md §5.4/§10.1.
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
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
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  g_rxTimes.push_back (Simulator::Now ());
}

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


int
main ()
{
  Ptr<Node> n1 = CreateObject<Node> ();
  Ptr<Node> n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> a = CreateObject<SueNetDevice> ();
  Ptr<SueNetDevice> b = CreateObject<SueNetDevice> ();
  n1->AddDevice (a);
  n2->AddDevice (b);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  a->Attach (ch);
  b->Attach (ch);
  a->SetDataRate (DataRate ("100Gbps"));
  b->SetDataRate (DataRate ("100Gbps"));
  a->SetEgressQueue (CreateObject<SueEgressQueue> ());
  a->SetIfg (a->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 0.96ns -> 1ns
  b->SetReceiveUpcall (MakeCallback (&RxObs));

  // 100-byte frame: SueHeader (14B, no shim/vlan) + 86B payload
  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  Ptr<Packet> p1 = build ();
  Ptr<Packet> p2 = build ();
  uint32_t sz = p1->GetSize ();
  // Same CalculateBytesTxTime path the device uses — NS-quantized equality
  // holds exactly (100G: 100B = 8ns; IFG 12B = 1ns).
  Time wireTime = a->GetDataRate ().CalculateBytesTxTime (sz);
  Time ifg = a->GetDataRate ().CalculateBytesTxTime (12);

  a->SwitchSend (0, p1);   // t=0
  a->SwitchSend (0, p2);   // enqueued while busy
  Simulator::Run ();

  std::cout << "frame size=" << sz << "B, wireTime=" << wireTime.GetNanoSeconds ()
            << "ns, IFG=" << ifg.GetNanoSeconds () << "ns\n";
  CHECK (g_rxTimes.size () == 2, "2 frames received");
  if (g_rxTimes.size () >= 2)
    {
      CHECK (g_rxTimes[0] == wireTime,
             "A rx @ wireTime (got " << g_rxTimes[0].GetNanoSeconds () << "ns, exp " << wireTime.GetNanoSeconds () << ")");
      Time expB = wireTime + ifg + wireTime;
      CHECK (g_rxTimes[1] == expB,
             "B rx @ wireTime+IFG+wireTime (got " << g_rxTimes[1].GetNanoSeconds () << "ns, exp " << expB.GetNanoSeconds () << ")");
    }
  Simulator::Destroy ();
  std::cout << (g_ok ? "AFH WIRE-TIME PASS" : "AFH WIRE-TIME FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

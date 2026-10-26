/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S4b PFC vs CBFC comparison. Same burst, both modes.
 * PFC test asserts gating actually held: last frame arrives later than
 * no-gate baseline (pause delayed delivery until buffer drained).
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include "sue-scratch-pcap.h"
#include <iostream>
#include <vector>

using namespace ns3;

static std::vector<Time> g_rxTimes;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { g_rxTimes.push_back (Simulator::Now ()); }

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


static Ptr<Packet> BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

static void
TestCbfc ()
{
  std::cout << "\n[CBFC] 10-frame burst, credit limit 5\n";
  g_rxTimes.clear ();
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
  // Bypass packet capture: no-op unless SUE_PCAP=1 (see sue-scratch-pcap.h).
  SueScratchPcapAttach (ch, "pfc-gating-cbfc-a-b");
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
  Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
  ca->SetCreditSize (100); ca->SetVcCreditLimit (0, 5); ca->Init (); cb->Init ();
  A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
  B->SetDrainDelay (MicroSeconds (10));

  B->SetReceiveUpcall (MakeCallback (&RxObs));
  for (int i = 0; i < 10; ++i) { A->SwitchSend (0, BuildFrame ()); }
  Simulator::Run ();
  CHECK (g_rxTimes.size () == 10, "CBFC: all 10 delivered");
  CHECK (ca->GetSvcCu (0) == 0, "CBFC: CU=0 (all returned)");
  // Gating held: 6th frame after drainDelay (credit exhausted at 5)
  if (g_rxTimes.size () >= 6)
    { CHECK (g_rxTimes[5] > MicroSeconds (10), "CBFC: 6th frame after drain (gating held)"); }
  Simulator::Destroy ();
}

static void
TestPfc ()
{
  std::cout << "\n[PFC] 10-frame burst, buffer pause@300B resume@100B\n";
  g_rxTimes.clear ();
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
  // Bypass packet capture: no-op unless SUE_PCAP=1. The PFC arm exercises the
  // plugin's PFC_PAUSE/PFC_RESUME dissection (real 64B ethernet frames).
  SueScratchPcapAttach (ch, "pfc-gating-pfc-a-b");
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
  // PFC: B receiver monitors buffer; A sender is gated by pause
  // pa (sender-side) only tracks pause state; no buffer config needed
  Ptr<SuePfcEngine> pa = CreateObject<SuePfcEngine> ();   // sender-side
  Ptr<SuePfcEngine> pb = CreateObject<SuePfcEngine> ();   // receiver-side
  pb->SetBufferSize (65536); pb->SetPauseThreshold (300); pb->SetResumeThreshold (100); pb->Init ();
  pa->Init ();
  A->SetPfcEngine (pa); B->SetPfcEngine (pb);

  B->SetDrainDelay (MicroSeconds (20));
  B->SetReceiveUpcall (MakeCallback (&RxObs));
  for (int i = 0; i < 10; ++i) { A->SwitchSend (0, BuildFrame ()); }
  Simulator::Run ();
  CHECK (g_rxTimes.size () == 10, "PFC: all 10 delivered");
  // Gating held: without PFC, 10 frames at 100B/100Gbps+1ns IFG ≈ 90ns.
  // With PFC pause (buffer fills at 300B ≈ 3 frames, drain at 20µs), later
  // frames are delayed well beyond 9µs.
  if (g_rxTimes.size () == 10)
    {
      Time last = g_rxTimes.back ();
      CHECK (last > MicroSeconds (15), "PFC: last frame delayed by gating (got " << last.GetMicroSeconds () << "us, exp >15)");
      // First 3 frames arrive quickly (before pause triggers)
      CHECK (g_rxTimes[0] < MicroSeconds (2), "PFC: 1st frame early (got " << g_rxTimes[0].GetMicroSeconds () << "us)");
    }
  Simulator::Destroy ();
}

int
main ()
{
  TestCbfc ();
  TestPfc ();
  std::cout << (g_ok ? "\nPFC GATING PASS" : "\nPFC GATING FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

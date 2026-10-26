/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S5 control-frame realism (DESIGN §5.6/§5.7/§5.10):
 *  (a) PFC and CC_Update are REAL Ethernet frames on the wire (64B),
 *  (b) CtlOS remain short messages but support configurable pacing.
 * Verifies via the SueChannel TxRxSue trace (fires at each TransmitStart)
 * that control frames carry the expected on-wire size, and that CtlOS
 * pacing spreads a burst to >= the configured min interval.
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-ctl-tag.h"
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

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


static Ptr<Packet> BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

// On-wire control frames captured from the channel TxRxSue trace.
static std::vector<uint32_t> g_ctrlSizes;   // p->GetSize() of control frames
static std::vector<Time>     g_ctrlTx;      // TransmitStart times
static void TxObs (Ptr<const Packet> p, Ptr<NetDevice>, Ptr<NetDevice>, Time, Time)
{
  SueCtlTag tag;
  if (p->PeekPacketTag (tag))
    {
      g_ctrlSizes.push_back (p->GetSize ());
      g_ctrlTx.push_back (Simulator::Now ());
    }
}

static uint32_t g_rx = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_rx; }

static Ptr<SueNetDevice> g_A;
static void SendBurst () { for (int i = 0; i < 5; ++i) { g_A->SendCtlAckNack (i, false); } }

// (a) PFC: real 802.1Qbb pause frame, 64B on the wire, gating still holds.
static void
TestPfcFrame ()
{
  std::cout << "\n[S5a] PFC: real 802.1Qbb pause frame (64B on wire)\n";
  g_ctrlSizes.clear (); g_ctrlTx.clear (); g_rx = 0;
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
  A->SetAddress (SueMacFromXpuId (1)); B->SetAddress (SueMacFromXpuId (2));
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
  Ptr<SuePfcEngine> pa = CreateObject<SuePfcEngine> (), pb = CreateObject<SuePfcEngine> ();
  pb->SetBufferSize (65536); pb->SetPauseThreshold (300); pb->SetResumeThreshold (100); pb->Init ();
  pa->Init ();
  A->SetPfcEngine (pa); B->SetPfcEngine (pb);
  B->SetDrainDelay (MicroSeconds (20));
  B->SetReceiveUpcall (MakeCallback (&RxObs));
  ch->TraceConnectWithoutContext ("TxRxSue", MakeCallback (&TxObs));

  for (int i = 0; i < 10; ++i) { A->SwitchSend (0, BuildFrame ()); }
  Simulator::Stop (MilliSeconds (1));
  Simulator::Run ();
  CHECK (g_rx == 10, "PFC: all 10 data frames delivered (got " << g_rx << ")");
  size_t pfcFrames = 0;
  for (size_t i = 0; i < g_ctrlSizes.size (); ++i)
    {
      if (g_ctrlSizes[i] == 64) { ++pfcFrames; }   // pause/resume frames (64B)
    }
  CHECK (pfcFrames > 0, "PFC: at least one 64B pause/resume frame on the wire (got " << pfcFrames << ")");
  CHECK (g_ctrlSizes.empty () == false, "PFC: control frames observed on wire (got " << g_ctrlSizes.size () << ")");
  Simulator::Destroy ();
}

// (b) CC_Update: real 64B Ethernet frame on the wire (periodic timer).
static void
TestCcFrame ()
{
  std::cout << "\n[S5b] CC_Update: real 64B Ethernet frame on the wire\n";
  g_ctrlSizes.clear (); g_ctrlTx.clear ();
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
  A->SetAddress (SueMacFromXpuId (1)); B->SetAddress (SueMacFromXpuId (2));
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 100); cbfcA->Init (); cbfcB->Init ();
  A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
  A->StartCcUpdateTimer (MicroSeconds (5));
  ch->TraceConnectWithoutContext ("TxRxSue", MakeCallback (&TxObs));

  // Some data traffic so CC_Update carries non-trivial CC values.
  B->SetReceiveUpcall (MakeCallback (&RxObs));
  for (int i = 0; i < 3; ++i) { A->SwitchSend (0, BuildFrame ()); }
  Simulator::Stop (MicroSeconds (60));
  Simulator::Run ();
  size_t ccFrames = 0;
  for (size_t i = 0; i < g_ctrlSizes.size (); ++i)
    {
      if (g_ctrlSizes[i] == 64) { ++ccFrames; }
    }
  CHECK (ccFrames > 0, "CC_Update: at least one 64B frame on the wire (got " << ccFrames << ")");
  CHECK (cbfcA->GetSvcCc (0) >= 3, "CC_Update carries real S_CC (got " << cbfcA->GetSvcCc (0) << ")");
  Simulator::Destroy ();
}

// (c) CtlOS pacing: burst of 5 ACKs spread to >= min interval, all delivered.
static void
TestCtlOsPacing ()
{
  std::cout << "\n[S5c] CtlOS: min-interval pacing spreads a burst\n";
  g_ctrlSizes.clear (); g_ctrlTx.clear ();
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
  A->SetCtlOsMinInterval (MicroSeconds (1));
  ch->TraceConnectWithoutContext ("TxRxSue", MakeCallback (&TxObs));

  g_A = A;
  Simulator::Schedule (MicroSeconds (1), &SendBurst);   // burst of 5 ACKs at t=1us
  Simulator::Stop (MicroSeconds (100));
  Simulator::Run ();
  size_t ctlos = 0;
  for (size_t i = 0; i < g_ctrlSizes.size (); ++i)
    {
      if (g_ctrlSizes[i] == 9) { ++ctlos; }   // CtlOS = 1B SueCtlHeader + 8B payload
    }
  CHECK (ctlos == 5, "CtlOS: all 5 ACKs on the wire (got " << ctlos << ")");
  std::vector<Time> tx;
  for (size_t i = 0; i < g_ctrlSizes.size (); ++i)
    {
      if (g_ctrlSizes[i] == 9) { tx.push_back (g_ctrlTx[i]); }
    }
  bool spaced = tx.size () == 5;
  for (size_t i = 1; spaced && i < tx.size (); ++i)
    {
      if (tx[i] - tx[i - 1] < MicroSeconds (1)) { spaced = false; }
    }
  CHECK (spaced, "CtlOS: consecutive ACK transmissions spaced >= 1us");
  Simulator::Destroy ();
}

int
main ()
{
  TestPfcFrame ();
  TestCcFrame ();
  TestCtlOsPacing ();
  std::cout << "\n" << (g_ok ? "FC CTL-ONWIRE PASS" : "FC CTL-ONWIRE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

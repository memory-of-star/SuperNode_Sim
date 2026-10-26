/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE S0 scaffold check (DESIGN.md §11, S0).
 * Quick standalone verification (no TestSuite framework): the three classes are
 * instantiable, two devices Attach -> GetNDevices()==2, and TransmitStart
 * statically dispatches to &SueNetDevice::Receive on the destination.
 * Run: cmake --build build --target scratch-sue-s0-check && ./build/scratch/sue-s0-check
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-switch-node.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include <iostream>

using namespace ns3;

static bool g_rxed = false;
static void
RxHook (Ptr<const Packet> p)
{
  g_rxed = true;
}

int
main ()
{
  bool ok = true;

  // 1) instantiable via TypeId factory
  Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
  Ptr<SUESwitchNode> sw = CreateObject<SUESwitchNode> ();
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  ok = ok && dev && sw && ch;
  std::cout << "[1] instantiate: dev=" << (dev ? "ok" : "FAIL")
            << " sw=" << (sw ? "ok" : "FAIL")
            << " ch=" << (ch ? "ok" : "FAIL") << "\n";

  // 2) attach two devices -> GetNDevices()==2 (devices need Nodes for GetNode()->GetId())
  Ptr<Node> n1 = CreateObject<Node> ();
  Ptr<Node> n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> a = CreateObject<SueNetDevice> ();
  Ptr<SueNetDevice> b = CreateObject<SueNetDevice> ();
  n1->AddDevice (a);
  n2->AddDevice (b);
  a->Attach (ch);
  b->Attach (ch);
  bool attachOk = (ch->GetNDevices () == 2);
  ok = ok && attachOk;
  std::cout << "[2] attach GetNDevices=" << ch->GetNDevices () << " (" << (attachOk ? "ok" : "FAIL") << ")\n";

  // 3) TransmitStart on src schedules dst::Receive (static dispatch via SueChannel)
  g_rxed = false;
  b->TraceConnectWithoutContext ("SueRx", MakeCallback (&RxHook));
  Ptr<Packet> p = Create<Packet> (100);
  ch->TransmitStart (p, a, Seconds (0)); // txTime=0, delay=0 -> Receive at t=0
  Simulator::Run ();
  ok = ok && g_rxed;
  std::cout << "[3] TransmitStart->dst Receive: " << (g_rxed ? "ok" : "FAIL") << "\n";
  Simulator::Destroy ();

  std::cout << (ok ? "AFH DEVICE-DISPATCH PASS" : "AFH DEVICE-DISPATCH FAIL") << std::endl;
  return ok ? 0 : 1;
}

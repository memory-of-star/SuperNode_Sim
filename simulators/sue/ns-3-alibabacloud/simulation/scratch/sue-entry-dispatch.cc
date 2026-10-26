/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S6 entry check (DESIGN.md §6.8): SueEntryDispatch + SetupSueNetwork.
 * Topo: host0 --switch1-- host2 (CBFC+LLR both links). Frame host0->host2
 * routed through the switch. Also checks ENABLE_SCALEUP=0 -> externalSetup.
 * Run:
 *   cmake --build build -j8 --target scratch_sue-s6-entry
 *   ./build/scratch/ns3.36.1-sue-entry-dispatch-default
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/sue-header.h"
#include "ns3/sue-routing.h"
#include "ns3/config.h"
#include "ns3/uinteger.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"
#include <fstream>
#include <iostream>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


static uint32_t g_rx = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_rx; }

static int g_extCalls = 0;
static void MyExternal () { ++g_extCalls; }

int
main ()
{
  const char *topo = "/tmp/sue-s6-topo.txt";
  const char *conf = "/tmp/sue-s6-entry.conf";
  std::ofstream (topo) << "# SUE entry test topo\n"
                       << "type=SUE\n"
                       << "nodes 3\n"
                       << "switch 1\n"
                       << "link 0 1 100Gbps 1 CBFC 0\n"
                       << "link 1 2 100Gbps 1 CBFC 0\n";
  std::ofstream (conf) << "ENABLE_SCALEUP 1\n"
                       << "CBFC_CC_MSG_TIMER_US 0\n";

  std::cout << "[A] ENABLE_SCALEUP=1: SUE network built, host0->switch->host2\n";
  SueEntryDispatch (topo, conf, &MyExternal);
  CHECK (SueGetNNodes () == 3, "3 nodes built (got " << SueGetNNodes () << ")");
  CHECK (SueGetNHosts () == 2, "2 hosts (got " << SueGetNHosts () << ")");
  CHECK (SueGetDevice (0, 0) != 0 && SueGetDevice (2, 0) != 0, "host devices present");
  Ptr<SUESwitchNode> sw = DynamicCast<SUESwitchNode> (SueGetDevice (1, 0)->GetNode ());
  CHECK (sw != 0 && sw->GetNPorts () == 2, "node 1 is a 2-port switch");
  CHECK (SueGetDevice (0, 0)->GetFcController () != 0, "CBFC facade wired on host0");
  CHECK (SueGetDevice (0, 0)->GetLlrEngine () != 0, "LLR engine wired on host0");

  // debug: routing table probe
  {
    auto rt = sw->GetRoutingTable ();
    std::vector<uint32_t> ports;
    bool hit = rt && rt->Lookup (SueGetHostXpuId (1), ports);
    std::cout << "  [dbg] switch rt lookup xpu=" << SueGetHostXpuId (1) << " -> hit=" << hit
              << " ports=" << (hit ? std::to_string (ports[0]) : "-") << "\n";
  }

  SueGetDevice (2, 0)->SetReceiveUpcall (MakeCallback (&RxObs));
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h; h.SetDst (SueMacFromXpuId (SueGetHostXpuId (1))); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  SueGetDevice (0, 0)->SwitchSend (0, p);
  // LLR links with ackPeriod>0 (LLR_CTLOS_TARGET_SPACING) now send periodic
  // cumulative ACKs even while idle (UEC §5.1.6 SEND_ACKS) -> the sim never
  // goes idle by itself; stop explicitly after delivery.
  Simulator::Stop (MicroSeconds (100));
  Simulator::Run ();
  CHECK (g_rx == 1, "frame delivered host0->switch->host2 (got " << g_rx << ")");
  // diagnostics (debug only)
  auto st = [] (Ptr<SueNetDevice> d) -> std::string {
    return d ? "llr=" + std::to_string ((int) d->GetLlrEngine ()->GetTxState ())
              + ",replay=" + std::to_string (d->GetLlrEngine ()->GetReplaySize ()) : "none";
  };
  std::cout << "  [dbg] host0 " << st (SueGetDevice (0, 0)) << "\n";
  std::cout << "  [dbg] sw0   " << st (SueGetDevice (1, 0)) << "\n";
  std::cout << "  [dbg] sw1   " << st (SueGetDevice (1, 1)) << "\n";
  std::cout << "  [dbg] host2 " << st (SueGetDevice (2, 0)) << "\n";
  Ptr<SUESwitchNode> swdbg = DynamicCast<SUESwitchNode> (SueGetDevice (1, 0)->GetNode ());
  std::cout << "  [dbg] switch rx=" << swdbg->GetCntRxPkts () << " tx=" << swdbg->GetCntTxPkts ()
            << " miss=" << swdbg->GetCntLookupMiss () << "\n";
  Simulator::Destroy ();

  std::cout << "[B] ENABLE_SCALEUP=0: externalSetup called\n";
  const char *confOff = "/tmp/sue-s6-entry-off.conf";
  std::ofstream (confOff) << "ENABLE_SCALEUP 0\n";
  SueEntryDispatch (topo, confOff, &MyExternal);
  CHECK (g_extCalls == 1, "externalSetup invoked once (got " << g_extCalls << ")");

  std::cout << (g_ok ? "ENTRY DISPATCH PASS" : "ENTRY DISPATCH FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

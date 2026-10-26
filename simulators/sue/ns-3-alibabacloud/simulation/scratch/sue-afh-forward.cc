/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S1c end-to-end: 2 hosts + 1 SUESwitch, AFH Gen1 forwarding.
 * Verifies: frame reaches correct host (XPU-id LPM), TTL decremented at switch,
 * TTL==0 dropped, lookup-miss dropped. Four ±VLAN/±shim combinations.
 * Local fast check (scratch/ gitignored).
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-header.h"
#include "ns3/sue-config.h"
#include "ns3/sue-shim-header.h"
#include "ns3/sue-routing.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/mac48-address.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "sue-scratch-pcap.h"
#include <iostream>

using namespace ns3;


// host2 observer
static bool g_rx;
static uint32_t g_xpu;
static uint8_t g_ttl;
static bool g_vlan, g_shim;
static uint8_t g_pcp;
static uint8_t g_shimSize;
static void
Host2Rx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader h)
{
  g_rx = true;
  g_xpu = h.GetXpuId ();
  g_ttl = h.GetTtl ();
  g_vlan = h.HasVlan ();
  g_shim = h.HasShim ();
  g_pcp = h.GetPcp ();
  g_shimSize = h.HasShim () ? h.GetShim ().Size () : 0;
}

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

static Ptr<Packet>
BuildFrame (uint32_t dstXpu, bool vlan, uint8_t pcp, bool shim, uint8_t ttl, uint8_t dscp, bool f)
{
  Ptr<Packet> p = Create<Packet> (64);  // payload
  SueHeader h;
  h.SetDst (SueMacFromXpuId (dstXpu));
  h.SetSrc (SueMacFromXpuId (1));          // host1 XPU-id
  if (vlan) { h.SetVlan (true, pcp, 0, 100); }
  if (shim)
    {
      SueShimHeader sh;
      sh.ttl = ttl; sh.dscp = dscp; sh.f = f; if (f) { sh.flowLabel = 0x55; }
      // shim presence is per-etype config: use the EtherType configured with a
      // shim (default SUE_ETH_TYPE_1) so the receiver parses the shim.
      h.SetEtherType (SUE_ETH_TYPE_1);
      h.SetShim (sh);
    }
  else
    {
      h.SetEtherType (SUE_ETH_TYPE_0);
    }
  p->AddHeader (h);
  return p;
}

static void
SendOne (Ptr<SueNetDevice> h1, Ptr<Packet> p)
{
  g_rx = false; g_xpu = 0; g_ttl = 0xFF; g_vlan = false; g_shim = false; g_pcp = 0; g_shimSize = 0;
  // §12.51: enqueue on the frame's DERIVED VC (shim DSCP > VLAN PCP+DEI >
  // default) — the §12.50 guard asserts qIndex == ComputeVc(h); a hardcoded
  // 0 silently split charge/refund across VCs for VLAN/shim frames.
  SueHeader vh;
  uint32_t vc = 0;
  if (p->PeekHeader (vh))
    {
      if (vh.HasShim ()) { vc = SueConfig::DscpToVc (vh.GetShim ().dscp); }
      else if (vh.HasVlan ()) { vc = SueConfig::PcpToVc (vh.GetPcp (), vh.GetDei ()); }
      else { vc = SueConfig::DefaultVc (); }
    }
  h1->SwitchSend (vc, p);
  Simulator::Run ();
}

int
main ()
{
  // topology: h1 -- ch1 -- sw[port0] ... sw[port1] -- ch2 -- h2
  Ptr<Node> n1 = CreateObject<Node> ();
  Ptr<Node> n2 = CreateObject<Node> ();
  Ptr<SUESwitchNode> sw = CreateObject<SUESwitchNode> ();

  Ptr<SueNetDevice> h1 = CreateObject<SueNetDevice> ();
  Ptr<SueNetDevice> h2 = CreateObject<SueNetDevice> ();
  n1->AddDevice (h1);
  n2->AddDevice (h2);
  h2->SetReceiveUpcall (MakeCallback (&Host2Rx));

  Ptr<SueNetDevice> p0 = CreateObject<SueNetDevice> ();
  Ptr<SueNetDevice> p1 = CreateObject<SueNetDevice> ();
  sw->AddPort (p0);   // port 0 -> h1
  sw->AddPort (p1);   // port 1 -> h2

  Ptr<SueChannel> ch1 = CreateObject<SueChannel> ();
  h1->Attach (ch1);
  p0->Attach (ch1);
  Ptr<SueChannel> ch2 = CreateObject<SueChannel> ();
  h2->Attach (ch2);
  p1->Attach (ch2);

  // Bypass packet capture: no-op unless SUE_PCAP=1 (see sue-scratch-pcap.h).
  SueScratchPcapAttach (ch1, "afh-forward-h1-sw");
  SueScratchPcapAttach (ch2, "afh-forward-sw-h2");

  Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
  rt->AddPrefix (1, 10, {0});   // XPU-id 1 -> port 0
  rt->AddPrefix (2, 10, {1});   // XPU-id 2 -> port 1
  sw->SetRoutingTable (rt);

  // A: no VLAN, no shim
  std::cout << "[A] no VLAN, no shim -> host2\n";
  SendOne (h1, BuildFrame (2, false, 0, false, 0, 0, false));
  CHECK (g_rx, "host2 received");
  CHECK (g_xpu == 2, "dst XPU-id = 2");
  CHECK (g_ttl == 0, "no shim -> TTL 0");
  CHECK (sw->GetCntVcRx (0) == 1, "combo A (no shim/vlan) -> VC0");

  // B: VLAN, no shim
  std::cout << "[B] VLAN, no shim -> host2\n";
  SendOne (h1, BuildFrame (2, true, 2, false, 0, 0, false));
  CHECK (g_rx, "host2 received");
  CHECK (g_vlan && g_pcp == 2, "VLAN pcp=2 preserved");

  // C: no VLAN, shim 2B (F=0), TTL 3 -> 2
  std::cout << "[C] no VLAN, shim 2B TTL=3 -> host2, TTL=2\n";
  SendOne (h1, BuildFrame (2, false, 0, true, 3, 5, false));
  CHECK (g_rx, "host2 received");
  CHECK (g_ttl == 2, "TTL decremented 3->2");
  CHECK (g_shim && g_shimSize == 2, "shim 2B present");

  // D: VLAN, shim 3B (F=1), TTL 4 -> 3
  std::cout << "[D] VLAN, shim 3B TTL=4 -> host2, TTL=3\n";
  SendOne (h1, BuildFrame (2, true, 4, true, 4, 10, true));
  CHECK (g_rx, "host2 received");
  CHECK (g_ttl == 3, "TTL decremented 4->3");
  CHECK (g_shim && g_shimSize == 3, "shim 3B present");
  CHECK (g_vlan && g_pcp == 4, "VLAN pcp=4 preserved");

  // TTL==0 drop: shim TTL=1 -> switch decrements to 0 -> drop
  std::cout << "[TTL0] shim TTL=1 -> drop at switch\n";
  SendOne (h1, BuildFrame (2, false, 0, true, 1, 5, false));
  CHECK (!g_rx, "host2 does NOT receive (TTL expired)");

  // TTL==0 on arrival -> drop (already expired)
  std::cout << "[TTL0arr] shim TTL=0 on arrival -> drop\n";
  SendOne (h1, BuildFrame (2, false, 0, true, 0, 5, false));
  CHECK (!g_rx, "host2 does NOT receive (TTL=0 on arrival)");

  // lookup miss: dst XPU-id 999 (no route)
  std::cout << "[Miss] dst XPU-id 999 -> drop\n";
  SendOne (h1, BuildFrame (999, false, 0, false, 0, 0, false));
  CHECK (!g_rx, "host2 does NOT receive (lookup miss)");

  // AFH counters (§6.11): 7 received (A,B,C,D,TTL1,TTL0arr,Miss), 4 forwarded (A,B,C,D),
  // 2 TTL drops (ttl=1, ttl=0), 1 miss. vc_rx counts only forwarded (post-ingress) frames.
  std::cout << "[Cnt] AFH counters\n";
  CHECK (sw->GetCntRxPkts () == 7, "rx=7 (got " << sw->GetCntRxPkts () << ")");
  CHECK (sw->GetCntTxPkts () == 4, "tx=4 (got " << sw->GetCntTxPkts () << ")");
  CHECK (sw->GetCntTtlZeroDrop () == 2, "ttl_zero_drop=2 (got " << sw->GetCntTtlZeroDrop () << ")");
  CHECK (sw->GetCntLookupMiss () == 1, "lookup_miss=1");
  CHECK (sw->GetCntVcRx (0) == 1, "VC0 rx=1 (combo A)");
  CHECK (sw->GetCntVcRx (2) == 2, "VC2 rx=2 (combo B pcp=2, D dscp=10&7=2)");
  CHECK (sw->GetCntVcRx (5) == 1, "VC5 rx=1 (combo C dscp=5)");

  Simulator::Destroy ();
  std::cout << (g_ok ? "AFH FORWARD PASS" : "AFH FORWARD FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S1a check: AFH Gen1 header parse (4 combinations) + routing table LPM/ECMP.
 * Local fast check (scratch/ is gitignored). Run:
 *   cmake --build build -j8 --target scratch_sue-s1-parse
 *   ./build/scratch/ns3.36.1-sue-afh-header-parse-default
 */
#include "ns3/sue-header.h"
#include "ns3/sue-shim-header.h"
#include "ns3/sue-routing.h"
#include "ns3/packet.h"
#include "ns3/mac48-address.h"
#include <iostream>

using namespace ns3;


static bool g_ok = true;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    bool c = (cond);                                                           \
    std::cout << "  [" << (c ? "ok" : "FAIL") << "] " << msg << "\n";          \
    g_ok = g_ok && c;                                                          \
  } while (0)

// Round-trip a SueHeader through a Packet (AddHeader -> PeekHeader).
static SueHeader
RoundTrip (const SueHeader &h)
{
  Ptr<Packet> p = Create<Packet> ();
  p->AddHeader (h);
  SueHeader h2;
  p->PeekHeader (h2);
  return h2;
}

int
main ()
{
  // ---- A: no VLAN, no shim ----
  std::cout << "[A] no VLAN, no shim\n";
  {
    SueHeader h;
    h.SetDst (SueMacFromXpuId (0x1AB));
    h.SetSrc (SueMacFromXpuId (0x001));
    h.SetEtherType (SUE_ETH_TYPE_0);
    SueHeader r = RoundTrip (h);
    CHECK (r.GetXpuId () == 0x1AB, "XPU-id = 0x1AB (got 0x" << std::hex << r.GetXpuId () << ")");
    CHECK (!r.HasVlan (), "no VLAN");
    CHECK (!r.HasShim (), "no shim");
    CHECK (r.GetSerializedSize () == 14, "size 14 (got " << r.GetSerializedSize () << ")");
  }
  // ---- B: VLAN, no shim ----
  std::cout << "[B] VLAN, no shim\n";
  {
    SueHeader h;
    h.SetDst (SueMacFromXpuId (0x007));
    h.SetSrc (SueMacFromXpuId (0x002));
    h.SetVlan (true, 3, 0, 100);
    h.SetEtherType (SUE_ETH_TYPE_0);
    SueHeader r = RoundTrip (h);
    CHECK (r.HasVlan (), "has VLAN");
    CHECK (r.GetPcp () == 3, "PCP = 3 (got " << (uint32_t) r.GetPcp () << ")");
    CHECK (r.GetVid () == 100, "VID = 100");
    CHECK (!r.HasShim (), "no shim");
    CHECK (r.GetSerializedSize () == 18, "size 18 (got " << r.GetSerializedSize () << ")");
  }
  // ---- C: no VLAN, shim 2B (F=0) ----
  std::cout << "[C] no VLAN, shim 2B (F=0)\n";
  {
    SueHeader h;
    h.SetDst (SueMacFromXpuId (0x123));
    h.SetEtherType (SUE_ETH_TYPE_1);   // EtherType configured with a shim (§5.2/§9)
    SueShimHeader sh;
    sh.ttl = 7; sh.dscp = 5; sh.f = false;
    h.SetShim (sh);
    SueHeader r = RoundTrip (h);
    CHECK (r.HasShim (), "has shim");
    CHECK (r.GetTtl () == 7, "TTL = 7");
    CHECK (r.GetShim ().dscp == 5, "DSCP = 5");
    CHECK (r.GetShim ().Size () == 2, "shim size 2 (got " << (uint32_t) r.GetShim ().Size () << ")");
    CHECK (!r.HasVlan (), "no VLAN");
  }
  // ---- D: VLAN, shim 3B (F=1, Flow Label) ----
  std::cout << "[D] VLAN, shim 3B (F=1)\n";
  {
    SueHeader h;
    h.SetDst (SueMacFromXpuId (0x2FF));
    h.SetVlan (true, 4, 0, 7);
    h.SetEtherType (SUE_ETH_TYPE_1);   // EtherType configured with a shim (§5.2/§9)
    SueShimHeader sh;
    sh.ttl = 9; sh.dscp = 12; sh.f = true; sh.flowLabel = 0x42;
    h.SetShim (sh);
    SueHeader r = RoundTrip (h);
    CHECK (r.HasShim (), "has shim");
    CHECK (r.GetShim ().Size () == 3, "shim size 3 (got " << (uint32_t) r.GetShim ().Size () << ")");
    CHECK (r.GetShim ().flowLabel == 0x42, "FlowLabel = 0x42");
    CHECK (r.GetTtl () == 9, "TTL = 9");
    CHECK (r.GetPcp () == 4, "PCP = 4");
  }
  // ---- TTL decrement ----
  std::cout << "[TTL] decrement\n";
  {
    SueHeader h;
    h.SetEtherType (SUE_ETH_TYPE_1);   // EtherType configured with a shim (§5.2/§9)
    SueShimHeader sh; sh.ttl = 3; sh.f = false;
    h.SetShim (sh);
    h.DecrementTtl ();
    CHECK (h.GetTtl () == 2, "TTL 3 -> 2");
  }

  // ---- Routing: LPM + ECMP ----
  std::cout << "[RT] LPM + ECMP\n";
  {
    SueRoutingTable rt;
    rt.AddPrefix (0x1AB, 10, {2});   // exact -> port 2
    rt.AddPrefix (0x100, 8, {3, 4}); // top-8 prefix covers 0x100..0x103 -> ECMP {3,4}
    std::vector<uint32_t> ports;
    CHECK (rt.Lookup (0x1AB, ports) && ports.size () == 1 && ports[0] == 2, "exact 0x1AB -> {2}");
    CHECK (rt.Lookup (0x101, ports) && ports.size () == 2 && ports[0] == 3 && ports[1] == 4, "0x101 (len8) -> {3,4}");
    int32_t p1 = rt.GetOutPort (0x101);
    int32_t p2 = rt.GetOutPort (0x101);
    CHECK (p1 == p2 && (p1 == 3 || p1 == 4), "ECMP deterministic pick = " << p1);
    CHECK (!rt.Lookup (0x3FF, ports), "0x3FF -> no match");
    CHECK (rt.GetOutPort (0x1AB) == 2, "exact GetOutPort = 2");
  }

  std::cout << (g_ok ? "AFH HEADER-PARSE PASS" : "AFH HEADER-PARSE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

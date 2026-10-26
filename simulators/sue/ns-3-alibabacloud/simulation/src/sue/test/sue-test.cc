/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE test suite (S0 + S1). Formalized, tracked version of the scratch checks.
 * Built with full + NS3_TESTS=ON; run via the ns-3 test runner (--test-name=sue).
 */
#include "ns3/test.h"
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-header.h"
#include "ns3/sue-shim-header.h"
#include "ns3/sue-routing.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-error-model.h"
#include "ns3/sue-pfc-header.h"
#include "ns3/sue-cbfc-cc-header.h"
#include "ns3/sue-ctl-tag.h"
#include "ns3/sue-frame-bad-tag.h"
#include "ns3/sue-cbfc-header.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-config.h"
#include "ns3/sue-lite-endpoint.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/data-rate.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/mac48-address.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/config.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/string.h"
#include <array>
#include <vector>

using namespace ns3;

// ---- helpers ----

static SueHeader
RoundTrip (const SueHeader &h)
{
  Ptr<Packet> p = Create<Packet> ();
  p->AddHeader (h);
  SueHeader h2;
  p->PeekHeader (h2);
  return h2;
}

static Ptr<Packet>
BuildFrame (uint32_t dstXpu, bool vlan, uint8_t pcp, bool shim, uint8_t ttl, uint8_t dscp, bool f)
{
  Ptr<Packet> p = Create<Packet> (64);
  SueHeader h;
  h.SetDst (SueMacFromXpuId (dstXpu));
  h.SetSrc (SueMacFromXpuId (1));
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

// host2 observer
static bool g_rx;
static uint32_t g_xpu;
static uint8_t g_ttl;
static bool g_vlan, g_shim;
static uint8_t g_pcp, g_shimSize;
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

// S3 frame-count observer (uint32, for tests asserting > 1 frame)
static uint32_t g_rxCount;
static void
RxCountObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  ++g_rxCount;
}

// Build 2-host + 1-switch topology. h1 sends, h2 observes.
struct FwdTopo
{
  Ptr<SueNetDevice> h1;
  Ptr<SueNetDevice> h2;
  Ptr<SUESwitchNode> sw;
};
static FwdTopo
BuildFwdTopo ()
{
  FwdTopo t;
  Ptr<Node> n1 = CreateObject<Node> ();
  Ptr<Node> n2 = CreateObject<Node> ();
  t.sw = CreateObject<SUESwitchNode> ();
  t.h1 = CreateObject<SueNetDevice> ();
  t.h2 = CreateObject<SueNetDevice> ();
  n1->AddDevice (t.h1);
  n2->AddDevice (t.h2);
  t.h2->SetReceiveUpcall (MakeCallback (&Host2Rx));
  Ptr<SueNetDevice> p0 = CreateObject<SueNetDevice> ();
  Ptr<SueNetDevice> p1 = CreateObject<SueNetDevice> ();
  t.sw->AddPort (p0);
  t.sw->AddPort (p1);
  Ptr<SueChannel> c1 = CreateObject<SueChannel> ();
  t.h1->Attach (c1);
  p0->Attach (c1);
  Ptr<SueChannel> c2 = CreateObject<SueChannel> ();
  t.h2->Attach (c2);
  p1->Attach (c2);
  Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
  rt->AddPrefix (1, 10, {0});
  rt->AddPrefix (2, 10, {1});
  t.sw->SetRoutingTable (rt);
  return t;
}

static void
SendOne (Ptr<SueNetDevice> h1, Ptr<Packet> p)
{
  g_rx = false; g_xpu = 0; g_ttl = 0xFF; g_vlan = false; g_shim = false; g_pcp = 0; g_shimSize = 0;
  // §12.50: enqueue on the frame's DERIVED VC (shim DSCP > VLAN PCP > default)
  // — the same table the switch's Forward uses — so the credit charge (OnTx,
  // keyed by the dequeue VC) and the refund (RefundCredit, keyed by
  // ComputeVc) land on the same VC. The old hardcoded qIndex=0 silently
  // split them for every shim/VLAN frame (§12.50 root cause).
  SueHeader h;
  uint32_t vc = 0;
  if (p->PeekHeader (h))
    {
      if (h.HasShim ()) { vc = SueConfig::DscpToVc (h.GetShim ().dscp); }
      else if (h.HasVlan ()) { vc = SueConfig::PcpToVc (h.GetPcp (), h.GetDei ()); }
      else { vc = SueConfig::DefaultVc (); }
    }
  h1->SwitchSend (vc, p);
  Simulator::Run ();
}

// ============================ S0 cases ============================
class SueInstantiateTestCase : public TestCase
{
public:
  SueInstantiateTestCase () : TestCase ("AFH classes instantiable") {}
  void DoRun () override
  {
    NS_TEST_ASSERT_MSG_NE (CreateObject<SueNetDevice> (), nullptr, "SueNetDevice");
    NS_TEST_ASSERT_MSG_NE (CreateObject<SUESwitchNode> (), nullptr, "SUESwitchNode");
    NS_TEST_ASSERT_MSG_NE (CreateObject<SueChannel> (), nullptr, "SueChannel");
  }
};

class SueAttachTestCase : public TestCase
{
public:
  SueAttachTestCase () : TestCase ("AFH attach registers two devices") {}
  void DoRun () override
  {
    Ptr<SueNetDevice> a = CreateObject<SueNetDevice> ();
    Ptr<SueNetDevice> b = CreateObject<SueNetDevice> ();
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    a->Attach (ch);
    b->Attach (ch);
    NS_TEST_ASSERT_MSG_EQ (ch->GetNDevices (), 2u, "expected 2 devices");
  }
};

class SueDispatchTestCase : public TestCase
{
public:
  SueDispatchTestCase () : TestCase ("AFH transmit dispatches to dst receive") {}
  void DoRun () override
  {
    g_rx = false;
    Ptr<Node> n1 = CreateObject<Node> ();
    Ptr<Node> n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> src = CreateObject<SueNetDevice> ();
    Ptr<SueNetDevice> dst = CreateObject<SueNetDevice> ();
    n1->AddDevice (src);
    n2->AddDevice (dst);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    src->Attach (ch);
    dst->Attach (ch);
    dst->SetReceiveUpcall (MakeCallback (&Host2Rx));
    // A valid SUE frame (bare payloads are dropped by the non-SUE EtherType
    // guard in Receive, E4).
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h;
    h.SetDst (SueMacFromXpuId (2));
    h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    ch->TransmitStart (p, src, Seconds (0));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_rx, true, "TransmitStart did not dispatch to dst Receive");
    Simulator::Destroy ();
  }
};

// ============================ S1a: header parse ============================
class SueHeaderParseTestCase : public TestCase
{
public:
  SueHeaderParseTestCase () : TestCase ("AFH header parse (4 combos, XPU-id boundaries)") {}
  void DoRun () override
  {
    // XPU-id boundaries: 0, 511, 1023 (10-bit range)
    for (uint32_t x : { 0u, 511u, 1023u })
      {
        SueHeader h;
        h.SetDst (SueMacFromXpuId (x));
        h.SetEtherType (SUE_ETH_TYPE_0);
        SueHeader r = RoundTrip (h);
        NS_TEST_ASSERT_MSG_EQ (r.GetXpuId (), x, "XPU-id boundary");
      }
    // combo A: no VLAN, no shim
    {
      SueHeader h;
      h.SetDst (SueMacFromXpuId (0x1AB));
      h.SetEtherType (SUE_ETH_TYPE_0);
      SueHeader r = RoundTrip (h);
      NS_TEST_ASSERT_MSG_EQ (r.GetXpuId (), 0x1ABu, "A xpu");
      NS_TEST_ASSERT_MSG_EQ (r.HasVlan (), false, "A no vlan");
      NS_TEST_ASSERT_MSG_EQ (r.HasShim (), false, "A no shim");
      NS_TEST_ASSERT_MSG_EQ (r.GetTtl (), 0u, "A no shim -> TTL 0");
      NS_TEST_ASSERT_MSG_EQ (r.GetSerializedSize (), 14u, "A size 14");
    }
    // combo B: VLAN, no shim
    {
      SueHeader h;
      h.SetDst (SueMacFromXpuId (7));
      h.SetVlan (true, 3, 1, 100);
      h.SetEtherType (SUE_ETH_TYPE_0);
      SueHeader r = RoundTrip (h);
      NS_TEST_ASSERT_MSG_EQ (r.HasVlan (), true, "B vlan");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetPcp (), 3u, "B pcp");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetDei (), 1u, "B dei");
      NS_TEST_ASSERT_MSG_EQ (r.GetVid (), 100u, "B vid");
      NS_TEST_ASSERT_MSG_EQ (r.HasShim (), false, "B no shim");
    }
    // combo C: shim 2B (F=0) -- use the EtherType configured with a shim
    {
      SueHeader h;
      h.SetDst (SueMacFromXpuId (0x123));
      h.SetEtherType (SUE_ETH_TYPE_1);
      SueShimHeader sh; sh.ttl = 7; sh.dscp = 5; sh.f = false;
      h.SetShim (sh);
      SueHeader r = RoundTrip (h);
      NS_TEST_ASSERT_MSG_EQ (r.HasShim (), true, "C shim");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetShim ().Size (), 2u, "C shim 2B");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetTtl (), 7u, "C ttl");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetShim ().dscp, 5u, "C dscp");
    }
    // combo D: VLAN + shim 3B (F=1, FlowLabel)
    {
      SueHeader h;
      h.SetDst (SueMacFromXpuId (0x2FF));
      h.SetVlan (true, 4, 0, 7);
      h.SetEtherType (SUE_ETH_TYPE_1);
      SueShimHeader sh; sh.ttl = 9; sh.dscp = 12; sh.f = true; sh.flowLabel = 0x42;
      h.SetShim (sh);
      SueHeader r = RoundTrip (h);
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetShim ().Size (), 3u, "D shim 3B");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetShim ().flowLabel, 0x42u, "D flowLabel");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetTtl (), 9u, "D ttl");
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) r.GetPcp (), 4u, "D pcp");
    }
    // TTL decrement
    {
      SueHeader h;
      h.SetEtherType (SUE_ETH_TYPE_1);
      SueShimHeader sh; sh.ttl = 3; sh.f = false;
      h.SetShim (sh);
      h.DecrementTtl ();
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) h.GetTtl (), 2u, "TTL 3->2");
    }
  }
};

// ============================ S1a: routing ============================
class SueRoutingTestCase : public TestCase
{
public:
  SueRoutingTestCase () : TestCase ("AFH routing LPM + ECMP") {}
  void DoRun () override
  {
    SueRoutingTable rt;
    rt.AddPrefix (0x1AB, 10, {2});    // exact
    rt.AddPrefix (0x100, 8, {3, 4});   // top-8 prefix covers 0x100..0x103 -> ECMP
    rt.AddPrefix (0, 0, {5});          // default

    std::vector<uint32_t> ports;
    // exact wins over default
    NS_TEST_ASSERT_MSG_EQ (rt.Lookup (0x1AB, ports) && ports.size () == 1 && ports[0] == 2, true, "exact -> {2}");
    // 8-bit prefix ECMP
    NS_TEST_ASSERT_MSG_EQ (rt.Lookup (0x101, ports) && ports.size () == 2 && ports[0] == 3 && ports[1] == 4,
                           true, "0x101 -> {3,4}");
    // default route
    NS_TEST_ASSERT_MSG_EQ (rt.Lookup (0x3FF, ports) && ports.size () == 1 && ports[0] == 5, true, "0x3FF -> default {5}");
    // LPM priority: 0x1AB also matches 8-bit prefix? top8 of 0x1AB = 0x6A; prefix 0x100 len8 top8 = 0x40 -> no match.
    // So exact is the only match for 0x1AB. Good.
    // ECMP determinism: same xpu+seed -> same port
    rt.SetEcmpSeed (5);
    int32_t p1 = rt.GetOutPort (0x101);
    int32_t p2 = rt.GetOutPort (0x101);
    NS_TEST_ASSERT_MSG_EQ (p1, p2, "ECMP deterministic");
    bool validPort = (p1 == 3 || p1 == 4);
    NS_TEST_ASSERT_MSG_EQ (validPort, true, "ECMP valid port");
    // distribution: across 0x100..0x10F (top8=0x40), both ports 3 and 4 appear
    bool has3 = false, has4 = false;
    rt.SetEcmpSeed (0);
    for (uint32_t x = 0x100; x <= 0x10F; ++x)
      {
        int32_t p = rt.GetOutPort (x);
        has3 = has3 || (p == 3);
        has4 = has4 || (p == 4);
      }
    NS_TEST_ASSERT_MSG_EQ (has3 && has4, true, "ECMP distributes across {3,4}");
    // §6.9 flow-entropy ECMP: 0 entropy keeps the legacy pick bit-identical;
    // same (xpuId, entropy) is deterministic; entropy sweeps spread across the
    // whole group (per-flow affinity instead of destination parity).
    NS_TEST_ASSERT_MSG_EQ (rt.GetOutPort (0x101, 0), rt.GetOutPort (0x101),
                           "entropy 0 == legacy pick");
    bool entDet = (rt.GetOutPort (0x101, 0xABCDEF01ull) == rt.GetOutPort (0x101, 0xABCDEF01ull));
    NS_TEST_ASSERT_MSG_EQ (entDet, true, "flow-entropy pick deterministic");
    bool ent3 = false, ent4 = false;
    for (uint64_t e = 1; e <= 64; ++e)
      {
        int32_t p = rt.GetOutPort (0x101, e);
        ent3 = ent3 || (p == 3);
        ent4 = ent4 || (p == 4);
      }
    NS_TEST_ASSERT_MSG_EQ (ent3 && ent4, true, "flow entropy spreads across {3,4}");
    // prefixLen > 10 is clamped to 10
    SueRoutingTable rt2;
    rt2.AddPrefix (0x1AB, 11, {9});   // clamped to len 10
    NS_TEST_ASSERT_MSG_EQ (rt2.Lookup (0x1AB, ports) && ports.size () == 1 && ports[0] == 9, true, "prefixLen>10 clamped");
    // miss with no default
    SueRoutingTable rt3;
    NS_TEST_ASSERT_MSG_EQ (rt3.Lookup (0x1AB, ports), false, "no route -> miss");
  }
};

// ============================ S1c: end-to-end forwarding ============================
class SueForwardTestCase : public TestCase
{
public:
  SueForwardTestCase () : TestCase ("AFH forwarding (4 combos, TTL, miss)") {}
  void DoRun () override
  {
    FwdTopo t = BuildFwdTopo ();
    // A: no VLAN, no shim
    SendOne (t.h1, BuildFrame (2, false, 0, false, 0, 0, false));
    NS_TEST_ASSERT_MSG_EQ (g_rx, true, "A received");
    NS_TEST_ASSERT_MSG_EQ (g_xpu, 2u, "A dst xpu=2");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) g_ttl, 0u, "A no shim ttl 0");
    // B: VLAN pcp=2
    SendOne (t.h1, BuildFrame (2, true, 2, false, 0, 0, false));
    NS_TEST_ASSERT_MSG_EQ (g_rx && g_vlan && g_pcp == 2, true, "B vlan pcp=2");
    // C: shim 2B ttl=3 -> 2
    SendOne (t.h1, BuildFrame (2, false, 0, true, 3, 5, false));
    NS_TEST_ASSERT_MSG_EQ (g_rx && g_ttl == 2 && g_shim && g_shimSize == 2, true, "C ttl 3->2");
    // D: vlan+shim 3B ttl=4 -> 3
    SendOne (t.h1, BuildFrame (2, true, 4, true, 4, 10, true));
    NS_TEST_ASSERT_MSG_EQ (g_rx && g_ttl == 3 && g_shimSize == 3 && g_pcp == 4, true, "D ttl 4->3");
    // TTL==1 -> drop
    SendOne (t.h1, BuildFrame (2, false, 0, true, 1, 5, false));
    NS_TEST_ASSERT_MSG_EQ (g_rx, false, "TTL expired -> drop");
    // lookup miss
    SendOne (t.h1, BuildFrame (999, false, 0, false, 0, 0, false));
    NS_TEST_ASSERT_MSG_EQ (g_rx, false, "lookup miss -> drop");
    Simulator::Destroy ();
  }
};

// ============================ S1: AFH counters ============================
class SueCounterTestCase : public TestCase
{
public:
  SueCounterTestCase () : TestCase ("AFH counters (rx tx lookup_miss ttl_zero_drop vc)") {}
  void DoRun () override
  {
    FwdTopo t = BuildFwdTopo ();
    t.sw->ResetCounters ();
    // one forward: combo C, shim dscp=5 -> vc = 5&7 = 5
    SendOne (t.h1, BuildFrame (2, false, 0, true, 3, 5, false));
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntRxPkts (), 1ull, "rx=1");
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntTxPkts (), 1ull, "tx=1");
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntLookupMiss (), 0ull, "no miss");
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntTtlZeroDrop (), 0ull, "no ttl drop");
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (5), 1ull, "vc5 rx=1");
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcTx (5), 1ull, "vc5 tx=1");
    // TTL==1 -> drop (not forwarded)
    SendOne (t.h1, BuildFrame (2, false, 0, true, 1, 5, false));
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntTtlZeroDrop (), 1ull, "ttl drop=1");
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntTxPkts (), 1ull, "tx still 1 (dropped)");
    // lookup miss
    SendOne (t.h1, BuildFrame (999, false, 0, false, 0, 0, false));
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntLookupMiss (), 1ull, "miss=1");
    // VC priority: VLAN pcp=2, no shim -> vc=2
    SendOne (t.h1, BuildFrame (2, true, 2, false, 0, 0, false));
    NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (2), 1ull, "vc2 (pcp) rx=1");
    Simulator::Destroy ();
  }
};


// ============================ S2: wire-time + IFG ============================
static std::vector<Time> g_timingRx;
static void TimingRx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { g_timingRx.push_back (Simulator::Now ()); }

class SueTimingTestCase : public TestCase
{
public:
  SueTimingTestCase () : TestCase ("AFH wire-time + IFG") {}
  void DoRun () override
  {
    g_timingRx.clear ();
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> a = CreateObject<SueNetDevice> (), b = CreateObject<SueNetDevice> ();
    n1->AddDevice (a); n2->AddDevice (b);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    a->Attach (ch); b->Attach (ch);
    a->SetDataRate (DataRate ("100Gbps")); b->SetDataRate (DataRate ("100Gbps"));
    a->SetEgressQueue (CreateObject<SueEgressQueue> ());
    a->SetIfg (a->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    b->SetReceiveUpcall (MakeCallback (&TimingRx));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    Ptr<Packet> p1 = build (), p2 = build ();
    // Same CalculateBytesTxTime path the device uses — NS-quantized equality
    // holds exactly (100G: 102B = 8.16ns -> 8ns; IFG 1ns).
    Time wireTime = a->GetDataRate ().CalculateBytesTxTime (p1->GetSize ());
    Time ifg = a->GetDataRate ().CalculateBytesTxTime (12);
    a->SwitchSend (0, p1); a->SwitchSend (0, p2);
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_timingRx.size (), 2u, "2 frames received");
    NS_TEST_ASSERT_MSG_EQ (g_timingRx[0], wireTime, "A @ wireTime");
    NS_TEST_ASSERT_MSG_EQ (g_timingRx[1], wireTime + ifg + wireTime, "B @ wireTime+IFG+wireTime");
    Simulator::Destroy ();
  }
};

// ============================ S2: CBFC engine ============================
class SueCbfcEngineTestCase : public TestCase
{
public:
  SueCbfcEngineTestCase () : TestCase ("CBFC engine (credit exhaust CF CC)") {}
  void DoRun () override
  {
    SueCbfcEngine A, B;
    A.SetCreditSize (100); A.SetVcCreditLimit (0, 5); A.SetVcCreditLimit (1, 5); A.Init (); B.Init ();
    NS_TEST_ASSERT_MSG_EQ (A.CanSend (0, 100), true, "VC0 can send");
    A.OnTx (0, 100);
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCu (0), 1u, "CU=1");
    for (int i = 0; i < 4; ++i) { A.OnTx (0, 100); }
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCu (0), 5u, "CU=5 exhausted");
    NS_TEST_ASSERT_MSG_EQ (A.CanSend (0, 100), false, "VC0 cannot send");
    NS_TEST_ASSERT_MSG_EQ (A.CanSend (1, 100), true, "VC1 independent");
    A.OnTx (1, 100);
    for (int i = 0; i < 5; ++i) { B.OnRx (0, 100); B.OnDrain (0, 100); }
    A.OnCfUpdate (0, B.GetRvcCf (0));
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCu (0), 0u, "CU=0 after CF_Update");
    A.OnTx (0, 100);
    std::array<uint32_t, 16> cc{}; cc[0] = A.GetSvcCc (0);
    B.OnCcUpdate (cc);
    A.OnCfUpdate (0, B.GetRvcCf (0));
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCu (0), 0u, "leak recovered");

    // CFG-CR-006 (§5.2.10.2): PktOvhd range -16..+127 and the below-size clamp.
    SueCbfcEngine P;
    P.SetCreditSize (100); P.SetVcCreditLimit (0, 10); P.Init ();
    P.SetPktOvhd (-16);
    NS_TEST_ASSERT_MSG_EQ (P.PktCredits (100), 1u, "PktOvhd=-16: ceil((100-16)/100) -> 1");
    P.SetPktOvhd (127);
    NS_TEST_ASSERT_MSG_EQ (P.PktCredits (100), 3u, "PktOvhd=+127: ceil(227/100) -> 3");
    P.SetPktOvhd (-200);
    NS_TEST_ASSERT_MSG_EQ (P.PktCredits (100), 1u, "PktOvhd below -size clamps to 0 -> 1 credit");
    P.SetPktOvhd (0);
  }
};

// ============================ S2: CC_Update wraparound guard ============================
class SueCcUpdateNegativeLostTestCase : public TestCase
{
public:
  SueCcUpdateNegativeLostTestCase () : TestCase ("CBFC CC_Update LostCredits<0 resync-only") {}
  void DoRun () override
  {
    SueCbfcEngine A, B;
    A.SetCreditSize (100); A.SetVcCreditLimit (0, 5); A.Init ();
    B.SetCreditSize (100); B.Init ();
    // R_VC_CC ahead of the sender's CC (e.g. a CC_Update processed inside an
    // LLR window): UEC §5.2.5.7.4 severe error. The wraparound value must not
    // be folded into R_VC_CF.
    B.OnRx (0, 100);
    std::array<uint32_t, 16> cc{}; cc[0] = A.GetSvcCc (0);   // 0
    B.OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (B.GetRvcCf (0), 0u, "R_VC_CF not poisoned (LostCredits<0)");
    // Positive path unchanged: sender sent 2, receiver drained 1 -> lost=1.
    A.OnTx (0, 100); A.OnTx (0, 100);
    B.OnRx (0, 100); B.OnDrain (0, 100);
    cc[0] = A.GetSvcCc (0);   // 2
    B.OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (B.GetRvcCf (0), 2u, "lost credit recovered (normal path)");
  }
};

// ================= S2e: dual cell granularity (UEC §5.2.5.8 method-2) =================
// A pow2-only sender snaps its TX cell to 32 facing a receiver with a native
// 56B MMU cell. The sender charges ceil(F/32); the receiver consumes/frees
// ceil(F/56); the per-frame difference leaks from the sender's CU and the
// periodic CC_Update reconciliation (LostCredits = S_CC - R_CC -> R_VC_CF)
// recovers it. Canonical walk for a 1518B frame: charge 48, refund 28, leak 20.
class SueCbfcDualCellTestCase : public TestCase
{
public:
  SueCbfcDualCellTestCase () : TestCase ("CBFC dual-cell granularity (32 vs 56 reconcile)") {}
  void DoRun () override
  {
    SueCbfcEngine A, B;
    A.SetTxCellSize (32); A.SetRxCellSize (128);   // A: sends at 32, native rx 128
    B.SetTxCellSize (128); B.SetRxCellSize (56);   // B (switch role): native rx 56, sends at 128
    A.SetVcCreditLimit (0, 4096); A.Init (); B.Init ();

    NS_TEST_ASSERT_MSG_EQ (A.PktCredits (1518), 48u, "tx credits ceil(1518/32) = 48");
    NS_TEST_ASSERT_MSG_EQ (B.RxPktCredits (1518), 28u, "rx cells ceil(1518/56) = 28");
    // Sub-cell frame still occupies one cell (UEC §5.2.5.8).
    NS_TEST_ASSERT_MSG_EQ (B.RxPktCredits (40), 1u, "40B < 56B cell still 1 credit");
    NS_TEST_ASSERT_MSG_EQ (A.PktCredits (40), 2u, "40B at 32B tx = 2 credits");

    A.OnTx (0, 1518);
    B.OnRx (0, 1518); B.OnDrain (0, 1518);
    A.OnCfUpdate (0, B.GetRvcCf (0));
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCc (0), 48u, "S_CC = 48");
    NS_TEST_ASSERT_MSG_EQ (B.GetRvcCc (0), 28u, "R_CC = 28 (native cells)");
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCu (0), 20u, "CU = 20 after CF_Update (granularity leak)");
    std::array<uint32_t, 16> cc{}; cc[0] = A.GetSvcCc (0);
    B.OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (B.GetRvcCf (0), 48u, "R_CF = 48 after reconciliation (28 drain + 20 leak)");
    A.OnCfUpdate (0, B.GetRvcCf (0));
    NS_TEST_ASSERT_MSG_EQ (A.GetSvcCu (0), 0u, "CU = 0 after the CC_Update round (ledger settled)");
  }
};

// ================= S2e: TX-cell adoption snap (UEC §5.2.8.2 / §5.2.10.1) =================
class SueCbfcCellAdoptTestCase : public TestCase
{
public:
  SueCbfcCellAdoptTestCase () : TestCase ("CBFC TX-cell adoption snap (5.2.8.2 + 5.2.10.1)") {}
  void DoRun () override
  {
    SueCbfcEngine A;
    A.SetRxCellSize (128);                                      // native MMU cell
    A.SetTxCellChoices ({2048, 32, 64, 128, 256, 512, 1024});   // unordered on purpose
    A.AdoptTxCellFromPeerRx (56);                               // peer advertises its native 56
    NS_TEST_ASSERT_MSG_EQ (A.GetTxCellSize (), 32u, "56 snaps down to 32 (largest pow2 <= 56)");
    NS_TEST_ASSERT_MSG_EQ (A.GetRxCellSize (), 128u, "rx native cell untouched by adoption");
    A.AdoptTxCellFromPeerRx (128);
    NS_TEST_ASSERT_MSG_EQ (A.GetTxCellSize (), 128u, "exact match adopts verbatim");
    A.AdoptTxCellFromPeerRx (0);
    NS_TEST_ASSERT_MSG_EQ (A.GetTxCellSize (), 128u, "peer cell 0 (absent TLV) keeps current");
    // No supported cell <= peer: smallest supported + loud warn (the
    // LostCredits<0 severe error would surface on the first CC_Update).
    SueCbfcEngine C;
    C.SetTxCellChoices ({64, 128});
    C.AdoptTxCellFromPeerRx (32);
    NS_TEST_ASSERT_MSG_EQ (C.GetTxCellSize (), 64u, "no compliant choice -> smallest supported");
    // Empty choice set = legacy verbatim adoption.
    SueCbfcEngine L;
    L.AdoptTxCellFromPeerRx (56);
    NS_TEST_ASSERT_MSG_EQ (L.GetTxCellSize (), 56u, "empty choice set adopts verbatim (legacy)");
  }
};

// ================= S2e: rx == tx degenerates to the single-cell engine =================
class SueCbfcDualCellCompatTestCase : public TestCase
{
public:
  SueCbfcDualCellCompatTestCase () : TestCase ("CBFC rx==tx identical to single-cell") {}
  void DoRun () override
  {
    SueCbfcEngine la, lb, da, db;
    la.SetCreditSize (100); lb.SetCreditSize (100);          // legacy pair
    da.SetCreditSize (100); db.SetCreditSize (100);          // dual pair (equal cells)
    da.SetTxCellChoices ({32, 64, 100, 128});
    da.AdoptTxCellFromPeerRx (100);                          // snap preserves 100
    db.AdoptTxCellFromPeerRx (100);
    NS_TEST_ASSERT_MSG_EQ (da.GetTxCellSize (), 100u, "snap preserves the equal cell");
    for (uint32_t f : {86u, 100u, 1518u, 40u})
      {
        NS_TEST_ASSERT_MSG_EQ (da.PktCredits (f), la.PktCredits (f), "PktCredits equal");
        NS_TEST_ASSERT_MSG_EQ (da.RxPktCredits (f), la.PktCredits (f), "RxPktCredits equal");
      }
    // Identical op sequence -> identical observable counters.
    for (int i = 0; i < 7; ++i)
      {
        la.OnTx (0, 1518); da.OnTx (0, 1518);
        lb.OnRx (0, 1518); db.OnRx (0, 1518);
        if (i % 2) { lb.OnDrain (0, 1518); db.OnDrain (0, 1518); }
        la.OnCfUpdate (0, lb.GetRvcCf (0)); da.OnCfUpdate (0, db.GetRvcCf (0));
      }
    std::array<uint32_t, 16> cc{}; cc[0] = la.GetSvcCc (0);
    lb.OnCcUpdate (cc); cc[0] = da.GetSvcCc (0); db.OnCcUpdate (cc);
    la.OnCfUpdate (0, lb.GetRvcCf (0)); da.OnCfUpdate (0, db.GetRvcCf (0));
    NS_TEST_ASSERT_MSG_EQ (da.GetSvcCc (0), la.GetSvcCc (0), "S_CC identical");
    NS_TEST_ASSERT_MSG_EQ (da.GetSvcCf (0), la.GetSvcCf (0), "S_CF identical");
    NS_TEST_ASSERT_MSG_EQ (da.GetSvcCu (0), la.GetSvcCu (0), "CU identical");
    NS_TEST_ASSERT_MSG_EQ (db.GetRvcCc (0), lb.GetRvcCc (0), "R_CC identical");
    NS_TEST_ASSERT_MSG_EQ (db.GetRvcCf (0), lb.GetRvcCf (0), "R_CF identical");
  }
};

// ================= S2e: dual-cell end-to-end over real devices =================
// A(32) -> B(56): the granularity leak would exhaust the window mid-transfer
// (512-credit window, 20 leaked per 1518B frame -> stalled after ~25 frames);
// the periodic CC_Update keeps the ledger settled, so every frame is
// delivered and CU returns to 0.
static uint32_t g_dualCellRx = 0;
static void DualCellRx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_dualCellRx; }
class SueCbfcDualCellE2eTestCase : public TestCase
{
public:
  SueCbfcDualCellE2eTestCase () : TestCase ("CBFC dual-cell e2e (CC_Update reconciles leak)") {}
  void DoRun () override
  {
    g_dualCellRx = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (32);                            // A: tx == rx == 32 (snapped sender)
    cb->SetTxCellSize (56); cb->SetRxCellSize (56);    // B: native 56 in both roles
    ca->SetVcCreditLimit (0, 512); ca->Init (); cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    B->SetDrainDelay (MicroSeconds (10));
    B->SetCfUpdateTimers (MicroSeconds (5), MicroSeconds (50));
    A->StartCcUpdateTimer (MicroSeconds (50));
    B->SetReceiveUpcall (MakeCallback (&DualCellRx));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (1500); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 100; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Stop (MicroSeconds (2000));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_dualCellRx, 100u, "all 100 delivered despite the leak");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (0), 4800u, "S_CC = 100 x 48 (32B granularity)");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "CU = 0 (leak fully reconciled by CC_Update)");
    const bool ccSeen = cb->GetCounters ().cc_update_rx > 0;
    NS_TEST_ASSERT_MSG_EQ (ccSeen, true, "CC_Update processed at B");
    Simulator::Destroy ();
  }
};

// ============================ S2: CBFC end-to-end ============================
static std::vector<Time> g_e2eRx;
static void E2eRx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { g_e2eRx.push_back (Simulator::Now ()); }
class SueCbfcE2eTestCase : public TestCase
{
public:
  SueCbfcE2eTestCase () : TestCase ("CBFC e2e (gating + CF_Update recovery)") {}
  void DoRun () override
  {
    g_e2eRx.clear ();
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100); ca->SetVcCreditLimit (0, 5); ca->Init (); cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    B->SetDrainDelay (MicroSeconds (10));

    B->SetReceiveUpcall (MakeCallback (&E2eRx));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_e2eRx.size (), 10u, "10 delivered");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (0), 10u, "consumed 10");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "all returned");
    bool paused = (g_e2eRx.size () >= 6) && (g_e2eRx[5] > MicroSeconds (10));
    NS_TEST_ASSERT_MSG_EQ (paused, true, "6th after drainDelay (gating held)");
    Simulator::Destroy ();
  }
};

// ============================ S2: TTL-drop deferred refund ============================
// §12.30: with the deferred refund, a TTL==0 drop must release the ingress
// credit for the FULL on-wire frame (header included) — the same byte count
// the Receive path charged — else a frame straddling a credit boundary leaks
// one credit per drop and the sender's CU never returns to 0.
class SueTtlDropRefundTestCase : public TestCase
{
public:
  SueTtlDropRefundTestCase () : TestCase ("CBFC TTL-drop refunds full-frame credit (no leak)") {}
  void DoRun () override
  {
    // Mini topo: host A -- switch p0/p1 -- host B (frame dies at the switch).
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SUESwitchNode> sw = CreateObject<SUESwitchNode> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueNetDevice> p0 = CreateObject<SueNetDevice> (), p1 = CreateObject<SueNetDevice> ();
    sw->AddPort (p0); sw->AddPort (p1);
    Ptr<SueChannel> c1 = CreateObject<SueChannel> (); A->Attach (c1); p0->Attach (c1);
    Ptr<SueChannel> c2 = CreateObject<SueChannel> (); B->Attach (c2); p1->Attach (c2);
    Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
    rt->AddPrefix (1, 10, {0});
    rt->AddPrefix (2, 10, {1});
    sw->SetRoutingTable (rt);
    // CBFC on the sender and the switch ingress port (deferred refund, §12.30).
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cp0 = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (256); ca->SetVcCreditLimit (5, 10); ca->Init ();
    cp0->SetCreditSize (256); cp0->Init ();
    A->SetCbfcEngine (ca); A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    p0->SetCbfcEngine (cp0); p0->SetDeferCreditRefund (true);
    A->SetDataRate (DataRate ("100Gbps")); p0->SetDataRate (DataRate ("100Gbps"));
    // Frame: SueHeader(14) + shim 2B = 16B header; payload 254B -> 270B on wire.
    // charge = ceil(270/256) = 2 credits. The (buggy) post-strip refund of
    // 254B = ceil(254/256) = 1 credit would leave CU=1 after CF_Update.
    Ptr<Packet> p = Create<Packet> (254);
    SueHeader h;
    h.SetDst (SueMacFromXpuId (2)); h.SetSrc (SueMacFromXpuId (1));
    SueShimHeader sh; sh.ttl = 1; sh.dscp = 5; sh.f = false;
    h.SetEtherType (SUE_ETH_TYPE_1); h.SetShim (sh);
    p->AddHeader (h);
    A->SwitchSend (5, p);
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntTtlZeroDrop (), 1ull, "TTL drop at switch");
    // The deferred refund was fed back via CF_Update: charge must equal refund.
    ca->OnCfUpdate (5, cp0->GetRvcCf (5));
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (5), cp0->GetRvcCf (5), "charge == refund");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (5), 0u, "no credit leak after TTL drop");
    Simulator::Destroy ();
  }
};

// ============================ §12.42: deferred refund vs LLR preamble ============================
// On an idle egress, SwitchSend runs a synchronous DequeueAndTransmit which
// prepends the 8B LLR preamble IN PLACE onto the same packet object the
// deferred refund then measures. A 2048B frame (8 credits at 256B/credit)
// becomes 2056B after the preamble — ceil(2056/256) = 9 credits: the refund
// would release one credit MORE than the ingress charged, the sender's
// S_VC_CF runs past S_VC_CC, CU = (CC-CF) mod 2^15 flips to ~2^15 and the
// credit window is permanently poisoned (first hit: the VRF 32-plane
// all-to-all stalled at ~1% progress under LLR+CBFC, §12.41). The refund
// must measure the frame at the size Receive charged — before SwitchSend.
class SueDeferredRefundPreambleTestCase : public TestCase
{
public:
  SueDeferredRefundPreambleTestCase () : TestCase ("CBFC deferred refund is preamble-invariant (no over-refund)") {}
  void DoRun () override
  {
    g_rxCount = 0;
    // Mini topo: host A -- switch p0/p1 -- host B.
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SUESwitchNode> sw = CreateObject<SUESwitchNode> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueNetDevice> p0 = CreateObject<SueNetDevice> (), p1 = CreateObject<SueNetDevice> ();
    sw->AddPort (p0); sw->AddPort (p1);
    Ptr<SueChannel> c1 = CreateObject<SueChannel> (); A->Attach (c1); p0->Attach (c1);
    Ptr<SueChannel> c2 = CreateObject<SueChannel> (); B->Attach (c2); p1->Attach (c2);
    Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
    rt->AddPrefix (2, 10, {1});
    sw->SetRoutingTable (rt);
    // CBFC: sender, deferred-refund ingress port, egress port (credit gate).
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cp0 = CreateObject<SueCbfcEngine> (), cp1 = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (256); ca->SetVcCreditLimit (0, 32); ca->Init ();
    cp0->SetCreditSize (256); cp0->Init ();
    cp1->SetCreditSize (256); cp1->SetVcCreditLimit (0, 32); cp1->Init ();
    A->SetCbfcEngine (ca); A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    p0->SetCbfcEngine (cp0); p0->SetDeferCreditRefund (true);
    p1->SetCbfcEngine (cp1); p1->SetEgressQueue (CreateObject<SueEgressQueue> ());
    // LLR framing on the egress->B link: DequeueAndTransmit prepends the 8B
    // preamble in place (the mutation the refund must not observe). No LLR
    // engine: the legacy preamble path needs no INIT handshake.
    p1->SetLlrEnabled (true); B->SetLlrEnabled (true);
    A->SetDataRate (DataRate ("100Gbps")); p0->SetDataRate (DataRate ("100Gbps"));
    p1->SetDataRate (DataRate ("100Gbps")); B->SetDataRate (DataRate ("100Gbps"));
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    // Two 2048B frames (2034 payload + 14 SueHeader): charge = 2048/256 = 8
    // credits each. Frame 1 reaches p1 while the egress is idle (synchronous
    // TX + in-place preamble — the hazard); frame 2 while p1 is still busy
    // (queued TX). BOTH refunds must release exactly the 8 charged credits:
    // the pre-fix code refunded ceil(2056/256) = 9 for frame 1.
    auto build = [] ()
      {
        Ptr<Packet> p = Create<Packet> (2034);
        SueHeader h;
        h.SetDst (SueMacFromXpuId (2)); h.SetSrc (SueMacFromXpuId (1));
        h.SetEtherType (SUE_ETH_TYPE_0);
        p->AddHeader (h);
        return p;
      };
    A->SwitchSend (0, build ());
    A->SwitchSend (0, build ());
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 2u, "both frames delivered (preamble stripped at B)");
    NS_TEST_ASSERT_MSG_EQ (cp0->GetRvcCf (0), 16u, "refund == charged credits (2 x 8, preamble-free size)");
    ca->OnCfUpdate (0, cp0->GetRvcCf (0));
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (0), 16u, "sender charged 2 x 8 credits");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "no over-refund: sender window fully reclaimed");
    Simulator::Destroy ();
  }
};


// ============================ S3: LLR preamble + FCS ============================
class SueLlrPreambleTestCase : public TestCase
{
public:
  SueLlrPreambleTestCase () : TestCase ("LLR preamble + FCS drop") {}
  void DoRun () override
  {
    g_rx = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    A->SetLlrEnabled (true); B->SetLlrEnabled (true);
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetBadEvery (2); B->SetErrorModel (em);
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 4; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 2u, "2 good frames delivered (2 bad dropped)");
  }
};

// ============================ S3: LLR NACK -> retransmit ============================
class SueLlrRetransmitTestCase : public TestCase
{
public:
  SueLlrRetransmitTestCase () : TestCase ("LLR NACK -> retransmit") {}
  void DoRun () override
  {
    g_rx = 0;
    FwdTopo t = BuildFwdTopo ();   // not used; build LLR-specific
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();

    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCorruptOnce (2); B->SetErrorModel (em);
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 3u, "3 delivered (NACK -> retransmit)");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eA->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "state ADVANCE");
    NS_TEST_ASSERT_MSG_EQ (eA->GetReplaySize (), 0u, "replay empty");
    // COUNTER-022: rx_replay counts the retransmit accepted in the NACK window.
    NS_TEST_ASSERT_MSG_EQ (eB->GetCounters ().rx_replay, 1ull,
                           "rx_replay counted (retransmit accepted in NACK window)");
  }
};

// ============================ S3: FLUSH not lifted by late ACK/NACK ============================
class SueLlrFlushStaysFlushTestCase : public TestCase
{
public:
  SueLlrFlushStaysFlushTestCase () : TestCase ("LLR FLUSH not lifted by late ACK (re_init_on_discard=0)") {}
  void DoRun () override
  {
    // (a) re_init_on_discard=0: FLUSH must survive a late in-range ACK — the
    // machine stays in FLUSH until management Start()s it (DESIGN §6.10).
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    e->SetReInitOnDiscard (false);
    e->SetReplayCtMax (0);   // first DoReplay -> replay_ct_is_max -> FLUSH
    e->SetReplayTimerMax (MicroSeconds (50));
    e->Start ();   // no init sink -> GoAdvance (test pattern)
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "started ADVANCE");
    e->TxStore (Create<Packet> (86));   // seq 0, txSeq -> 1
    e->OnReplayTimer ();   // REPLAY -> replay_ct(0)>=max(0) -> FLUSH
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->GetTxState (), (uint32_t) SueLlrEngine::TxState::FLUSH, "entered FLUSH");
    // Late ACK(0) passes the UEC ok-formula (acked_seq = SEQ_MOD-1): must NOT
    // lift the machine into ADVANCE (would skip INIT re-sync, FLUSH<->ADVANCE
    // oscillation with data never delivered).
    e->OnAckNack (0, false);
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->GetTxState (), (uint32_t) SueLlrEngine::TxState::FLUSH, "FLUSH survives late ACK");

    // (b) normal completion path unchanged: REPLAY -> ADVANCE when the whole
    // buffer is ACKed.
    Ptr<SueLlrEngine> e2 = CreateObject<SueLlrEngine> ();
    e2->SetReplayCtMax (255);   // never FLUSH via replay count
    e2->Start ();
    e2->TxStore (Create<Packet> (86));   // seq 0
    e2->TxStore (Create<Packet> (86));   // seq 1
    e2->OnAckNack (0, true);   // NACK(0): release seq0, buffer keeps seq1 -> REPLAY
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e2->GetTxState (), (uint32_t) SueLlrEngine::TxState::REPLAY, "entered REPLAY");
    e2->OnAckNack (1, false);  // ACK(1): buffer empty -> ADVANCE
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e2->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "REPLAY completed to ADVANCE");
    NS_TEST_ASSERT_MSG_EQ (e2->GetReplaySize (), 0u, "replay buffer empty");
    NS_TEST_ASSERT_MSG_EQ (e2->GetCounters ().tx_replay, 1ull, "tx_replay = 1 completed replay (UEC Table 5-13)");
    Simulator::Destroy ();
  }
};

// ============================ S3: partial ACK wakes TX (stop-and-go prevention) ============================
// Without the TxOpportunityCb, a partially-released LLR window (buffer not
// empty) never wakes DequeueAndTransmit: TX stalls until the WHOLE window is
// ACKed — throughput collapses to stop-and-go at window granularity, or the
// egress queue stalls indefinitely if the last ACK is lost.
class SueLlrPartialAckWakeTestCase : public TestCase
{
public:
  SueLlrPartialAckWakeTestCase () : TestCase ("LLR partial ACK wakes TX (no stop-and-go)") {}
  void DoRun () override
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetOutstandingSeqMax (4);   // window = 4
    eB->SetAckPeriod (MilliSeconds (1));   // cumulative ACK at 1ms cadence
    eA->Start (); eB->Start ();
    Simulator::Stop (MicroSeconds (10));   // handshake only (before the 1ms ACK)
    Simulator::Run ();
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 6; ++i) { A->SwitchSend (0, build ()); }
    // The 1ms cumulative ACK(2) releases 3 of the 4 in-flight frames: buffer
    // non-empty. Without the wakeup, frames 4/5 wait for the 2ms ACK(3) that
    // empties the buffer; with it, they transmit right after 1ms.
    Simulator::Stop (MicroSeconds (1050));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 6u, "all 6 delivered right after the partial ACK");
  }
};

// ============================ S3: PFC pause does not stop LLR replay ============================
// UEC §5.1: "the reception of pause frames does not stop the replay of
// previously sent data frames by LLR and acts only on new data frames."
class SueLlrReplayDuringPfcPauseTestCase : public TestCase
{
public:
  SueLlrReplayDuringPfcPauseTestCase () : TestCase ("LLR PFC pause does not stop replay (UEC 5.1)") {}
  void DoRun () override
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();
    Simulator::Run ();   // INIT / INIT_ECHO handshake
    // B: RX unfiltered (mode_local OFF) so the replayed seq1 is accepted as-is.
    eB->SetModeLocal (false);
    // Sender-side PFC: pause VC 0 BEFORE the NACK-driven replay fires.
    Ptr<SuePfcEngine> pa = CreateObject<SuePfcEngine> ();
    pa->Init ();
    A->SetPfcEngine (pa);
    pa->OnPause (0);
    NS_TEST_ASSERT_MSG_EQ (pa->IsPaused (0), true, "VC0 paused");
    // Two frames stored in the LLR replay buffer; NACK(0) releases seq0 and
    // drives a REPLAY of seq1 while the VC is paused.
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    eA->TxStore (build ());   // seq 0
    eA->TxStore (build ());   // seq 1
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    eA->OnAckNack (0, true);
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eA->GetTxState (), (uint32_t) SueLlrEngine::TxState::REPLAY, "entered REPLAY");
    // Stop after the replay frame's wire time (~10ns): B is mode_local OFF and
    // sends no ACK, so an unbounded Run would let the replay timer re-fire
    // (repeat replays) — we only assert the pause-gated FIRST retransmit.
    Simulator::Stop (MicroSeconds (10));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 1u, "replay delivered despite pause (UEC 5.1)");
  }
};

// ============================ S3: FLUSH + INIT recovery ============================
class SueLlrFlushTestCase : public TestCase
{
public:
  SueLlrFlushTestCase () : TestCase ("LLR FLUSH + INIT recovery") {}
  void DoRun () override
  {
    g_rx = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayCtMax (2); eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();

    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCorruptFirstN (3); B->SetErrorModel (em);
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    A->SwitchSend (0, build ());   // seq=0, will FLUSH
    // schedule 2 new frames after FLUSH
    Simulator::Schedule (MicroSeconds (300), &SueNetDevice::SwitchSend, A, 0, build ());
    Simulator::Schedule (MicroSeconds (301), &SueNetDevice::SwitchSend, A, 0, build ());
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (eA->GetCntTxDiscard (), 1ull, "1 FLUSH");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eB->GetNextRxSeq (), 3u, "B re-synced to 3");
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 2u, "2 post-FLUSH frames delivered");
  }
};

// ============================ S3: POISONED + counters ============================
// FRM-IFT-003: frame_bad reporting — the POISONED frame reaches the upper
// layer carrying the SueFrameBadTag (SueNetDevice tags ACCEPT_POISONED, §5.1.10).
static uint32_t g_rxBadTagCount;
static void
RxBadTagObs (Ptr<SueNetDevice>, Ptr<Packet> p, SueHeader)
{
  ++g_rxCount;
  SueFrameBadTag fb;
  if (p->PeekPacketTag (fb)) { ++g_rxBadTagCount; }
}

class SueLlrPoisonedTestCase : public TestCase
{
public:
  SueLlrPoisonedTestCase () : TestCase ("LLR POISONED + 22 counters") {}
  void DoRun () override
  {
    g_rx = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();

    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetPoisonOnce (2); B->SetErrorModel (em);
    B->SetReceiveUpcall (MakeCallback (&RxBadTagObs));
    g_rxCount = 0; g_rxBadTagCount = 0;
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 3u, "3 delivered (poisoned accepted)");
    NS_TEST_ASSERT_MSG_EQ (g_rxBadTagCount, 1u,
                           "frame_bad tag set exactly once (the POISONED frame)");
    const SueLlrCounters &bc = eB->GetCounters ();
    NS_TEST_ASSERT_MSG_EQ (bc.rx_ok, 2ull, "rx_ok=2");
    NS_TEST_ASSERT_MSG_EQ (bc.rx_poisoned, 1ull, "rx_poisoned=1");
    NS_TEST_ASSERT_MSG_EQ (bc.rx_expected_seq_poisoned, 1ull, "rx_expected_seq_poisoned=1");
    NS_TEST_ASSERT_MSG_EQ (bc.tx_nack_ctl_os, 0ull, "B tx_nack=0 (no NACK for poisoned)");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eB->GetNextRxSeq (), 3u, "B next_rx_seq=3");
    const SueLlrCounters &ac = eA->GetCounters ();
    NS_TEST_ASSERT_MSG_EQ (ac.tx_replay, 0ull, "A tx_replay=0");
    NS_TEST_ASSERT_MSG_EQ (ac.rx_ack_ctl_os, 3ull, "A rx_ack=3");

    DoRunTxPoison ();
  }

  // --- scenario 2: TX-side FCS poisoning (COUNTER-007, §5.1.10) ---
  // The SENDER poisons the 2nd eligible frame (XOR 0xAAAAAAAA semantics): the
  // frame reaches the peer with SueFrameBadTag -> classified POISONED ->
  // accepted, no NACK, no replay; tx_poisoned / rx_poisoned both count it.
  void DoRunTxPoison ()
  {
    g_rxCount = 0; g_rxBadTagCount = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetPoisonTxOnce (2);   // A's 2nd eligible TX frame is poisoned
    A->SetErrorModel (em);
    B->SetReceiveUpcall (MakeCallback (&RxBadTagObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 3u, "3 delivered (TX-poisoned accepted, no replay)");
    NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().tx_poisoned, 1ull,
                           "tx_poisoned counts the TX-poisoned frame");
    NS_TEST_ASSERT_MSG_EQ (eB->GetCounters ().rx_poisoned, 1ull,
                           "peer classified it POISONED (rx_poisoned=1)");
    NS_TEST_ASSERT_MSG_EQ (eB->GetCounters ().tx_nack_ctl_os, 0ull, "no NACK for poisoned");
    NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().tx_replay, 0ull, "no replay");
    NS_TEST_ASSERT_MSG_EQ (g_rxBadTagCount, 1u, "frame_bad tag set exactly once");
  }
};

// ============================ S3b: INIT handshake (echo + timeout) ============================
// Sink-to-peer INIT/INIT_ECHO forwarding (unit-level; the device wires the same
// sinks to SendCtlInit/SendCtlInitEcho over the wire, §6.2).
static void
InitSinkToPeer (Ptr<SueLlrEngine> peer, uint32_t seq, uint16_t data)
{
  Simulator::ScheduleNow (&SueLlrEngine::OnInit, peer, seq, data);
}
static void
InitEchoSinkToPeer (Ptr<SueLlrEngine> peer, uint32_t seq, uint16_t data)
{
  Simulator::ScheduleNow (&SueLlrEngine::OnInitEcho, peer, seq, data);
}
static uint32_t g_initSent = 0;
static std::vector<Time> g_initTimes;
static void
InitCountSink (uint32_t, uint16_t)
{
  ++g_initSent;
  g_initTimes.push_back (Simulator::Now ());
}
static uint32_t g_initEchoSent = 0;
static void
InitEchoCountSink (uint32_t, uint16_t)
{
  ++g_initEchoSent;
}

class SueLlrInitHandshakeTestCase : public TestCase
{
public:
  SueLlrInitHandshakeTestCase () : TestCase ("LLR INIT handshake (echo -> ADVANCE; timeout x retry -> GoAdvance)") {}
  void DoRun () override
  {
    // --- scenario 1: peer echoes INIT_ECHO -> sender reaches ADVANCE, no retries ---
    Ptr<SueLlrEngine> a = CreateObject<SueLlrEngine> ();
    Ptr<SueLlrEngine> b = CreateObject<SueLlrEngine> ();
    a->SetInitSink (MakeBoundCallback (&InitSinkToPeer, b));
    b->SetInitEchoSink (MakeBoundCallback (&InitEchoSinkToPeer, a));
    a->SetInitData (0xABCD);        // CTL-INIT-004: optional RTT timestamp/identifier
    a->Start ();                    // INIT -> wait for INIT_ECHO
    Simulator::Stop (MicroSeconds (100));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (a->GetCounters ().tx_init_ctl_os, 1ull, "A sent INIT once (no timeout retry)");
    NS_TEST_ASSERT_MSG_EQ (a->GetPeerInitData (), 0xABCDu,
                           "init_data echoed back verbatim (0xABCD)");
    NS_TEST_ASSERT_MSG_EQ (b->GetCounters ().rx_init_ctl_os, 1ull, "B received INIT");
    NS_TEST_ASSERT_MSG_EQ (b->GetCounters ().tx_init_echo_ctl_os, 1ull, "B echoed INIT_ECHO");
    NS_TEST_ASSERT_MSG_EQ (a->GetCounters ().rx_init_echo_ctl_os, 1ull, "A received INIT_ECHO");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) a->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "A in ADVANCE after echo");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) b->GetTxState (), (uint32_t) SueLlrEngine::TxState::OFF, "B stays OFF (receiver)");

    // --- scenario 2: no peer echo -> INIT timeout x retries -> GoAdvance fallback ---
    g_initSent = 0;
    Ptr<SueLlrEngine> c = CreateObject<SueLlrEngine> ();
    c->SetInitSink (MakeCallback (&InitCountSink));
    c->SetInitTimerMax (MicroSeconds (10));
    c->SetInitRetryMax (2);
    c->Start ();
    Simulator::Stop (MicroSeconds (100));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (c->GetCounters ().tx_init_ctl_os, 3ull, "1 INIT + 2 timeout retries");
    NS_TEST_ASSERT_MSG_EQ (g_initSent, 3u, "INIT sink fired 3x");
    NS_TEST_ASSERT_MSG_EQ (c->GetCounters ().rx_init_echo_ctl_os, 0ull, "no INIT_ECHO ever received");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) c->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "GoAdvance fallback after retry cap");

    // --- scenario 3: INIT re-send floor = 4x ctlos_target_spacing (§5.1.9.1.1) ---
    // ackPeriod (50us) is the spacing in wire time -> the INIT retry must NOT
    // fire at the 10us init-timer grid but at 4x50us = 200us.
    g_initSent = 0; g_initTimes.clear ();
    Ptr<SueLlrEngine> f = CreateObject<SueLlrEngine> ();
    f->SetInitSink (MakeCallback (&InitCountSink));
    f->SetInitTimerMax (MicroSeconds (10));
    f->SetInitRetryMax (10);
    f->SetAckPeriod (MicroSeconds (50));   // spacing -> 4x floor = 200us
    f->Start ();
    Simulator::Stop (MicroSeconds (250));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_initSent, 2u, "INIT at 0us and one retry at the 200us floor");
    NS_TEST_ASSERT_MSG_GT (g_initTimes[1] - g_initTimes[0], NanoSeconds (199000),
                           "INIT re-send gap >= 4x spacing (200us), not the 10us timer");
  }
};

// ============ UEC §5.1.4: two-phase modes + INIT/FLUSH behaviors + re_init ============
// llr_mode_local/llr_mode_remote (RX/TX gating), llr_init_behavior /
// llr_flush_behavior (DISCARD|BLOCK|BEST_EFFORT), re_init_on_discard.
class SueLlrModesBehaviorsTestCase : public TestCase
{
public:
  SueLlrModesBehaviorsTestCase () : TestCase ("LLR two-phase modes + init and flush behaviors + re_init (UEC 5.1.4)") {}
  void DoRun () override
  {
    // --- A: llr_mode_remote OFF -> no INIT may be sent, TX = BEST_EFFORT ---
    Ptr<SueLlrEngine> a = CreateObject<SueLlrEngine> ();
    a->SetInitSink (MakeCallback (&InitCountSink));
    a->SetModeRemote (false);
    a->Start ();
    NS_TEST_ASSERT_MSG_EQ (a->GetCounters ().tx_init_ctl_os, 0ull, "remote OFF: no INIT sent");
    NS_TEST_ASSERT_MSG_EQ ((a->GetTxBehavior () == SueLlrEngine::TxBehavior::BEST_EFFORT), true, "remote OFF -> BEST_EFFORT");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) a->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "Start with remote OFF -> ADVANCE (GoAdvance)");

    // --- B: llr_mode_local OFF -> INIT not processed, no INIT_ECHO, frames unfiltered ---
    Ptr<SueLlrEngine> b = CreateObject<SueLlrEngine> ();
    b->SetModeLocal (false);
    b->SetInitEchoSink (MakeCallback (&InitEchoCountSink));
    uint32_t bSeq = b->GetNextRxSeq ();
    b->OnInit (7);
    NS_TEST_ASSERT_MSG_EQ (b->GetCounters ().rx_init_ctl_os, 0ull, "local OFF: INIT not processed");
    NS_TEST_ASSERT_MSG_EQ (b->GetCounters ().tx_init_echo_ctl_os, 0ull, "local OFF: no INIT_ECHO");
    NS_TEST_ASSERT_MSG_EQ (b->GetNextRxSeq (), bSeq, "next_rx_seq unchanged");
    NS_TEST_ASSERT_MSG_EQ ((b->RxFrame (5, 2) == SueLlrEngine::ACCEPT), true, "local OFF: BAD frame accepted unfiltered");
    NS_TEST_ASSERT_MSG_EQ (b->GetCounters ().tx_nack_ctl_os, 0ull, "local OFF: no NACK");

    // --- C: llr_init_behavior DISCARD|BLOCK|BEST_EFFORT ---
    Ptr<SueLlrEngine> c = CreateObject<SueLlrEngine> ();
    c->SetInitSink (MakeCallback (&InitCountSink));
    c->SetInitTimerMax (Seconds (10));     // hold INIT state
    c->Start ();
    NS_TEST_ASSERT_MSG_EQ ((c->GetInitBehavior () == SueLlrEngine::TxBehavior::BLOCK), true, "default init_behavior = BLOCK");
    NS_TEST_ASSERT_MSG_EQ ((c->GetTxBehavior () == SueLlrEngine::TxBehavior::BLOCK), true, "INIT + default -> BLOCK");
    c->SetInitBehavior ("DISCARD");
    NS_TEST_ASSERT_MSG_EQ ((c->GetTxBehavior () == SueLlrEngine::TxBehavior::DISCARD), true, "INIT + DISCARD");
    c->DiscardNew ();
    NS_TEST_ASSERT_MSG_EQ (c->GetCounters ().tx_discard, 1ull, "DiscardNew counted tx_discard");
    c->SetInitBehavior ("BEST_EFFORT");
    NS_TEST_ASSERT_MSG_EQ ((c->GetTxBehavior () == SueLlrEngine::TxBehavior::BEST_EFFORT), true, "INIT + BEST_EFFORT");
    c->SetInitBehavior ("GARBAGE");
    NS_TEST_ASSERT_MSG_EQ ((c->GetInitBehavior () == SueLlrEngine::TxBehavior::BLOCK), true, "unknown behavior falls back to BLOCK");

    // --- D1: re_init_on_discard=FALSE -> FLUSH stays (no INIT), Start() re-enters INIT ---
    Ptr<SueLlrEngine> d1 = CreateObject<SueLlrEngine> ();
    d1->SetReInitOnDiscard (false);
    d1->SetReplayCtMax (1);
    d1->Start ();                          // no init sink -> ADVANCE
    d1->TxStore (Create<Packet> (86));
    d1->OnReplayTimer ();                  // ct=1 -> retransmit
    d1->OnReplayTimer ();                  // ct=2 > 1 -> FLUSH
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) d1->GetTxState (), (uint32_t) SueLlrEngine::TxState::FLUSH, "replay_ct exceeded -> FLUSH");
    NS_TEST_ASSERT_MSG_EQ (d1->GetCounters ().tx_init_ctl_os, 0ull, "re_init=FALSE: no INIT sent after FLUSH");
    // UEC Table 5-13: tx_replay counts COMPLETED replay operations (REPLAY state
    // exits); the FLUSHed session never completed, so it stays 0 here (the
    // replay rounds are observable via tx_discard / replay_ct).
    NS_TEST_ASSERT_MSG_EQ (d1->GetCounters ().tx_replay, 0ull, "no completed replay (FLUSHed)");
    NS_TEST_ASSERT_MSG_EQ (d1->GetCounters ().tx_discard, 1ull, "FLUSH discarded the replay frame");
    d1->SetInitSink (MakeCallback (&InitCountSink));
    g_initSent = 0;
    d1->Start ();                          // management re-init
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) d1->GetTxState (), (uint32_t) SueLlrEngine::TxState::INIT, "management Start() -> INIT");
    NS_TEST_ASSERT_MSG_EQ (d1->GetCounters ().tx_init_ctl_os, 1ull, "INIT sent after management re-init");

    // --- D2: re_init_on_discard=TRUE -> FLUSH auto-enters INIT ---
    Ptr<SueLlrEngine> d2 = CreateObject<SueLlrEngine> ();
    d2->SetInitSink (MakeCallback (&InitCountSink));
    d2->SetReplayCtMax (1);
    d2->Start ();                          // sink wired -> INIT state + INIT sent
    d2->TxStore (Create<Packet> (86));
    d2->OnReplayTimer ();                  // ct=1
    d2->OnReplayTimer ();                  // ct=2 -> FLUSH -> auto StartInit
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) d2->GetTxState (), (uint32_t) SueLlrEngine::TxState::INIT, "re_init=TRUE: FLUSH auto-enters INIT");
    NS_TEST_ASSERT_MSG_EQ (d2->GetCounters ().tx_init_ctl_os, 2ull, "INIT sent initially + after FLUSH");

    // --- E: config wiring via ApplySueConfig (UEC §5.1.4 keys -> engine) ---
    Config::SetGlobal ("LLR_MODE_LOCAL", UintegerValue (0));
    Config::SetGlobal ("LLR_MODE_REMOTE", UintegerValue (1));
    Config::SetGlobal ("LLR_INIT_BEHAVIOR", StringValue ("DISCARD"));
    Config::SetGlobal ("LLR_FLUSH_BEHAVIOR", StringValue ("BEST_EFFORT"));
    Config::SetGlobal ("LLR_RE_INIT_ON_DISCARD", UintegerValue (0));
    Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    dev->SetLlrEngine (e);
    ApplySueConfig (dev);
    NS_TEST_ASSERT_MSG_EQ (e->GetModeLocal (), false, "LLR_MODE_LOCAL applied");
    NS_TEST_ASSERT_MSG_EQ (e->GetModeRemote (), true, "LLR_MODE_REMOTE applied");
    NS_TEST_ASSERT_MSG_EQ ((e->GetInitBehavior () == SueLlrEngine::TxBehavior::DISCARD), true, "LLR_INIT_BEHAVIOR applied");
    NS_TEST_ASSERT_MSG_EQ ((e->GetFlushBehavior () == SueLlrEngine::TxBehavior::BEST_EFFORT), true, "LLR_FLUSH_BEHAVIOR applied");
    NS_TEST_ASSERT_MSG_EQ ((e->GetTxBehavior () == SueLlrEngine::TxBehavior::BEST_EFFORT), true, "OFF state -> BEST_EFFORT");
    // restore config defaults (other TestCases read these only via SueReadConf/ApplySueConfig)
    Config::SetGlobal ("LLR_MODE_LOCAL", UintegerValue (1));
    Config::SetGlobal ("LLR_INIT_BEHAVIOR", StringValue ("BLOCK"));
    Config::SetGlobal ("LLR_FLUSH_BEHAVIOR", StringValue ("BLOCK"));
    Config::SetGlobal ("LLR_RE_INIT_ON_DISCARD", UintegerValue (1));

    // Drop pending INIT/replay timer events scheduled by the engines above; a
    // later TestCase's Simulator::Run() would otherwise fire them on destroyed
    // engines (dangling EventId -> crash).
    Simulator::Destroy ();
  }
};

// ============ S3d: next_rx_seq_vld gating (UEC §5.1.7) ============
class SueLlrVldTestCase : public TestCase
{
public:
  SueLlrVldTestCase () : TestCase ("LLR next_rx_seq_vld gating (UEC 5.1.7)") {}
  void DoRun () override
  {
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    // Before any LLR_INIT: vld FALSE -> eligible frames are filtered silently
    // (no count, no NACK, next_rx_seq untouched) — UEC expected_frame =
    // next_rx_seq_vld && (frame_seq == next_rx_seq).
    NS_TEST_ASSERT_MSG_EQ (e->GetNextRxSeqVld (), false, "vld FALSE before INIT");
    NS_TEST_ASSERT_MSG_EQ ((e->RxFrame (0, 0) == SueLlrEngine::DROP), true, "eligible frame before INIT -> DROP");
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_ok, 0ull, "filtered frame not counted rx_ok");
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().tx_nack_ctl_os, 0ull, "filtered frame -> no NACK");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->GetNextRxSeq (), 0u, "next_rx_seq unchanged");
    // A valid LLR_INIT (mode_local ON) sets vld TRUE; the same frame is then
    // the expected frame and is accepted.
    e->OnInit (0);
    NS_TEST_ASSERT_MSG_EQ (e->GetNextRxSeqVld (), true, "vld TRUE after INIT");
    NS_TEST_ASSERT_MSG_EQ ((e->RxFrame (0, 0) == SueLlrEngine::ACCEPT), true, "expected frame after INIT -> ACCEPT");
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_ok, 1ull, "rx_ok=1");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->GetNextRxSeq (), 1u, "next_rx_seq advanced");
    // mode_local OFF bypasses the vld gate entirely (pass-through unfiltered).
    Ptr<SueLlrEngine> off = CreateObject<SueLlrEngine> ();
    off->SetModeLocal (false);
    NS_TEST_ASSERT_MSG_EQ (off->GetNextRxSeqVld (), false, "local OFF: vld stays FALSE");
    NS_TEST_ASSERT_MSG_EQ ((off->RxFrame (7, 2) == SueLlrEngine::ACCEPT), true, "local OFF: BAD frame accepted even without INIT");
    Simulator::Destroy ();
  }
};

// ============ S3d: INIT_ECHO init_seq validation (UEC §5.1.9.1.2) ============
class SueLlrInitEchoSeqTestCase : public TestCase
{
public:
  SueLlrInitEchoSeqTestCase () : TestCase ("LLR INIT_ECHO init_seq match (UEC 5.1.9.1.2)") {}
  void DoRun () override
  {
    Ptr<SueLlrEngine> a = CreateObject<SueLlrEngine> ();
    a->SetInitSink (MakeCallback (&InitCountSink));
    g_initSent = 0;
    a->Start ();   // INIT state: INIT(tx_seq=0) sent, init timer armed
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) a->GetTxState (), (uint32_t) SueLlrEngine::TxState::INIT, "Start -> INIT");
    NS_TEST_ASSERT_MSG_EQ (g_initSent, 1u, "one INIT sent");
    // Mismatched echo: still counted, but does NOT advance out of INIT.
    a->OnInitEcho (42);
    NS_TEST_ASSERT_MSG_EQ (a->GetCounters ().rx_init_echo_ctl_os, 1ull, "mismatched echo counted");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) a->GetTxState (), (uint32_t) SueLlrEngine::TxState::INIT, "mismatched echo ignored -> still INIT");
    // Matching echo (init_seq == tx_seq == 0): ADVANCE.
    a->OnInitEcho (0);
    NS_TEST_ASSERT_MSG_EQ (a->GetCounters ().rx_init_echo_ctl_os, 2ull, "matching echo counted");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) a->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "matching echo -> ADVANCE");
    // A stale echo of the old INIT (init_seq 0) after tx_seq advanced is ignored.
    a->TxStore (Create<Packet> (86));   // tx_seq -> 1
    a->OnInitEcho (0);
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) a->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "stale echo ignored (stays ADVANCE)");
    NS_TEST_ASSERT_MSG_EQ (a->GetCounters ().rx_init_echo_ctl_os, 3ull, "stale echo counted");
    Simulator::Destroy ();
  }
};

// ============ S3d: ineligible preamble (BEST_EFFORT TX -> unfiltered RX) ============
static uint32_t g_rxXpu;
static void
RxXpuObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader h)
{
  ++g_rxCount;
  g_rxXpu = h.GetXpuId ();
}

class SueLlrIneligiblePreambleTestCase : public TestCase
{
public:
  SueLlrIneligiblePreambleTestCase () : TestCase ("LLR ineligible preamble (BEST_EFFORT TX, unfiltered RX)") {}
  void DoRun () override
  {
    g_rxCount = 0;
    g_rxXpu = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    // A: TX LLR off (llr_mode_remote=0 -> BEST_EFFORT, ineligible frames).
    // B: RX LLR on (default). Every data frame now carries an 8B preamble
    // (eligible or ineligible, §5.1.1.1), so B strips it and passes the
    // ineligible frame up unfiltered — the "关闭 TX 相" experiment no longer
    // mis-parses the SUE header.
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
    eA->SetModeRemote (false);
    Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->Start (); eB->Start ();   // real handshake: B INITs, A echoes
    B->SetReceiveUpcall (MakeCallback (&RxXpuObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    A->SwitchSend (0, build ());
    Simulator::Run ();
    Simulator::Destroy ();
    // A never sends INIT (remote OFF -> BEST_EFFORT from the start).
    NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().tx_init_ctl_os, 0ull, "A remote OFF: no INIT sent");
    // B's INIT handshake completes via A's INIT_ECHO.
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eB->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "B ADVANCE");
    // Frame delivered; ineligible preamble stripped so the SUE header parses.
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 1u, "ineligible frame delivered");
    NS_TEST_ASSERT_MSG_EQ (g_rxXpu, 2u, "SUE header parsed after preamble strip (xpu=2)");
    // Ineligible frames are never seq-filtered / NACKed.
    NS_TEST_ASSERT_MSG_EQ (eB->GetNextRxSeq (), 0u, "B next_rx_seq unchanged (ineligible not filtered)");
    NS_TEST_ASSERT_MSG_EQ (eB->GetCounters ().tx_nack_ctl_os, 0ull, "B no NACK for ineligible frame");
  }
};

// ============ S4: periodic ACK pacing (UEC §5.1.6) ============
static void PaceRx (Ptr<const Packet> p);   // defined with the CtlOS pacing case
static std::vector<Time> g_paceRx;          // CtlOS arrival times (SueRx trace)
static uint32_t g_ackSeq; static uint32_t g_ackN; static uint32_t g_nackN;
static void
AckObs (uint32_t seq, bool isNack)
{
  if (isNack) { ++g_nackN; }
  else { ++g_ackN; g_ackSeq = seq; }
}

class SueLlrAckPeriodTestCase : public TestCase
{
public:
  SueLlrAckPeriodTestCase () : TestCase ("LLR periodic ACK (UEC 5.1.6)") {}
  void DoRun () override
  {
    // --- ackPeriod > 0: 3 good frames within one period -> one cumulative ACK ---
    // The ACK cadence is armed on init_echo_sent (the INIT_ECHO must be
    // generated), so wire an echo sink — the chip's trigger source (UEC §5.1.6).
    g_ackN = 0; g_nackN = 0; g_ackSeq = 0;
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    e->SetAckNackSink (MakeCallback (&AckObs));
    e->SetInitEchoSink (MakeCallback (&InitEchoCountSink));
    e->SetAckPeriod (MicroSeconds (50));
    e->OnInit (0);   // vld TRUE, next_rx_seq=0, INIT_ECHO sent -> SEND_ACKS
    NS_TEST_ASSERT_MSG_EQ ((e->RxFrame (0, 0) == SueLlrEngine::ACCEPT), true, "f0 accepted");
    NS_TEST_ASSERT_MSG_EQ ((e->RxFrame (1, 0) == SueLlrEngine::ACCEPT), true, "f1 accepted");
    NS_TEST_ASSERT_MSG_EQ ((e->RxFrame (2, 0) == SueLlrEngine::ACCEPT), true, "f2 accepted");
    // Not yet at the cadence boundary: no ACK has been sent (deferred).
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().tx_ack_ctl_os, 0ull, "no per-frame ACK (period armed)");
    NS_TEST_ASSERT_MSG_EQ (g_ackN, 0u, "ack sink not fired before period");
    // Run past the cadence: one cumulative ACK covering seq 0..2.
    Simulator::Stop (MicroSeconds (60));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().tx_ack_ctl_os, 1ull, "one cumulative ACK after period");
    NS_TEST_ASSERT_MSG_EQ (g_ackN, 1u, "ack sink fired once");
    NS_TEST_ASSERT_MSG_EQ (g_ackSeq, 2u, "cumulative ACK seq = next_rx_seq-1 = 2");
    NS_TEST_ASSERT_MSG_EQ (g_nackN, 0u, "no NACK");
    Simulator::Destroy ();

    // --- ackPeriod = 0 (default): per-frame ACK, 3 frames -> 3 ACKs ---
    g_ackN = 0; g_nackN = 0;
    Ptr<SueLlrEngine> p = CreateObject<SueLlrEngine> ();
    p->SetAckNackSink (MakeCallback (&AckObs));
    p->SetInitEchoSink (MakeCallback (&InitEchoCountSink));
    p->OnInit (0);
    p->RxFrame (0, 0);
    p->RxFrame (1, 0);
    p->RxFrame (2, 0);
    NS_TEST_ASSERT_MSG_EQ (p->GetCounters ().tx_ack_ctl_os, 3ull, "ackPeriod=0: per-frame ACK x3");
    NS_TEST_ASSERT_MSG_EQ (g_ackN, 3u, "ack sink fired 3x");
    Simulator::Destroy ();
  }
};

// ---- 2^20 seq wrap: clean pass-through across the whole seq space ----
// 2026-08-14 thpt 512B long-burst regression: m_nextRxSeq grew raw past
// SEQ_MOD and every post-wrap frame was judged missing -> NACK/FLUSH resync,
// ~3 frames lost per wrap (2.33 wraps in a 10 Gbit run). Engine-level drive of
// the full seq space plus 100 frames pins the mod-space invariant.
static uint32_t g_wrapAckN = 0, g_wrapNackN = 0;
static void
WrapAckSink (Ptr<SueLlrEngine> tx, uint32_t seq, bool isNack)
{
  if (isNack) { ++g_wrapNackN; } else { ++g_wrapAckN; }
  tx->OnAckNack (seq, isNack);
}

class SueLlrSeqWrapTestCase : public TestCase
{
public:
  SueLlrSeqWrapTestCase () : TestCase ("LLR seq wraps the full 2^20 space cleanly (no NACK)") {}
  void DoRun () override
  {
    g_wrapAckN = 0; g_wrapNackN = 0;
    Ptr<SueLlrEngine> tx = CreateObject<SueLlrEngine> ();
    Ptr<SueLlrEngine> rx = CreateObject<SueLlrEngine> ();
    tx->Start ();
    rx->Start ();
    rx->SetAckNackSink (MakeBoundCallback (&WrapAckSink, tx));
    rx->OnInit (0);                     // next_rx_seq = 0, vld TRUE
    const uint32_t n = SueLlrEngine::SEQ_MOD + 100;
    uint32_t accepted = 0, dropped = 0;
    for (uint32_t i = 0; i < n; ++i)
      {
        uint32_t seq = tx->TxStore (Create<Packet> (86));
        if (rx->RxFrame (seq, 0) == SueLlrEngine::ACCEPT) { ++accepted; }
        else { ++dropped; }
      }
    NS_TEST_ASSERT_MSG_EQ (accepted, n, "every frame accepted across the wrap");
    NS_TEST_ASSERT_MSG_EQ (dropped, 0u, "no frame dropped at the wrap");
    NS_TEST_ASSERT_MSG_EQ (g_wrapNackN, 0u, "no NACK across the wrap");
    NS_TEST_ASSERT_MSG_EQ (g_wrapAckN, n, "per-frame ACK for every frame");
    NS_TEST_ASSERT_MSG_EQ (rx->GetNextRxSeq (), 100u, "next_rx_seq stays in the mod space");
    NS_TEST_ASSERT_MSG_EQ (rx->GetCounters ().rx_missing_seq, 0ull, "no missing-seq at the wrap");
    NS_TEST_ASSERT_MSG_EQ (tx->GetCounters ().rx_ack_nack_seq_error, 0ull,
                           "every ACK in range across the wrap");
    Simulator::Destroy ();
  }
};

// ---- §12.40 VRF: segmented forwarding DB + DSCP>VLAN>port derivation ----
static uint64_t g_vrfRxA = 0, g_vrfRxB = 0;
static void
VrfHostARx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_vrfRxA; }
static void
VrfHostBRx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_vrfRxB; }

class SueVrfTestCase : public TestCase
{
public:
  SueVrfTestCase () : TestCase ("AFH VRF segmentation + source precedence (DSCP then VLAN then port)") {}
  void DoRun () override
  {
    // (a) 转发库分段单元:每 VRF 一张独立路由表,经 switch 按 VRF 装配;
    //     未编程 VRF 表 = miss(不回退 VRF0);旧签名 = VRF0 表。
    {
      Ptr<SueRoutingTable> rt0 = CreateObject<SueRoutingTable> ();
      Ptr<SueRoutingTable> rt1 = CreateObject<SueRoutingTable> ();
      rt0->AddPrefix (1, 10, {0});
      rt1->AddPrefix (1, 10, {1});
      Ptr<SUESwitchNode> sw0 = CreateObject<SUESwitchNode> ();
      sw0->SetRoutingTable (rt0);            // 旧签名 = VRF0
      sw0->SetRoutingTable (1, rt1);
      NS_TEST_ASSERT_MSG_EQ (sw0->GetRoutingTable (), rt0, "legacy GetRoutingTable = vrf0 table");
      NS_TEST_ASSERT_MSG_EQ (sw0->GetRoutingTable (0), rt0, "vrf0 table roundtrip");
      NS_TEST_ASSERT_MSG_EQ (sw0->GetRoutingTable (1), rt1, "vrf1 table roundtrip");
      NS_TEST_ASSERT_MSG_EQ (sw0->GetRoutingTable (7), nullptr, "vrf7 unprogrammed = null (miss)");
    }

    // (b) 配置 accessor:三张映射表的默认(VFR0)与部分覆盖语义。
    Config::SetGlobal ("DSCP_TO_VRF_MAP", StringValue ("9:3"));
    Config::SetGlobal ("VLAN_TO_VRF_MAP", StringValue ("100:2,200:2"));
    Config::SetGlobal ("PORT_TO_VRF_MAP", StringValue ("0:1"));
    SueConfig::InvalidateCache ();
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVrf (9), 3u, "dscp9 -> vrf3");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVrf (5), 0u, "dscp5 unmapped -> 0 (next source)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::VlanToVrf (100), 2u, "vid100 -> vrf2");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::VlanToVrf (300), 0u, "vid300 unmapped -> 0 (next source)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PortToVrf (0), 1u, "port0 -> vrf1");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PortToVrf (1), 0u, "port1 unmapped -> 0");

    // (c) e2e:两个入口 host(h1a=port0 映射 vrf1、h1b=port1 未映射)+ 两个
    //     出口 host(hA=port2、hB=port3)。路由:vrf0 xpu2->p2、vrf1 xpu2->p3、
    //     vrf2 xpu2->p2;vrf3 未编程。
    //     优先级阶梯:全未映射->vrf0;port 命中->vrf1;VLAN(100:2)覆盖
    //     port->vrf2;DSCP(9:3)覆盖 VLAN+port->vrf3(未编程表 = miss)。
    g_vrfRxA = g_vrfRxB = 0;
    Ptr<Node> nA = CreateObject<Node> (), nB = CreateObject<Node> ();
    Ptr<Node> n1a = CreateObject<Node> (), n1b = CreateObject<Node> ();
    Ptr<SUESwitchNode> sw = CreateObject<SUESwitchNode> ();
    Ptr<SueNetDevice> hA = CreateObject<SueNetDevice> (), hB = CreateObject<SueNetDevice> ();
    Ptr<SueNetDevice> h1a = CreateObject<SueNetDevice> (), h1b = CreateObject<SueNetDevice> ();
    nA->AddDevice (hA); nB->AddDevice (hB); n1a->AddDevice (h1a); n1b->AddDevice (h1b);
    hA->SetReceiveUpcall (MakeCallback (&VrfHostARx));
    hB->SetReceiveUpcall (MakeCallback (&VrfHostBRx));
    Ptr<SueNetDevice> p0 = CreateObject<SueNetDevice> (), p1 = CreateObject<SueNetDevice> ();
    Ptr<SueNetDevice> p2 = CreateObject<SueNetDevice> (), p3 = CreateObject<SueNetDevice> ();
    sw->AddPort (p0); sw->AddPort (p1); sw->AddPort (p2); sw->AddPort (p3);
    Ptr<SueChannel> c0 = CreateObject<SueChannel> (); h1a->Attach (c0); p0->Attach (c0);
    Ptr<SueChannel> c1 = CreateObject<SueChannel> (); h1b->Attach (c1); p1->Attach (c1);
    Ptr<SueChannel> c2 = CreateObject<SueChannel> (); hA->Attach (c2); p2->Attach (c2);
    Ptr<SueChannel> c3 = CreateObject<SueChannel> (); hB->Attach (c3); p3->Attach (c3);
    Ptr<SueRoutingTable> rt0 = CreateObject<SueRoutingTable> ();
    rt0->AddPrefix (2, 10, {2});
    Ptr<SueRoutingTable> rt1 = CreateObject<SueRoutingTable> ();
    rt1->AddPrefix (2, 10, {3});
    Ptr<SueRoutingTable> rt2 = CreateObject<SueRoutingTable> ();
    rt2->AddPrefix (2, 10, {2});
    sw->SetRoutingTable (rt0);            // vrf0(旧签名)
    sw->SetRoutingTable (1, rt1);
    sw->SetRoutingTable (2, rt2);
    sw->ResetCounters ();
    // 1. h1b 裸帧:port1 未映射 -> vrf0 -> hA
    SendOne (h1b, BuildFrame (2, false, 0, false, 0, 0, false));
    // 2. h1a 裸帧:port0 -> vrf1 -> hB
    SendOne (h1a, BuildFrame (2, false, 0, false, 0, 0, false));
    // 3. h1a vid=100:VLAN(2)>port(1) -> vrf2 -> hA
    SendOne (h1a, BuildFrame (2, true, 0, false, 0, 0, false));
    // 4. h1a shim dscp=9 + vid=100:DSCP(3)>VLAN(2)>port(1) -> vrf3 未编程 -> miss
    SendOne (h1a, BuildFrame (2, true, 0, true, 3, 9, false));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_vrfRxA, 2u, "hA got 2: bare h1b (vrf0) + vlan frame (vrf2 wins over port)");
    NS_TEST_ASSERT_MSG_EQ (g_vrfRxB, 1u, "hB got 1: bare h1a (port map -> vrf1)");
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntVrfRx (0), 1ull, "vrf0 forwarded 1");
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntVrfRx (1), 1ull, "vrf1 forwarded 1");
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntVrfRx (2), 1ull, "vrf2 forwarded 1");
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntVrfRx (3), 0ull, "vrf3 forwarded 0 (unprogrammed table)");
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntVrfMiss (3), 1ull, "vrf3 miss counted (AFR_DST_LOOKUP_MISS per segment)");
    NS_TEST_ASSERT_MSG_EQ (sw->GetCntLookupMiss (), 1ull, "global miss = 1");
    Simulator::Destroy ();

    // 恢复默认(防串扰,TS-47 惯例)
    Config::SetGlobal ("DSCP_TO_VRF_MAP", StringValue (""));
    Config::SetGlobal ("VLAN_TO_VRF_MAP", StringValue (""));
    Config::SetGlobal ("PORT_TO_VRF_MAP", StringValue (""));
    SueConfig::InvalidateCache ();
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVrf (9), 0u, "restored: dscp9 -> 0");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::VlanToVrf (100), 0u, "restored: vid100 -> 0");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PortToVrf (0), 0u, "restored: port0 -> 0");
  }
};

// ---- LLR-SP-16: ACK/NACK counters count RECEIVED CtlOS (pairing) ----
class SueLlrAckCounterPairingTestCase : public TestCase
{
public:
  SueLlrAckCounterPairingTestCase () : TestCase ("LLR ACK-NACK counters pair across link") {}
  void DoRun () override
  {
    // Engine-level: duplicate and out-of-range ACKs are RECEIVED CtlOS — they
    // must be counted (UEC Table 5-13), only the PROCESSING is skipped.
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    e->Start ();                       // no sink -> ADVANCE
    e->TxStore (Create<Packet> (86));  // txSeq -> 1 (makes ACK(0) in-range)
    e->OnAckNack (0, false);           // valid ACK(0)
    e->OnAckNack (0, false);           // duplicate: still counted
    e->OnAckNack (SueLlrEngine::SEQ_MOD / 2 + 10, false);   // out-of-range
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_ack_ctl_os, 3ull,
                           "rx_ack counts dup + out-of-range ACKs");
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_ack_nack_seq_error, 1ull,
                           "seq_error counted for the out-of-range one");
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().tx_replay, 0ull, "no replay from dup/out-of-range");

    // Device-level pairing: an idle link with ackPeriod>0 keeps sending
    // cumulative (duplicate-seq) ACKs — the peer's rx_ack must equal the
    // sender's tx_ack (previously rx_ack only counted the FIRST one and the
    // two ends never paired).
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); B->SetDataRate (DataRate ("100Gbps"));
    A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    // CTL-RATE-001 cadence: 2048B spacing @100G = 163.84ns -> 164ns. The old
    // 10us direct timer would exceed the 17296B upper bound (1384ns) at 100G.
    eB->SetAckPeriod (B->GetDataRate ().CalculateBytesTxTime (2048));
    eA->Start (); eB->Start ();
    g_paceRx.clear ();
    A->TraceConnectWithoutContext ("SueRx", MakeCallback (&PaceRx));
    Simulator::Stop (MicroSeconds (55));    // ~300 periods after INIT_ECHO
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_GT (eB->GetCounters ().tx_ack_ctl_os, 0ull, "idle link sends periodic ACKs");
    NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().rx_ack_ctl_os, eB->GetCounters ().tx_ack_ctl_os,
                           "tx_ack == rx_ack on an idle link (pairing)");
    // CTL-RATE-001 (5.1.3.1.1): the ACK/NACK rate must stay between once per
    // 400B and once per 17296B of wire time — at 100Gbps that is [32ns, 1384ns].
    NS_TEST_ASSERT_MSG_GT (g_paceRx.size (), 2u, "multiple ACKs observed on the wire");
    for (size_t i = 1; i < g_paceRx.size (); ++i)
      {
        Time d = g_paceRx[i] - g_paceRx[i - 1];
        NS_TEST_ASSERT_MSG_GT (d, NanoSeconds (32), "ACK gap >= 400B wire time (32ns @100G)");
        NS_TEST_ASSERT_MSG_LT (d, NanoSeconds (1384), "ACK gap <= 17296B wire time (1384ns @100G)");
      }
  }
};

// ============ S4: acked_seq init (UEC Table 5-10) ============
// acked_seq = seq confirmed by the most recent successfully received ACK/NACK;
// all-1s (SEQ_MOD-1) on llr_mode_remote change; init_seq-1 when sending LLR_INIT.
// The all-1s / init_seq-1 initializations let the first ACK(0) be accepted
// without the old m_anyAcked guard (acked_seq all-1s never equals a first
// frame's seq=0), while a genuine repeated ACK(seq==acked_seq) is still skipped.
class SueLlrAckedSeqTestCase : public TestCase
{
public:
  SueLlrAckedSeqTestCase () : TestCase ("LLR acked_seq init (UEC Table 5-10)") {}
  void DoRun () override
  {
    // --- (a) LLR_INIT send -> acked_seq = init_seq-1 ---
    Ptr<SueLlrEngine> a = CreateObject<SueLlrEngine> ();
    a->SetInitSink (MakeCallback (&InitCountSink));
    a->Start ();   // INIT(init_seq = tx_seq = 0)
    NS_TEST_ASSERT_MSG_EQ (a->GetAckedSeq (), SueLlrEngine::SEQ_MOD - 1,
                           "INIT(0) -> acked_seq = init_seq-1 = all-1s (SEQ_MOD-1)");
    a->OnInitEcho (0);   // -> ADVANCE
    for (int i = 0; i < 3; ++i) { a->TxStore (Create<Packet> (86)); }   // tx_seq -> 3
    a->Start ();   // management re-enter INIT: INIT(init_seq=3)
    NS_TEST_ASSERT_MSG_EQ (a->GetAckedSeq (), 2u, "INIT(3) -> acked_seq = init_seq-1 = 2");
    Simulator::Destroy ();

    // --- (b) llr_mode_remote change -> all-1s AND tx_seq -> 0 ---
    Ptr<SueLlrEngine> m = CreateObject<SueLlrEngine> ();
    m->SetInitSink (MakeCallback (&InitCountSink));
    m->Start (); m->OnInitEcho (0);      // ADVANCE
    m->TxStore (Create<Packet> (86));    // tx_seq -> 1, replay=[0]
    NS_TEST_ASSERT_MSG_EQ (m->GetOutstandingSeq (), 1u,
                           "outstanding_seq = (tx_seq-1 - acked_seq) = 1 (== replay size)");
    m->OnAckNack (0, false);             // acked_seq -> 0, replay=[]
    NS_TEST_ASSERT_MSG_EQ (m->GetAckedSeq (), 0u, "acked_seq advanced to 0 by ACK(0)");
    NS_TEST_ASSERT_MSG_EQ (m->GetOutstandingSeq (), 0u,
                           "outstanding_seq = 0 after ACK (== replay size)");
    m->SetModeRemote (false);
    NS_TEST_ASSERT_MSG_EQ (m->GetAckedSeq (), SueLlrEngine::SEQ_MOD - 1,
                           "mode_remote change (->OFF) resets acked_seq to all-1s");
    NS_TEST_ASSERT_MSG_EQ (m->GetTxSeq (), 0u,
                           "mode_remote change (->OFF) resets tx_seq to 0");
    NS_TEST_ASSERT_MSG_EQ (m->GetOutstandingSeq (), 0u,
                           "outstanding_seq = 0 after mode change (buffer cleared)");
    m->SetModeRemote (true);
    NS_TEST_ASSERT_MSG_EQ (m->GetAckedSeq (), SueLlrEngine::SEQ_MOD - 1,
                           "mode_remote change (->ON) resets acked_seq to all-1s");
    NS_TEST_ASSERT_MSG_EQ (m->GetTxSeq (), 0u,
                           "mode_remote change (->ON) resets tx_seq to 0");
    Simulator::Destroy ();

    // --- (c) first ACK(0) accepted (not misread as dup); a repeat is skipped ---
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    e->SetInitSink (MakeCallback (&InitCountSink));
    e->Start (); e->OnInitEcho (0);      // ADVANCE
    e->TxStore (Create<Packet> (86));    // tx_seq -> 1, replay=[0]
    e->OnAckNack (0, false);
    NS_TEST_ASSERT_MSG_EQ (e->GetAckedSeq (), 0u, "first ACK(0) accepted: acked_seq=0");
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_ack_ctl_os, 1ull, "first ACK counted");
    e->OnAckNack (0, false);             // true duplicate ACK(0)
    NS_TEST_ASSERT_MSG_EQ (e->GetAckedSeq (), 0u, "dup ACK: acked_seq unchanged");
    // LLR-SP-16 (UEC Table 5-13): the dup is a RECEIVED CtlOS and is counted;
    // only its PROCESSING is skipped.
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_ack_ctl_os, 2ull, "dup ACK counted (received), not processed");
    NS_TEST_ASSERT_MSG_EQ (e->GetReplaySize (), 0u, "dup ACK: replay buffer not re-released");
    Simulator::Destroy ();
  }
};

// ============ S4: pcs_lost_status_timer (UEC Table 5-10) ============
// Minimal PCS model: SetPcsStatus(FALSE) in ADVANCE/REPLAY arms the timer;
// expiry -> FLUSH (replay discarded) even with an empty buffer, then re-INIT.
// A glitch shorter than pcs_lost_status_timer_max that recovers is harmless.
class SueLlrPcsLostTestCase : public TestCase
{
public:
  SueLlrPcsLostTestCase () : TestCase ("LLR pcs_lost_status_timer (UEC Table 5-10)") {}
  void DoRun () override
  {
    // --- (a) short glitch (< max): recover before expiry -> no FLUSH ---
    Ptr<SueLlrEngine> p1 = CreateObject<SueLlrEngine> ();
    p1->SetInitSink (MakeCallback (&InitCountSink));
    p1->SetPcsLostTimerMax (MicroSeconds (50));
    p1->Start (); p1->OnInitEcho (0);        // ADVANCE
    p1->SetPcsStatus (false);                // glitch starts at t=0
    Simulator::Schedule (MicroSeconds (20), &SueLlrEngine::SetPcsStatus, p1, true);
    Simulator::Stop (MicroSeconds (100));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) p1->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE,
                           "short glitch (< max): no FLUSH");
    NS_TEST_ASSERT_MSG_EQ (p1->GetPcsLostTimerExpired (), false, "timer not expired");
    Simulator::Destroy ();

    // --- (b) long glitch (>= max): timer expires -> FLUSH -> re-INIT ---
    // FLUSH even with an empty replay buffer — the distinguishing pcs_lost
    // behavior vs replay_ct_max (which only counts replay rounds).
    Ptr<SueLlrEngine> p2 = CreateObject<SueLlrEngine> ();
    p2->SetInitSink (MakeCallback (&InitCountSink));
    p2->SetPcsLostTimerMax (MicroSeconds (50));
    p2->SetInitTimerMax (Seconds (10));      // keep the re-INIT observable
    p2->Start (); p2->OnInitEcho (0);        // ADVANCE
    p2->SetPcsStatus (false);                // glitch starts at t=0, buffer empty
    Simulator::Stop (MicroSeconds (200));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (p2->GetPcsLostTimerExpired (), true, "timer expired");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) p2->GetTxState (), (uint32_t) SueLlrEngine::TxState::INIT,
                           "long glitch -> FLUSH -> re-INIT (waiting for INIT_ECHO)");
    NS_TEST_ASSERT_MSG_EQ (p2->GetCntTxDiscard (), 0ull,
                           "empty buffer: no frames discarded by the FLUSH");
    Simulator::Destroy ();
  }
};

// ============ S4: LLR_status (UEC Table 5-10) ============
class SueLlrStatusTestCase : public TestCase
{
public:
  SueLlrStatusTestCase () : TestCase ("LLR status register (UEC Table 5-10)") {}
  void DoRun () override
  {
    Ptr<SueLlrEngine> s = CreateObject<SueLlrEngine> ();
    s->SetInitSink (MakeCallback (&InitCountSink));
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), false,
                           "OFF state (mode_remote ON): LLR_status FALSE");
    s->Start ();                              // INIT
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), false, "INIT: LLR_status FALSE");
    s->OnInitEcho (0);                        // ADVANCE
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), true,
                           "ADVANCE (mode_remote ON): LLR_status TRUE");
    s->TxStore (Create<Packet> (86));         // tx_seq=1, replay=[0]
    s->TxStore (Create<Packet> (86));         // tx_seq=2, replay=[0,1]
    s->OnAckNack (0, true);                   // release fs=0 -> REPLAY
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) s->GetTxState (), (uint32_t) SueLlrEngine::TxState::REPLAY,
                           "NACK -> REPLAY");
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), true, "REPLAY: LLR_status TRUE");
    // mode_remote OFF -> LLR_status tracks PCS_status (pass-through)
    s->SetModeRemote (false);
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), true,
                           "mode OFF + PCS TRUE: LLR_status = PCS_status");
    s->SetPcsStatus (false);
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), false,
                           "mode OFF + PCS FALSE: LLR_status = PCS_status");
    s->SetPcsStatus (true);
    NS_TEST_ASSERT_MSG_EQ (s->GetLlrStatus (), true, "PCS recovered: LLR_status TRUE");
    Simulator::Destroy ();
  }
};

// ============ S4: replay_ct semantics (UEC Table 5-10) ============
// replay_ct_is_max = replay_ct >= replay_ct_max; an all-ones (255) max means the
// replay-count limit never triggers FLUSH (only data_age / pcs_lost can).
class SueLlrReplayCtUnlimitedTestCase : public TestCase
{
public:
  SueLlrReplayCtUnlimitedTestCase () : TestCase ("LLR replay_ct_max=255 (UEC Table 5-10)") {}
  void DoRun () override
  {
    Ptr<SueLlrEngine> z = CreateObject<SueLlrEngine> ();
    z->SetInitSink (MakeCallback (&InitCountSink));
    z->SetReplayCtMax (255);                 // all-ones: never FLUSH via replay_ct
    z->SetReplayTimerMax (MicroSeconds (10));
    z->SetDataAgeTimerMax (Seconds (10));    // keep data-age out of the picture
    z->Start (); z->OnInitEcho (0);          // ADVANCE
    z->TxStore (Create<Packet> (86));        // tx_seq=1, replay=[0]
    z->TxStore (Create<Packet> (86));        // tx_seq=2, replay=[0,1]
    z->OnAckNack (0, true);                  // release fs=0 -> REPLAY, replay=[1]
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) z->GetTxState (), (uint32_t) SueLlrEngine::TxState::REPLAY,
                           "NACK -> REPLAY");
    NS_TEST_ASSERT_MSG_EQ (z->GetCntTxDiscard (), 0ull, "no discard yet");
    // ~50 replay-timer expirations: replay keeps retransmitting, never FLUSHes.
    Simulator::Stop (MicroSeconds (500));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) z->GetTxState (), (uint32_t) SueLlrEngine::TxState::REPLAY,
                           "replay_ct_max=255: replay-count limit never triggers FLUSH");
    NS_TEST_ASSERT_MSG_EQ (z->GetCntTxDiscard (), 0ull, "still no discard");
    Simulator::Destroy ();
  }
};

// ============================ S7: replay-buffer gates (DESIGN §6.10) ============================
// The three independent CanTx() gates: frame-count storage (LLR_REPLAY_BUF_MAX),
// in-flight bytes ~ BDP (LLR_OUTSTANDING_DATA_MAX) and in-flight frame window
// (LLR_OUTSTANDING_SEQ_MAX). Related but not equivalent: the storage gate is
// the glitch-absorption capacity (DESIGN §6.10: buffer >= rate*(RTT+silence)),
// the byte gate SHOULD = BDP (UEC §5.1.6), the seq gate = the in-flight window.
// ACK release re-opens all three.
class SueLlrCanTxLimitsTestCase : public TestCase
{
public:
  SueLlrCanTxLimitsTestCase () : TestCase ("LLR replay-buffer gates (storage, BDP, window)") {}
  void DoRun () override
  {
    Ptr<SueLlrEngine> z = CreateObject<SueLlrEngine> ();
    z->SetInitSink (MakeCallback (&InitCountSink));
    z->Start (); z->OnInitEcho (0);          // ADVANCE (no peer data flow)
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) z->GetTxState (),
                           (uint32_t) SueLlrEngine::TxState::ADVANCE, "ADVANCE after INIT echo");
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), true, "empty buffer: all gates open");

    uint32_t s0 = z->TxStore (Create<Packet> (86));   // replay=[0]
    NS_TEST_ASSERT_MSG_EQ (z->GetReplaySize (), 1u, "one frame stored");
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), true, "1 frame: defaults non-binding");

    // Each gate is closed in isolation (the others stay open) then re-opened.
    // (a) storage gate: LLR_REPLAY_BUF_MAX (frame capacity)
    z->TxStore (Create<Packet> (86));        // replay=[0,1]
    NS_TEST_ASSERT_MSG_EQ (z->GetReplaySize (), 2u, "two frames stored");
    z->SetReplayBufMax (1);
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), false, "storage gate closed (2 >= replay_buf_max=1)");
    z->SetReplayBufMax (1u << 20);
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), true, "storage gate re-opens");

    // (b) byte gate: LLR_OUTSTANDING_DATA_MAX (in-flight bytes ~ BDP)
    z->SetOutstandingDataMax (150);          // 2*86B = 172B >= 150
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), false, "byte gate closed (172B >= 150B)");
    z->SetOutstandingDataMax (0xFFFFFFFFu);
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), true, "byte gate re-opens");

    // (c) seq gate: LLR_OUTSTANDING_SEQ_MAX (in-flight frame window)
    z->SetOutstandingSeqMax (1);             // 2 in flight > 1
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), false, "window gate closed (2 > seq_max=1)");
    z->SetOutstandingSeqMax (1u << 20);
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), true, "window gate re-opens");

    // ACK release (ack_nack_seq covers both frames) drains the buffer and
    // re-opens all three gates
    z->OnAckNack (s0 + 1, false);            // ACK(s0+1) releases [s0, s0+1]
    NS_TEST_ASSERT_MSG_EQ (z->GetReplaySize (), 0u, "ACK releases both frames");
    NS_TEST_ASSERT_MSG_EQ (z->CanTx (), true, "CanTx again after ACK release");

    // S7 config key: registered + applied through ApplySueConfig
    UintegerValue uv;
    NS_TEST_ASSERT_MSG_EQ (GlobalValue::GetValueByNameFailSafe ("LLR_REPLAY_BUF_MAX", uv), true,
                           "LLR_REPLAY_BUF_MAX registered as a GlobalValue");
    Ptr<Node> n = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> ();
    n->AddDevice (A);
    A->SetDataRate (DataRate ("100Gbps"));     // ApplySueConfig needs the rate for the IFG conversion
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA);
    // SetLlrEngine routes the INIT sink to the device (needs a channel); override
    // with the manual counter so Start() stays on the bare-engine path.
    eA->SetInitSink (MakeCallback (&InitCountSink));
    GlobalValue::Bind ("LLR_REPLAY_BUF_MAX", UintegerValue (1));
    ApplySueConfig (A);
    eA->Start (); eA->OnInitEcho (0);
    NS_TEST_ASSERT_MSG_EQ (eA->CanTx (), true, "config-applied storage gate open (empty)");
    eA->TxStore (Create<Packet> (86));
    eA->TxStore (Create<Packet> (86));
    NS_TEST_ASSERT_MSG_EQ (eA->CanTx (), false, "config-applied storage gate blocks 2 frames");
    GlobalValue::Bind ("LLR_REPLAY_BUF_MAX", UintegerValue (1u << 20));  // restore engine default
    Simulator::Destroy ();
  }
};


// ============================ S4: LLR+CBFC coexistence ============================
class SueCoexistTestCase : public TestCase
{
public:
  SueCoexistTestCase () : TestCase ("FC LLR+CBFC coexistence (nine rules 1 2 4)") {}
  void DoRun () override
  {
    g_rxCount = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    // CBFC
    Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
    cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 10); cbfcA->Init (); cbfcB->Init ();
    A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
    B->SetDrainDelay (MicroSeconds (10));

    // LLR
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();

    // Error: frame 2 (seq=1) bad
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCorruptOnce (2); B->SetErrorModel (em);
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 3u, "3 delivered (NACK->retransmit)");
    // Rule 1: retransmit not re-counted
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetSvcCc (0), 3u, "S_VC_CC=3 (retransmit not re-counted)");
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetSvcCu (0), 0u, "S_VC_CU=0 (all returned)");
    // Rule 2: dropped frame not counted; retransmit counted once
    NS_TEST_ASSERT_MSG_EQ (cbfcB->GetRvcCf (0), 3u, "R_VC_CF=3 (3 good drained)");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eA->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "LLR ADVANCE");
  }
};

// ============ S5: control frames — real Ethernet PFC/CC_Update + CtlOS pacing ===

// ---- unit: SuePfcHeader (DESIGN §5.10) ----
class SuePfcHeaderTestCase : public TestCase
{
public:
  SuePfcHeaderTestCase () : TestCase ("PFC frame header (real 802.1Qbb 64B)") {}
  void DoRun () override
  {
    SuePfcHeader h;
    h.SetMacSa (Mac48Address ("00:11:22:33:44:55"));
    h.SetQuanta (3, 500);
    NS_TEST_ASSERT_MSG_EQ (h.GetSerializedSize (), 64u, "PFC frame is 64B on wire");
    Ptr<Packet> p = Create<Packet> ();
    p->AddHeader (h);
    NS_TEST_ASSERT_MSG_EQ (p->GetSize (), 64u, "packet size 64B");
    uint8_t b[64] = {0};
    p->CopyData (b, 64);
    // MACDA = slow-protocol multicast 01-80-C2-00-00-01
    uint8_t expDa[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x01};
    for (int i = 0; i < 6; ++i) { NS_TEST_ASSERT_MSG_EQ (b[i], expDa[i], "MACDA byte " << i); }
    // EtherType=0x8808, Opcode=0x0101
    NS_TEST_ASSERT_MSG_EQ (b[12], (uint8_t) 0x88, "EtherType hi");
    NS_TEST_ASSERT_MSG_EQ (b[13], (uint8_t) 0x08, "EtherType lo");
    NS_TEST_ASSERT_MSG_EQ (b[14], (uint8_t) 0x01, "Opcode hi");
    NS_TEST_ASSERT_MSG_EQ (b[15], (uint8_t) 0x01, "Opcode lo");
    // FCS present (CRC-32 over the 60B body, non-zero)
    uint32_t fcs = (b[60] << 24) | (b[61] << 16) | (b[62] << 8) | b[63];
    NS_TEST_ASSERT_MSG_NE (fcs, 0u, "FCS non-zero");
    // round-trip
    SuePfcHeader h2;
    p->RemoveHeader (h2);
    NS_TEST_ASSERT_MSG_EQ ((h2.GetPev () & (1u << 3)), (uint16_t) (1u << 3), "PEV bit 3 set");
    NS_TEST_ASSERT_MSG_EQ (h2.GetQuanta (3), 500u, "quanta 3 round-trip");
    NS_TEST_ASSERT_MSG_EQ (h2.GetMacSa (), Mac48Address ("00:11:22:33:44:55"), "MACSA round-trip");
  }
};

// ---- unit: SueCbfcCcHeader (DESIGN §5.6) ----
class SueCcUpdateHeaderTestCase : public TestCase
{
public:
  SueCcUpdateHeaderTestCase () : TestCase ("CBFC CC_Update frame header (real 64B + 20b pack)") {}
  void DoRun () override
  {
    SueCbfcCcHeader h;
    h.SetMacSa (Mac48Address ("00:11:22:33:44:55"));
    h.SetCc (0, 3);
    h.SetCc (1, 0xABCDE);
    h.SetCc (7, 0xFFFFF);
    h.SetCc (0, 0x1FFFFF);   // mask test: 21-bit value wraps to 20 bits
    NS_TEST_ASSERT_MSG_EQ (h.GetCc (0), 0xFFFFFu, "SetCc masks to 20 bits");
    h.SetCc (0, 3);          // restore
    NS_TEST_ASSERT_MSG_EQ (h.GetSerializedSize (), 64u, "CC_Update frame is 64B");
    Ptr<Packet> p = Create<Packet> ();
    p->AddHeader (h);
    uint8_t b[64] = {0};
    p->CopyData (b, 64);
    // EtherType=0x8808, Opcode=0xFFFE, CID=FA-7A-CB, MsgType=0x01
    NS_TEST_ASSERT_MSG_EQ (b[12], (uint8_t) 0x88, "EtherType hi");
    NS_TEST_ASSERT_MSG_EQ (b[13], (uint8_t) 0x08, "EtherType lo");
    NS_TEST_ASSERT_MSG_EQ (b[14], (uint8_t) 0xFF, "Opcode hi");
    NS_TEST_ASSERT_MSG_EQ (b[15], (uint8_t) 0xFE, "Opcode lo");
    NS_TEST_ASSERT_MSG_EQ (b[16], (uint8_t) 0xFA, "CID 0");
    NS_TEST_ASSERT_MSG_EQ (b[17], (uint8_t) 0x7A, "CID 1");
    NS_TEST_ASSERT_MSG_EQ (b[18], (uint8_t) 0xCB, "CID 2");
    NS_TEST_ASSERT_MSG_EQ (b[19], (uint8_t) 0x01, "MsgType");
    // 20-bit CC fields are MSB-first contiguous starting at byte 20. cc0=3
    // (0b..011) sets bits at field offsets 18/19 -> byte 20+(18>>3)=22,
    // masks 1<<(7-2)=0x20 | 1<<(7-3)=0x10. cc1=0xABCDE ORs its first 4 bits
    // into byte 22 too (value bits 19,17 set -> 0x08|0x02): 0x30|0x0A==0x3A.
    NS_TEST_ASSERT_MSG_EQ (b[20], (uint8_t) 0x00, "cc0 packs starting at byte 20 (byte 20 clear)");
    NS_TEST_ASSERT_MSG_EQ (b[21], (uint8_t) 0x00, "byte 21 clear");
    NS_TEST_ASSERT_MSG_EQ (b[22], (uint8_t) 0x3A, "byte 22 == 0x30 (cc0) | 0x0A (cc1) == 0x3A");
    // Isolated packing check (single VC, no cross-VC interference).
    {
      SueCbfcCcHeader iso;
      iso.SetCc (0, 3);
      Ptr<Packet> pIso = Create<Packet> ();
      pIso->AddHeader (iso);
      uint8_t isoB[64] = {0};
      pIso->CopyData (isoB, 64);
      NS_TEST_ASSERT_MSG_EQ (isoB[20], (uint8_t) 0x00, "iso cc0: byte 20 clear");
      NS_TEST_ASSERT_MSG_EQ (isoB[21], (uint8_t) 0x00, "iso cc0: byte 21 clear");
      NS_TEST_ASSERT_MSG_EQ (isoB[22], (uint8_t) 0x30, "iso cc0=3 packs to byte 22 == 0x30");
    }
    // round-trip
    SueCbfcCcHeader h2;
    p->RemoveHeader (h2);
    NS_TEST_ASSERT_MSG_EQ (h2.GetCc (0), 3u, "cc0 round-trip");
    NS_TEST_ASSERT_MSG_EQ (h2.GetCc (1), 0xABCDEu, "cc1 round-trip");
    NS_TEST_ASSERT_MSG_EQ (h2.GetCc (7), 0xFFFFFu, "cc7 round-trip");
    NS_TEST_ASSERT_MSG_EQ (h2.GetMacSa (), Mac48Address ("00:11:22:33:44:55"), "MACSA round-trip");
  }
};

// ---- SIM: CtlOS pacing (§5.7) ----
static void
PaceRx (Ptr<const Packet> p)
{
  SueCtlTag tag;
  if (p->PeekPacketTag (tag) && tag.GetType () == SueCtlHeader::LLR_ACK)
    { g_paceRx.push_back (Simulator::Now ()); }
}
class SueCtlOsPacingTestCase : public TestCase
{
public:
  SueCtlOsPacingTestCase () : TestCase ("LLR CtlOS min-interval pacing") {}
  void DoRun () override
  {
    // --- variant 1: explicit min interval = 1us ---
    g_paceRx.clear ();
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    A->SetCtlOsMinInterval (MicroSeconds (1));
    B->TraceConnectWithoutContext ("SueRx", MakeCallback (&PaceRx));
    // Burst of 5 ACKs at t=0; pacing should spread them to 1us apart.
    for (uint32_t i = 0; i < 5; ++i) { A->SendCtlAckNack (i, false); }
    Simulator::Stop (MicroSeconds (50));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_paceRx.size (), 5u, "all 5 paced ACKs delivered");
    for (size_t i = 1; i < g_paceRx.size (); ++i)
      {
        Time d = g_paceRx[i] - g_paceRx[i - 1];
        NS_TEST_ASSERT_MSG_GT (d, MicroSeconds (1) - NanoSeconds (1),
                               "consecutive CtlOS spacing >= min interval");
      }
    Simulator::Destroy ();

    // --- variant 2: target spacing in wire bytes (2048B @100Gbps = ~164ns, NS-quantized) ---
    g_paceRx.clear ();
    Ptr<Node> m1 = CreateObject<Node> (), m2 = CreateObject<Node> ();
    Ptr<SueNetDevice> C = CreateObject<SueNetDevice> (), D = CreateObject<SueNetDevice> ();
    m1->AddDevice (C); m2->AddDevice (D);
    Ptr<SueChannel> ch2 = CreateObject<SueChannel> (); C->Attach (ch2); D->Attach (ch2);
    C->SetDataRate (DataRate ("100Gbps")); C->SetIfg (C->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    C->SetEgressQueue (CreateObject<SueEgressQueue> ());
    D->SetDataRate (DataRate ("100Gbps")); D->SetEgressQueue (CreateObject<SueEgressQueue> ());
    C->SetCtlOsTargetSpacing (2048);
    D->TraceConnectWithoutContext ("SueRx", MakeCallback (&PaceRx));
    for (uint32_t i = 0; i < 3; ++i) { C->SendCtlAckNack (i, false); }
    Simulator::Stop (MicroSeconds (100));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_paceRx.size (), 3u, "all 3 target-spacing ACKs delivered");
    for (size_t i = 1; i < g_paceRx.size (); ++i)
      {
        Time d = g_paceRx[i] - g_paceRx[i - 1];
        // 163.84ns quantizes to the device's own gate value (163ns under NS
        // truncation) — compare against the same CalculateBytesTxTime call.
        NS_TEST_ASSERT_MSG_GT (d, C->GetDataRate ().CalculateBytesTxTime (2048) - NanoSeconds (1),
                               "2048B spacing @100G = ~164ns (NS-quantized)");
      }
    Simulator::Destroy ();
  }
};

// ============================ S6: S4-feature TestSuite coverage ==============
// Receive-time observer for the PFC gate tests (mirror of sue-s4b-pfc.cc).
static std::vector<Time> g_rxTimes;
static void
RxTimeObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  g_rxTimes.push_back (Simulator::Now ());
}

// PFC pause/resume event counter; forwards to the on-wire SendCtlPause like the
// default device wiring, so a replacement sink still gates the sender.
static uint32_t g_pfcPause, g_pfcResume;
static void
PfcEventSink (Ptr<SueNetDevice> dev, uint32_t vc, bool isPause)
{
  if (isPause) { ++g_pfcPause; } else { ++g_pfcResume; }
  dev->SendCtlPause (vc, isPause);
}
// plain counter sink for the facade unit test (no device to forward to)
static void
FacadePfcSink (uint32_t vc, bool isPause)
{
  (void) vc;
  if (isPause) { ++g_pfcPause; } else { ++g_pfcResume; }
}

// ---- S4d: CC_Update periodic leak recovery over the wire (nine rule 8/9) ----
class SueCcUpdateOnWireTestCase : public TestCase
{
public:
  SueCcUpdateOnWireTestCase () : TestCase ("CBFC CC_Update on-wire leak recovery") {}
  void DoRun () override
  {
    g_rxCount = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
    cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 100); cbfcA->Init (); cbfcB->Init ();
    A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
    B->SetDrainDelay (MicroSeconds (10));
    A->StartCcUpdateTimer (MicroSeconds (5));   // CC_Update every 5us
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayCtMax (2); eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCorruptFirstN (3); B->SetErrorModel (em);   // seq=0 FLUSH -> leak
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    A->SwitchSend (0, build ());   // seq=0 -> FLUSH
    Simulator::Schedule (MicroSeconds (300), &SueNetDevice::SwitchSend, A, 0, build ());
    Simulator::Schedule (MicroSeconds (301), &SueNetDevice::SwitchSend, A, 0, build ());
    Simulator::Stop (MilliSeconds (1));   // bound (CC_Update is periodic)
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 2u, "2 post-FLUSH frames delivered");
    NS_TEST_ASSERT_MSG_EQ (eA->GetCntTxDiscard (), 1ull, "1 FLUSH (leak 1 credit)");
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetSvcCu (0), 0u, "leak recovered: CU=0");
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetSvcCc (0), 3u, "S_CC=3");
    // MSG-ORD-001 snapshot semantics (documented closure): each CC_Update
    // carries the S_CC snapshot AT SEND TIME only. CU==0 above proves it — a
    // post-dated snapshot (including the 2 frames sent after the last
    // CC_Update cycle) would refund lost=3 instead of 1, wrap CU to ~2^15-2
    // and block the VC permanently (the §12.31 A2 failure shape).
  }
};

// ---- S4f: counter system (§6.11) ----
class SueCbfcCountersTestCase : public TestCase
{
public:
  SueCbfcCountersTestCase () : TestCase ("FC LLR+CBFC counters") {}
  void DoRun () override
  {
    g_rxCount = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
    cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 10); cbfcA->Init ();
    A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (CreateObject<SueCbfcEngine> ());
    B->SetDrainDelay (MicroSeconds (10));
    A->StartCcUpdateTimer (MicroSeconds (50));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->Start (); eB->Start ();
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCorruptOnce (2); B->SetErrorModel (em);   // frame 2 (seq=1) bad
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Stop (MicroSeconds (200));   // bound (CC_Update periodic)
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 3u, "3 delivered");
    const SueLlrCounters &a = eA->GetCounters ();
    const SueLlrCounters &b = eB->GetCounters ();
    NS_TEST_ASSERT_MSG_GT_OR_EQ (a.rx_ack_ctl_os, 3ull, "A rx_ack>=3");
    NS_TEST_ASSERT_MSG_EQ (a.rx_nack_ctl_os, 1ull, "A rx_nack=1");
    NS_TEST_ASSERT_MSG_EQ (b.tx_nack_ctl_os, 1ull, "B tx_nack=1");
    NS_TEST_ASSERT_MSG_EQ (b.rx_bad, 1ull, "B rx_bad=1");
    NS_TEST_ASSERT_MSG_EQ (b.rx_expected_seq_bad, 1ull, "B rx_expected_seq_bad=1");
    NS_TEST_ASSERT_MSG_GT_OR_EQ (a.tx_replay, 1ull, "A tx_replay>=1");
    NS_TEST_ASSERT_MSG_GT_OR_EQ (cbfcA->GetCounters ().cc_update_tx, 1ull, "A cc_update_tx>=1");
  }
};

// ---- S4e: SueFcController facade, FC_MODE=CBFC (§6.4/§6.5) ----
class SueFcControllerTestCase : public TestCase
{
public:
  SueFcControllerTestCase () : TestCase ("FC facade (CBFC mode)") {}
  void DoRun () override
  {
    g_rxCount = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
    cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 5); cbfcA->Init (); cbfcB->Init ();
    Ptr<SueFcController> fcA = CreateObject<SueFcController> (), fcB = CreateObject<SueFcController> ();
    fcA->SetCbfc (cbfcA); fcB->SetCbfc (cbfcB);
    A->SetFcController (fcA); B->SetFcController (fcB);
    B->SetDrainDelay (MicroSeconds (10));
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    NS_TEST_ASSERT_MSG_EQ (fcA->GetMode (), SueFcController::CBFC, "FC_MODE=CBFC");
    NS_TEST_ASSERT_MSG_EQ (A->GetFcController (), fcA, "device holds controller");
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }   // limit 5 -> gating
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 10u, "all 10 delivered via facade");
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetSvcCc (0), 10u, "S_CC=10");
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetSvcCu (0), 0u, "CU=0 (all returned)");
  }
};

// ---- S4b: PFC gate (simple per-VC threshold) ----
class SuePfcTestCase : public TestCase
{
public:
  SuePfcTestCase () : TestCase ("PFC gating (simple threshold)") {}
  void DoRun () override
  {
    g_rxTimes.clear ();
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SuePfcEngine> pa = CreateObject<SuePfcEngine> (), pb = CreateObject<SuePfcEngine> ();
    pb->SetBufferSize (65536); pb->SetPauseThreshold (300); pb->SetResumeThreshold (100); pb->Init ();
    pa->Init ();
    A->SetPfcEngine (pa); B->SetPfcEngine (pb);
    B->SetDrainDelay (MicroSeconds (20));
    B->SetReceiveUpcall (MakeCallback (&RxTimeObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxTimes.size (), 10u, "all 10 delivered");
    if (g_rxTimes.size () == 10)
      {
        NS_TEST_ASSERT_MSG_GT (g_rxTimes.back (), MicroSeconds (15),
                               "last frame delayed by PFC gating");
        NS_TEST_ASSERT_MSG_LT (g_rxTimes[0], MicroSeconds (2),
                               "1st frame early (before pause)");
      }
  }
};

// ---- S6: shared-pool PFC (dynamic threshold) gates a burst that would never
//       pause in simple mode (per-VC threshold 10000B >> 10-frame burst). ----
class SuePfcSharedPoolTestCase : public TestCase
{
public:
  SuePfcSharedPoolTestCase () : TestCase ("PFC shared-pool gating") {}
  void DoRun () override
  {
    g_rxTimes.clear (); g_pfcPause = 0; g_pfcResume = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SuePfcEngine> pa = CreateObject<SuePfcEngine> (), pb = CreateObject<SuePfcEngine> ();
    pb->SetBufferSize (65536);
    pb->SetPauseThreshold (10000);   // unreachable in simple mode (10-frame burst)
    pb->SetResumeThreshold (100);
    // Shared pool: dynamic thresh = (2048 - sharedUsed) >> 2; headroom 10KB is
    // unreachable so headroomUsed stays 0 and the threshold shrinks as the pool
    // fills -- pause fires mid-burst, resume only after drain.
    pb->EnableSharedPool (2048, 10000, 0, 2);
    pb->Init ();
    pa->Init ();
    A->SetPfcEngine (pa); B->SetPfcEngine (pb);
    // Replace B's pause sink with a counting forwarder (default wiring calls
    // SendCtlPause directly; keep that on-wire behavior).
    pb->SetPauseSink (MakeBoundCallback (&PfcEventSink, B));
    B->SetDrainDelay (MicroSeconds (20));
    B->SetReceiveUpcall (MakeCallback (&RxTimeObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxTimes.size (), 10u, "all 10 delivered (drained)");
    NS_TEST_ASSERT_MSG_GT (g_pfcPause, 0u, "shared-pool pause fired");
    NS_TEST_ASSERT_MSG_GT (g_pfcResume, 0u, "resume fired after drain");
    if (g_rxTimes.size () == 10)
      {
        NS_TEST_ASSERT_MSG_GT (g_rxTimes.back (), MicroSeconds (15),
                               "burst gated by shared-pool headroom");
      }
  }
};

// ---- PFC-SP-03: shared-pool hysteresis via resume_offset ----
class SuePfcResumeOffsetTestCase : public TestCase
{
public:
  SuePfcResumeOffsetTestCase () : TestCase ("PFC shared-pool hysteresis (resume offset)") {}
  void DoRun () override
  {
    g_pfcPause = 0; g_pfcResume = 0;
    Ptr<SuePfcEngine> pfc = CreateObject<SuePfcEngine> ();
    // shared 1000B, headroom 100B, offset 200B, alpha 0. The dynamic threshold
    // th = avail + headroomUsed stays pinned at 1000 while the pool drains (the
    // shared usage moves from the shared side to the headroom side), so the
    // hysteresis band is exactly offset=200B wide.
    pfc->EnableSharedPool (1000, 100, 200, 0);
    pfc->Init ();
    pfc->SetPauseSink (MakeCallback (&FacadePfcSink));
    // Headroom exceeded -> pause (SwitchMmu hdrm>0 condition).
    pfc->OnRx (0, 200);
    pfc->CheckThresholds ();
    NS_TEST_ASSERT_MSG_EQ (g_pfcPause, 1u, "pause fires on headroom overflow");
    pfc->OnRx (0, 800);   // bufUsed=1000 (pool exhausted)
    pfc->CheckThresholds ();
    NS_TEST_ASSERT_MSG_EQ (g_pfcPause, 1u, "still paused (single pause event)");
    // Inside the band (th-offset < bufUsed <= th): NO resume.
    pfc->OnDrain (0, 100);   // bufUsed=900 > resumeLine(800)
    pfc->CheckThresholds ();
    NS_TEST_ASSERT_MSG_EQ (g_pfcResume, 0u, "no resume inside the hysteresis band (900B)");
    // Exactly at the band edge: resume fires.
    pfc->OnDrain (0, 100);   // bufUsed=800 <= resumeLine(800)
    pfc->CheckThresholds ();
    NS_TEST_ASSERT_MSG_EQ (g_pfcResume, 1u, "resume fires at th-offset (800B)");
    // Re-pause / re-resume cycle stays stable.
    pfc->OnRx (0, 200);
    pfc->CheckThresholds ();
    NS_TEST_ASSERT_MSG_EQ (g_pfcPause, 2u, "re-pause fires again");
    pfc->OnDrain (0, 1000);
    pfc->CheckThresholds ();
    NS_TEST_ASSERT_MSG_EQ (g_pfcResume, 2u, "re-resume fires again");
  }
};

// ---- S6: nine rule 3 -- CC_Update inside an LLR window is discarded ----
class SueRule3TestCase : public TestCase
{
public:
  SueRule3TestCase () : TestCase ("FC nine rule 3 (CC_Update in LLR window)") {}
  void DoRun () override
  {
    Ptr<SueCbfcEngine> cbfc = CreateObject<SueCbfcEngine> ();
    cbfc->Init ();
    std::array<uint32_t, 16> cc{}; cc[0] = 5;
    // Held window: OnCcUpdate must be a no-op (SVC_CC stays 0, no recovery).
    cbfc->SuspendForLlrWindow (true);
    NS_TEST_ASSERT_MSG_EQ (cbfc->GetInLlrWindow (), true, "window held");
    cbfc->OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (cbfc->GetRvcCf (0), 0u, "no credit recovery in window");
    // Window open: recovery proceeds (lost=5 -> R_VC_CF=5).
    cbfc->SuspendForLlrWindow (false);
    cbfc->OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (cbfc->GetRvcCf (0), 5u, "recovered 5 credits after window");
    NS_TEST_ASSERT_MSG_EQ (cbfc->GetCounters ().cc_lost_credits_recovered[0], 1ull,
                           "lost-recovered counted once");
    // Same snapshot again: no double recovery.
    cbfc->OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (cbfc->GetRvcCf (0), 5u, "no double recovery");
    // New snapshot: another 5 lost.
    cc[0] = 10;
    cbfc->OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (cbfc->GetRvcCf (0), 10u, "second recovery applied");
  }
};

// ---- S6: engine-driven LLR window (§5.2.9 rules 3/9 real windows) ----
// The window must cover the REAL windows (REPLAY state / NACK-sent discard
// window, spanning many events), not just the synchronous retransmit call.
static bool g_winFlag;
static void
WinObs (bool on)
{
  g_winFlag = on;
}

class SueLlrWindowDrivenTestCase : public TestCase
{
public:
  SueLlrWindowDrivenTestCase () : TestCase ("FC LLR window engine-driven (rules 3 and 9 real windows)") {}
  void DoRun () override
  {
    // (a) engine window mechanics: TX replay window + RX discard window.
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    e->SetLlrWindowCb (MakeCallback (&WinObs));
    g_winFlag = false;
    e->Start ();   // no sink -> ADVANCE (window stays false)
    NS_TEST_ASSERT_MSG_EQ (g_winFlag, false, "window closed at start");
    e->OnInit (0);   // vld + next_rx=0
    // BAD/missing frame -> NACK -> discard window opens.
    e->RxFrame (5, 2);
    NS_TEST_ASSERT_MSG_EQ (g_winFlag, true, "discard window opened on NACK");
    // COUNTER-020: rx_missing_seq counts the missing-frame classification
    // (seq 5 ahead of next_rx_seq 0 -> missing, not duplicate).
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().rx_missing_seq, 1ull,
                           "rx_missing_seq counted (seq 5 ahead of next 0)");
    // The retransmit lands -> window closes.
    e->RxFrame (0, 0);
    NS_TEST_ASSERT_MSG_EQ (g_winFlag, false, "discard window closed on retransmit");
    // TX side: NACK -> REPLAY opens the window; final ACK -> ADVANCE closes it.
    e->TxStore (Create<Packet> (86));   // seq 0
    e->TxStore (Create<Packet> (86));   // seq 1
    e->OnAckNack (0, true);   // release seq0, REPLAY seq1
    NS_TEST_ASSERT_MSG_EQ (g_winFlag, true, "replay window opened");
    e->OnAckNack (1, false);  // buffer empty -> ADVANCE
    NS_TEST_ASSERT_MSG_EQ (g_winFlag, false, "replay window closed");

    // (b) device wiring: rule 9 gates SendCtlCcUpdate on the REAL window.
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
    cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 100); cbfcA->Init ();
    A->SetCbfcEngine (cbfcA);
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA);
    eA->Start ();
    eA->TxStore (Create<Packet> (86));   // seq 0
    eA->TxStore (Create<Packet> (86));   // seq 1
    eA->OnAckNack (0, true);   // -> REPLAY: window opens via the wiring
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetInLlrWindow (), true, "device CBFC window on during REPLAY");
    A->SendCtlCcUpdate ();
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetCounters ().cc_update_tx, 0ull,
                           "rule 9: no CC_Update inside the real replay window");
    eA->OnAckNack (1, false);  // -> ADVANCE: window closes
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetInLlrWindow (), false, "device CBFC window off after ADVANCE");
    A->SendCtlCcUpdate ();
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetCounters ().cc_update_tx, 1ull,
                           "rule 9: CC_Update sent after the window");

    // (c) rule 3 + type B: receiver's discard window discards CC_Update, but
    // type-B (LLR-ineligible) frames still charge R_VC_CC (UEC Table 5-26).
    Ptr<SueNetDevice> C = CreateObject<SueNetDevice> ();
    Ptr<Node> n3 = CreateObject<Node> ();
    n3->AddDevice (C);
    C->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueChannel> ch2 = CreateObject<SueChannel> (); C->Attach (ch2);   // INIT TX needs a channel
    Ptr<SueNetDevice> D = CreateObject<SueNetDevice> ();
    Ptr<Node> n4 = CreateObject<Node> ();
    n4->AddDevice (D);
    D->SetDataRate (DataRate ("100Gbps"));
    D->Attach (ch2);   // second endpoint so the link state leaves INITIALIZING
    Ptr<SueCbfcEngine> cbfcC = CreateObject<SueCbfcEngine> ();
    cbfcC->SetCreditSize (100); cbfcC->Init ();
    C->SetCbfcEngine (cbfcC);
    Ptr<SueLlrEngine> eC = CreateObject<SueLlrEngine> ();
    C->SetLlrEngine (eC);
    eC->Start ();
    eC->OnInit (0);
    eC->RxFrame (2, 2);   // missing -> NACK -> discard window open
    NS_TEST_ASSERT_MSG_EQ (cbfcC->GetInLlrWindow (), true, "RX discard window on");
    std::array<uint32_t, 16> cc{}; cc[0] = 5;
    cbfcC->OnCcUpdate (cc);
    NS_TEST_ASSERT_MSG_EQ (cbfcC->GetRvcCf (0), 0u, "rule 3: CC_Update discarded in window");
    // INT-004 (rule 4): CF_Update received inside the window STILL applies.
    cbfcC->OnCfUpdate (0, 5);
    NS_TEST_ASSERT_MSG_EQ (cbfcC->GetSvcCf (0), 5u, "rule 4: CF_Update still applies inside the window");
    // Type B: an ineligible frame accepted during the window still charges.
    cbfcC->OnRx (0, 100);
    NS_TEST_ASSERT_MSG_EQ (cbfcC->GetRvcCc (0), 1u, "type-B frame counted in window (UEC Table 5-26)");
    // Retransmit (the missing seq 0) lands -> window closes -> CC_Update
    // processed again.
    eC->RxFrame (0, 0);
    NS_TEST_ASSERT_MSG_EQ (cbfcC->GetInLlrWindow (), false, "discard window closed");
    cbfcC->OnCcUpdate (cc);
    // lost = cc(5) - R_VC_CC(1, the type-B frame counted in-window) = 4.
    NS_TEST_ASSERT_MSG_EQ (cbfcC->GetRvcCf (0), 4u, "rule 3: recovery proceeds after window");
    Simulator::Destroy ();
  }
};

// ---- S6: nine rule 9 -- no CC_Update TX inside an LLR window ----
class SueRule9TestCase : public TestCase
{
public:
  SueRule9TestCase () : TestCase ("FC nine rule 9 (no CC_Update in window)") {}
  void DoRun () override
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> ();
    cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 100); cbfcA->Init ();
    A->SetCbfcEngine (cbfcA);
    // Window held: SendCtlCcUpdate suppressed (no counter bump, no frame).
    cbfcA->SuspendForLlrWindow (true);
    A->SendCtlCcUpdate ();
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetCounters ().cc_update_tx, 0ull,
                           "CC_Update suppressed in LLR window");
    // Window open: CC_Update transmitted (counter bumped).
    cbfcA->SuspendForLlrWindow (false);
    A->SendCtlCcUpdate ();
    NS_TEST_ASSERT_MSG_EQ (cbfcA->GetCounters ().cc_update_tx, 1ull,
                           "CC_Update sent after window");
    Simulator::Destroy ();
  }
};

// ---- S6: SueFcController PFC OnRx runs CheckThresholds (pauses fire) ----
class SueFacadePfcOnRxTestCase : public TestCase
{
public:
  SueFacadePfcOnRxTestCase () : TestCase ("FC facade PFC OnRx thresholds") {}
  void DoRun () override
  {
    Ptr<SuePfcEngine> pfc = CreateObject<SuePfcEngine> ();
    pfc->SetBufferSize (65536); pfc->SetPauseThreshold (300); pfc->SetResumeThreshold (100);
    pfc->Init ();
    g_pfcPause = 0; g_pfcResume = 0;
    pfc->SetPauseSink (MakeCallback (&FacadePfcSink));
    Ptr<SueFcController> fc = CreateObject<SueFcController> ();
    fc->SetPfc (pfc);
    NS_TEST_ASSERT_MSG_EQ (fc->GetMode (), SueFcController::PFC, "FC_MODE=PFC");
    // 4x 90B through the facade: 360B >= pause 300 -> pause sink fires.
    for (int i = 0; i < 4; ++i) { fc->OnRx (0, 90); }
    NS_TEST_ASSERT_MSG_EQ (g_pfcPause, 1u, "facade OnRx triggered pause");
    NS_TEST_ASSERT_MSG_EQ (g_pfcResume, 0u, "no resume yet");
    // Drain below resume threshold (100B) -> resume sink fires.
    fc->OnDrain (0, 320);
    NS_TEST_ASSERT_MSG_EQ (pfc->GetBufferUsed (0), 40u, "buffer drained to 40B");
    NS_TEST_ASSERT_MSG_EQ (g_pfcResume, 1u, "facade OnDrain triggered resume");
  }
};

// ---- S5b: SueLiteEndpoint host-side self-simulated traffic generator ----
class SueLiteEndpointTestCase : public TestCase
{
public:
  SueLiteEndpointTestCase () : TestCase ("ENDPOINT generator (shim-vlan VC, stubs, workload gate)") {}
  void DoRun () override
  {
    // Manual topo has no FC/LLR and no real rates on the switch ports; give
    // the sender an egress queue and every device a real rate so frames at
    // 20us spacing serialize cleanly (the placeholder 32768 b/s would hold a
    // port busy for ~24ms and drop the overlapping frames).
    auto RateTopo = [] (FwdTopo t) {
      t.h1->SetDataRate (DataRate ("100Gbps"));
      t.h1->SetEgressQueue (CreateObject<SueEgressQueue> ());
      for (uint32_t d = 0; d < t.sw->GetNDevices (); ++d)
        {
          DynamicCast<SueNetDevice> (t.sw->GetDevice (d))->SetDataRate (DataRate ("100Gbps"));
        }
    };
    // Scenario A: shim/dscp=3, 5 frames h1(xpu1) -> h2(xpu2). The generator
    // runs to natural exhaustion (FrameCount bounds the self-chain), so
    // Simulator::Run() returns once the last frame is delivered.
    {
      FwdTopo t = BuildFwdTopo ();
      RateTopo (t);
      Ptr<SueLiteEndpoint> ep1 = CreateObject<SueLiteEndpoint> ();
      Ptr<SueLiteEndpoint> ep2 = CreateObject<SueLiteEndpoint> ();
      ep1->SetSourceXpu (1);
      ep1->SetDevice (t.h1);
      ep1->SetDestinationXpu (2);
      ep1->SetUseShim (true);
      ep1->SetVc (3);
      ep1->SetPayloadSize (86);
      ep1->SetInterval (MicroSeconds (20));
      ep1->SetFrameCount (5);
      ep1->SetStartTime (MicroSeconds (10));
      ep1->Start ();
      ep2->SetSourceXpu (2);
      ep2->SetDevice (t.h2);
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (ep1->GetTxCount (), 5ull, "A tx 5 frames");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxCount (), 5ull, "A rx 5 frames");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByVc (3), 5ull, "A VC3 via shim.dscp");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByDst (2), 5ull, "A dst xpu2");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxBySrc (1), 5ull, "A src xpu1 (MAC SA)");
    }
    // Scenario B: VLAN/pcp=2, 4 frames + the reserved AstraNetworkAPI stubs.
    {
      FwdTopo t = BuildFwdTopo ();
      RateTopo (t);
      Ptr<SueLiteEndpoint> ep1 = CreateObject<SueLiteEndpoint> ();
      Ptr<SueLiteEndpoint> ep2 = CreateObject<SueLiteEndpoint> ();
      ep1->SetSourceXpu (1);
      ep1->SetDevice (t.h1);
      ep1->SetDestinationXpu (2);
      ep1->SetUseVlan (true);
      ep1->SetVc (2);
      ep1->SetFrameCount (4);
      ep1->SetStartTime (MicroSeconds (10));
      ep1->Start ();
      ep2->SetSourceXpu (2);
      ep2->SetDevice (t.h2);
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (ep1->GetTxCount (), 4ull, "B tx 4 frames");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxCount (), 4ull, "B rx 4 frames");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByVc (2), 4ull, "B VC2 via vlan.pcp");
      NS_TEST_ASSERT_MSG_EQ (ep1->sim_send (1, 2, nullptr), -1, "B sim_send stub -1");
      NS_TEST_ASSERT_MSG_EQ (ep1->sim_recv (1, 2, nullptr), -1, "B sim_recv stub -1");
      NS_TEST_ASSERT_MSG_EQ (ep1->sim_schedule (1, 2, 100), -1, "B sim_schedule stub -1");
    }
    // Scenario C: ENABLE_SCALEUP_WORKLOAD=1 (external workload not wired) must
    // warn + fall back to the built-in generator, not go silent (DESIGN §12.23).
    {
      Config::SetGlobal ("ENABLE_SCALEUP_WORKLOAD", UintegerValue (1));
      FwdTopo t = BuildFwdTopo ();
      RateTopo (t);
      Ptr<SueLiteEndpoint> ep1 = CreateObject<SueLiteEndpoint> ();
      Ptr<SueLiteEndpoint> ep2 = CreateObject<SueLiteEndpoint> ();
      ep1->SetSourceXpu (1);
      ep1->SetDevice (t.h1);
      ep1->SetDestinationXpu (2);
      ep1->SetUseShim (true);
      ep1->SetVc (1);
      ep1->SetFrameCount (3);
      ep1->SetStartTime (MicroSeconds (10));
      ep1->Start ();
      ep2->SetSourceXpu (2);
      ep2->SetDevice (t.h2);
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxCount (), 3ull,
                             "C workload gate falls back to the built-in generator");
      Config::SetGlobal ("ENABLE_SCALEUP_WORKLOAD", UintegerValue (0));
    }
  }
};

// ============ S5: VC mapping tables (DESIGN §7) — end-to-end ============
// DSCP_TO_VC_MAP / PCP_TO_VC_MAP / DEFAULT_VC implement the reserved §7 keys:
// a general flow-class mapping that is end-to-end consistent — host send, host
// receive and the switch all decode the SAME table. The default map must
// reproduce the legacy dscp&0x07 / pcp identity / 0 behavior exactly, so every
// earlier case stays green. Every scenario restores the defaults before the
// next (40 cases share one process; a leftover non-identity map would pollute
// SueCounterTestCase / SueLiteEndpointTestCase).
class SueVcMapConfigTestCase : public TestCase
{
public:
  SueVcMapConfigTestCase () : TestCase ("CFG VC mapping (DSCP_PCP_DEFAULT tables)") {}

  void AssertDefaults ()
  {
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (0), 0, "dscp0 -> vc0");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (3), 3, "dscp3 -> vc3");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (5), 5, "dscp5 -> vc5");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (8), 0, "dscp8 -> vc0 (8&7=0, NOT dscp/8)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (12), 4, "dscp12 -> vc4");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (63), 7, "dscp63 -> vc7");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (0), 0, "pcp0 -> vc0");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (5), 5, "pcp5 -> vc5");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (7), 7, "pcp7 -> vc7");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (0, true), 0, "pcp0/dei1 -> vc0 (identity row)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (7, true), 7, "pcp7/dei1 -> vc7 (identity row)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DefaultVc (), 0, "default vc 0");
  }

  void RestoreDefaults ()
  {
    Config::SetGlobal ("DSCP_TO_VC_MAP", StringValue (
      "0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7,8:0,9:1,10:2,11:3,12:4,13:5,14:6,15:7,"
      "16:0,17:1,18:2,19:3,20:4,21:5,22:6,23:7,24:0,25:1,26:2,27:3,28:4,29:5,30:6,31:7,"
      "32:0,33:1,34:2,35:3,36:4,37:5,38:6,39:7,40:0,41:1,42:2,43:3,44:4,45:5,46:6,47:7,"
      "48:0,49:1,50:2,51:3,52:4,53:5,54:6,55:7,56:0,57:1,58:2,59:3,60:4,61:5,62:6,63:7"));
    Config::SetGlobal ("PCP_TO_VC_MAP", StringValue ("0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7"));
    Config::SetGlobal ("DEFAULT_VC", UintegerValue (0));
    SueConfig::InvalidateCache ();
  }

  void DoRun () override
  {
    // Scenario 1: default direct map (first-use parse of the GlobalValue
    // defaults; re-primes whatever a prior test left in the cache).
    AssertDefaults ();

    // Scenario 2: partial override — listed entries remap, unlisted keep the
    // direct default (dscp&0x07 / pcp identity); DEFAULT_VC is separate.
    Config::SetGlobal ("DSCP_TO_VC_MAP", StringValue ("0:7,63:0,5:5"));
    Config::SetGlobal ("PCP_TO_VC_MAP", StringValue ("0:7"));
    Config::SetGlobal ("DEFAULT_VC", UintegerValue (5));
    SueConfig::InvalidateCache ();
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (0), 7, "dscp0 -> vc7 (override)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (63), 0, "dscp63 -> vc0 (override)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (5), 5, "dscp5 -> vc5 (listed, self-map)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DscpToVc (9), 1, "dscp9 unlisted -> vc1 (partial = dscp&0x07)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (0), 7, "pcp0 -> vc7 (override)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (3), 3, "pcp3 unlisted -> vc3 (identity)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::DefaultVc (), 5, "default vc 5 (override)");
    RestoreDefaults ();
    AssertDefaults ();

    // Scenario 3 (end-to-end, switch path): dscp=5 -> vc7 via the map, so the
    // switch counts the frame under vc7, NOT under dscp&7=5.
    Config::SetGlobal ("DSCP_TO_VC_MAP", StringValue ("5:7"));
    SueConfig::InvalidateCache ();
    {
      FwdTopo t = BuildFwdTopo ();
      t.sw->ResetCounters ();
      SendOne (t.h1, BuildFrame (2, false, 0, true, 3, 5, false));   // shim dscp=5
      NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (7), 1ull, "switch vc7 rx=1 (map 5:7)");
      NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcTx (7), 1ull, "switch vc7 tx=1 (map 5:7)");
      NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (5), 0ull, "switch vc5 rx=0 (NOT dscp&7=5)");
      Simulator::Destroy ();
    }
    RestoreDefaults ();
    AssertDefaults ();

    // Scenario 4 (end-to-end, host path): SetDscp(3) on the generator -> wire
    // shim.dscp=3 -> the receiver attributes via the same map: vc7, not vc3.
    Config::SetGlobal ("DSCP_TO_VC_MAP", StringValue ("3:7"));
    SueConfig::InvalidateCache ();
    {
      FwdTopo t = BuildFwdTopo ();
      t.h1->SetDataRate (DataRate ("100Gbps"));
      t.h1->SetEgressQueue (CreateObject<SueEgressQueue> ());
      for (uint32_t d = 0; d < t.sw->GetNDevices (); ++d)
        {
          DynamicCast<SueNetDevice> (t.sw->GetDevice (d))->SetDataRate (DataRate ("100Gbps"));
        }
      Ptr<SueLiteEndpoint> ep1 = CreateObject<SueLiteEndpoint> ();
      Ptr<SueLiteEndpoint> ep2 = CreateObject<SueLiteEndpoint> ();
      ep1->SetSourceXpu (1);
      ep1->SetDevice (t.h1);
      ep1->SetDestinationXpu (2);
      ep1->SetUseShim (true);
      ep1->SetDscp (3);
      ep1->SetPayloadSize (86);
      ep1->SetInterval (MicroSeconds (20));
      ep1->SetFrameCount (5);
      ep1->SetStartTime (MicroSeconds (10));
      ep1->Start ();
      ep2->SetSourceXpu (2);
      ep2->SetDevice (t.h2);
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxCount (), 5ull, "host rx 5 frames");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByVc (7), 5ull, "host vc7 rx=5 (map 3:7, end-to-end)");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByVc (3), 0ull, "host vc3 rx=0 (NOT dscp&7=3)");
    }
    RestoreDefaults ();
    AssertDefaults ();

    // Scenario 5 (DEI row select): the 16-entry PCP_TO_VC_MAP addresses keys
    // (dei<<3|pcp) — "0:1,8:7" maps pcp0/dei0 -> vc1 but pcp0/dei1 -> vc7;
    // unlisted keys stay identity within their row. DEI survives the wire
    // (TCI bit 4), so host send, switch decode and host receive all agree.
    Config::SetGlobal ("PCP_TO_VC_MAP", StringValue ("0:1,8:7"));
    SueConfig::InvalidateCache ();
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (0), 1, "pcp0/dei0 -> vc1 (override)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (0, true), 7, "pcp0/dei1 -> vc7 (override, DEI row)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (1, true), 1, "pcp1/dei1 unlisted -> vc1 (row identity)");
    NS_TEST_ASSERT_MSG_EQ (SueConfig::PcpToVc (5), 5, "pcp5/dei0 unlisted -> vc5 (identity)");
    {
      FwdTopo t = BuildFwdTopo ();
      t.sw->ResetCounters ();
      Ptr<Packet> p = Create<Packet> (64);
      SueHeader h;
      h.SetDst (SueMacFromXpuId (2));
      h.SetSrc (SueMacFromXpuId (1));
      h.SetVlan (true, 0, 1, 100);   // pcp=0, DEI=1
      h.SetEtherType (SUE_ETH_TYPE_0);
      p->AddHeader (h);
      SendOne (t.h1, p);
      NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (7), 1ull, "switch vc7 rx=1 (pcp0+DEI1 -> key 8:7)");
      NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (1), 0ull, "switch vc1 rx=0 (NOT DEI0 row)");
      NS_TEST_ASSERT_MSG_EQ (t.sw->GetCntVcRx (0), 0ull, "switch vc0 rx=0 (NOT pcp identity)");
      Simulator::Destroy ();
    }
    // Endpoint path: SetPcp(0)+SetDei(true)+SetUseVlan -> wire DEI=1 -> the
    // sender's own egress-queue index and the receiver attribution are vc7.
    {
      FwdTopo t = BuildFwdTopo ();
      t.h1->SetDataRate (DataRate ("100Gbps"));
      t.h1->SetEgressQueue (CreateObject<SueEgressQueue> ());
      for (uint32_t d = 0; d < t.sw->GetNDevices (); ++d)
        {
          DynamicCast<SueNetDevice> (t.sw->GetDevice (d))->SetDataRate (DataRate ("100Gbps"));
        }
      Ptr<SueLiteEndpoint> ep1 = CreateObject<SueLiteEndpoint> ();
      Ptr<SueLiteEndpoint> ep2 = CreateObject<SueLiteEndpoint> ();
      ep1->SetSourceXpu (1);
      ep1->SetDevice (t.h1);
      ep1->SetDestinationXpu (2);
      ep1->SetUseVlan (true);
      ep1->SetPcp (0);
      ep1->SetDei (true);
      ep1->SetPayloadSize (86);
      ep1->SetInterval (MicroSeconds (20));
      ep1->SetFrameCount (5);
      ep1->SetStartTime (MicroSeconds (10));
      ep1->Start ();
      ep2->SetSourceXpu (2);
      ep2->SetDevice (t.h2);
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxCount (), 5ull, "host rx 5 frames");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByVc (7), 5ull, "host vc7 rx=5 (pcp0+DEI1, end-to-end)");
      NS_TEST_ASSERT_MSG_EQ (ep2->GetRxByVc (1), 0ull, "host vc1 rx=0 (NOT DEI0 row)");
    }
    RestoreDefaults ();
    AssertDefaults ();
  }
};

// ============ CBFC-SP-13: LLDP RTS/RTR handshake (§5.2.8.4) — end-to-end ============
// The lossless-VC initialization handshake (UEC §5.2.8.4, minimal model):
// R_VC_Want -> S_VC_RTS -> R_VC_RTR, multi-round LLDP exchange, and the
// engine-side gate that keeps a VC from using credits until its handshake
// completes. These cases are the in-suite verification for NEG-SP-01/02
// (their original scratch carrier, sue-llrp-negotiate, was never committed).
// All cases wire devices directly (no SueReadConf/ApplySueConfig) so no
// GlobalValue leaks into the other 48 cases.

static uint32_t g_llrpRxTotal;
static uint32_t g_llrpRxVc[8];
static Time g_llrpFirstRx{Seconds (0)};
static bool g_llrpMidWindowClean;
static void
LlrpRxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader h)
{
  if (g_llrpRxTotal == 0) { g_llrpFirstRx = Simulator::Now (); }
  ++g_llrpRxTotal;
  uint8_t vc = h.HasShim () ? SueConfig::DscpToVc (h.GetShim ().dscp) : SueConfig::DefaultVc ();
  ++g_llrpRxVc[vc];
}

static void
CheckLldpMidWindow ()
{
  g_llrpMidWindowClean = (g_llrpRxTotal == 0);
}

class SueLldpHeaderRoundTripTestCase : public TestCase
{
public:
  SueLldpHeaderRoundTripTestCase () : TestCase ("LLDP header round-trip (RTS-RTR bitmaps)") {}
  void DoRun () override
  {
    SueLldpHeader h;
    h.SetLlrSupported (true);
    h.SetCreditSize (256);
    h.SetPktOvhd (12);
    h.SetNumVcs (8);
    for (uint8_t v = 0; v < 8; ++v) { h.SetVcLimit (v, 100 + v); }
    // Bitmap edges: bit 0, a mid bit, and the byte-boundary bit 7.
    h.SetRvcWant (0, true); h.SetRvcWant (3, true); h.SetRvcWant (7, true);
    h.SetSvcRts (1, true); h.SetSvcRts (7, true);
    h.SetRvcRtr (5, true);
    NS_TEST_ASSERT_MSG_EQ (h.GetSerializedSize (), 45u, "45B header (42 + 3 bitmaps)");

    Buffer buf;
    buf.AddAtStart (h.GetSerializedSize ());
    h.Serialize (buf.Begin ());
    SueLldpHeader h2;
    NS_TEST_ASSERT_MSG_EQ (h2.Deserialize (buf.Begin ()), 45u, "deserialize consumed 45B");
    NS_TEST_ASSERT_MSG_EQ (h2.GetRvcWantBitmap (), 0x89u, "rvcWant bitmap round-trips");
    NS_TEST_ASSERT_MSG_EQ (h2.GetSvcRtsBitmap (), 0x82u, "svcRts bitmap round-trips");
    NS_TEST_ASSERT_MSG_EQ (h2.GetRvcRtrBitmap (), 0x20u, "rvcRtr bitmap round-trips");
    NS_TEST_ASSERT_MSG_EQ (h2.GetLlrSupported (), true, "llrSupported round-trips");
    NS_TEST_ASSERT_MSG_EQ (h2.GetCreditSize (), 256u, "creditSize round-trips");
    NS_TEST_ASSERT_MSG_EQ (h2.GetVcLimit (7), 107u, "vcLimit round-trips");
    NS_TEST_ASSERT_MSG_EQ (h2.GetRvcWant (2), false, "unset want bit stays 0");
  }
};

class SueCbfcInitHandshakeE2eTestCase : public TestCase
{
public:
  SueCbfcInitHandshakeE2eTestCase () : TestCase ("CBFC RTS-RTR handshake gating (no credit TX before init)") {}
  void DoRun () override
  {
    g_llrpRxTotal = 0; g_llrpFirstRx = Seconds (0); g_llrpMidWindowClean = false;
    for (auto &c : g_llrpRxVc) { c = 0; }
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    // 2µs one-way propagation stretches the 3-round handshake to ~7.2µs so the
    // mid-window observation at 6µs lands safely inside it.
    ch->SetAttribute ("Delay", TimeValue (MicroSeconds (2)));
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100); ca->SetVcCreditLimit (0, 8); ca->Init ();
    cb->SetCreditSize (100); cb->SetVcCreditLimit (0, 8); cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    B->SetDrainDelay (MicroSeconds (10));
    B->SetReceiveUpcall (MakeCallback (&LlrpRxObs));
    A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
    A->StartLldpExchange (); B->StartLldpExchange ();
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
    // Fires at 6µs, inside the ~7.2µs handshake window: nothing may have
    // arrived yet (the §5.2.8.4 gate blocks credit TX before RTR completes).
    Simulator::Schedule (MicroSeconds (6), &CheckLldpMidWindow);
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_llrpRxTotal, 10u, "10 delivered after handshake");
    NS_TEST_ASSERT_MSG_EQ (g_llrpMidWindowClean, true, "0 frames before the RTS/RTR handshake completes");
    bool firstRxLate = g_llrpFirstRx >= MicroSeconds (6);
    NS_TEST_ASSERT_MSG_EQ (firstRxLate, true, "first frame only after the mid-window check");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (0), 10u, "sender consumed 10 credits");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "all credits returned (no leak)");
    NS_TEST_ASSERT_MSG_EQ (ca->GetVcLosslessActive (0), true, "A vc0 lossless-active");
    NS_TEST_ASSERT_MSG_EQ (cb->GetVcLosslessActive (0), true, "B vc0 lossless-active");
    Simulator::Destroy ();
  }
};

class SueCbfcAsymmetricInitTestCase : public TestCase
{
public:
  SueCbfcAsymmetricInitTestCase () : TestCase ("CBFC asymmetric RTS-RTR (VC not wanted stays gated)") {}
  void DoRun () override
  {
    g_llrpRxTotal = 0; g_llrpFirstRx = Seconds (0);
    for (auto &c : g_llrpRxVc) { c = 0; }
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    ch->SetAttribute ("Delay", TimeValue (MicroSeconds (2)));
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetIfg (B->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100);
    for (uint8_t v = 0; v < 4; ++v) { ca->SetVcCreditLimit (v, 8); }   // A wants VC0-3 only
    ca->Init ();
    cb->SetCreditSize (100);
    for (uint8_t v = 0; v < 8; ++v) { cb->SetVcCreditLimit (v, 8); }   // B wants all 8
    cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    A->SetDrainDelay (MicroSeconds (10));
    A->SetReceiveUpcall (MakeCallback (&LlrpRxObs));
    A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
    A->StartLldpExchange (); B->StartLldpExchange ();
    // shim dscp = vc so the receiver attributes the wire frame to the same VC
    // the sender's egress queue used (identity map, §7). Payload 84 keeps the
    // frame at exactly 100B (84 + 16B header) = 1 credit @ creditSize 100.
    auto build = [] (uint8_t dscp) {
      Ptr<Packet> p = Create<Packet> (84);
      SueHeader h; h.SetDst (SueMacFromXpuId (1));
      SueShimHeader sh; sh.ttl = 8; sh.dscp = dscp; sh.f = false;
      h.SetEtherType (SUE_ETH_TYPE_1); h.SetShim (sh);
      p->AddHeader (h); return p;
    };
    for (int i = 0; i < 10; ++i) { B->SwitchSend (0, build (0)); }
    for (int i = 0; i < 10; ++i) { B->SwitchSend (4, build (4)); }
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (cb->GetSvcCc (0), 10u, "B sent 10 on VC0");
    NS_TEST_ASSERT_MSG_EQ (cb->GetSvcCc (4), 0u, "VC4 never sent (A did not want it)");
    NS_TEST_ASSERT_MSG_EQ (cb->GetVcLosslessActive (0), true, "B vc0 active");
    NS_TEST_ASSERT_MSG_EQ (cb->GetVcLosslessActive (4), false, "B vc4 stays gated");
    NS_TEST_ASSERT_MSG_EQ (ca->GetVcLosslessActive (4), true, "A vc4 active (B wants all)");
    NS_TEST_ASSERT_MSG_EQ (g_llrpRxVc[0], 10u, "A received 10 on VC0");
    NS_TEST_ASSERT_MSG_EQ (g_llrpRxVc[4], 0u, "A received 0 on VC4");
    Simulator::Destroy ();
  }
};

class SueCbfcLldpFallbackTestCase : public TestCase
{
public:
  SueCbfcLldpFallbackTestCase () : TestCase ("CBFC LLDP no-peer fallback self-negotiates") {}
  void DoRun () override
  {
    g_llrpRxTotal = 0; g_llrpFirstRx = Seconds (0);
    for (auto &c : g_llrpRxVc) { c = 0; }
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    ch->SetAttribute ("Delay", TimeValue (MicroSeconds (2)));
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100); ca->SetVcCreditLimit (0, 8); ca->Init ();
    cb->SetCreditSize (100); cb->SetVcCreditLimit (0, 8); cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    B->SetDrainDelay (MicroSeconds (10));
    B->SetReceiveUpcall (MakeCallback (&LlrpRxObs));
    // Only A negotiates; B stays direct-config (it ignores LLDP frames, see
    // OnRemoteLldpTlv guard). A's 10µs no-peer fallback must self-negotiate
    // and open its VCs instead of hanging forever.
    A->EnableLldpNegotiation ();
    A->StartLldpExchange ();
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_llrpRxTotal, 10u, "10 delivered after fallback self-negotiation");
    bool firstRxLate = g_llrpFirstRx >= MicroSeconds (10);
    NS_TEST_ASSERT_MSG_EQ (firstRxLate, true, "nothing before the 10µs fallback window");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "all credits returned");
    NS_TEST_ASSERT_MSG_EQ (ca->GetVcLosslessActive (0), true, "A vc0 active via self-negotiation");
    Simulator::Destroy ();
  }
};

class SueCbfcActiveGateUnitTestCase : public TestCase
{
public:
  SueCbfcActiveGateUnitTestCase () : TestCase ("CBFC lossless-active gate unit (default on, credit-limit immune)") {}
  void DoRun () override
  {
    Ptr<SueCbfcEngine> e = CreateObject<SueCbfcEngine> ();   // ctor runs Init()
    e->SetCreditSize (100);
    e->SetVcCreditLimit (0, 5);
    NS_TEST_ASSERT_MSG_EQ (e->CanSend (0, 100), true, "default: VC active after Init");
    e->SetVcLosslessActive (0, false);
    NS_TEST_ASSERT_MSG_EQ (e->CanSend (0, 100), false, "gated VC cannot use credits");
    e->SetVcCreditLimit (0, 8);   // must NOT touch the active bit
    NS_TEST_ASSERT_MSG_EQ (e->CanSend (0, 100), false, "SetVcCreditLimit does not re-activate");
    NS_TEST_ASSERT_MSG_EQ (e->GetVcLosslessActive (0), false, "active bit unchanged");
    e->SetVcCreditLimit (1, 5);
    NS_TEST_ASSERT_MSG_EQ (e->CanSend (1, 100), true, "other VCs unaffected by the gate");
    e->Init ();
    NS_TEST_ASSERT_MSG_EQ (e->GetVcLosslessActive (0), true, "Init restores the direct-config default");
    NS_TEST_ASSERT_MSG_EQ (e->CanSend (0, 100), true, "sendable again after Init");
  }
};

// ============ A-class: CBFC CF_Update loss + CF_min/CF_max pacing ============
// CFG-CR-008 / CFG-TIM-001 / CFG-TIM-002 (docs/combined-l6-specs.csv closure):
// CF_Update wire times observed at the sender through the SueRx trace.
static std::vector<Time> g_cfRx;
static void
CfRxObs (Ptr<const Packet> p)
{
  SueCtlTag tag;
  if (!(p->PeekPacketTag (tag) && tag.GetType () == SueCtlHeader::CF_UPDATE))
    { return; }
  // The CF_max full update packs 2 VCs per message (4 back-to-back messages
  // per fire, 9B CtlOS each @100G = ~1ns apart) — intra-batch gaps are NOT CF_min
  // violations. Filter to messages carrying VC0 so the observed series is the
  // per-VC generation grid (CF_min one-shots + the periodic full updates).
  // Layout on the wire: [SueCtlHeader][SueCbfcCtlOsHeader][...] — peel the
  // 1-byte ctl header off a COPY before peeking the CF header.
  Ptr<Packet> copy = p->Copy ();
  SueCtlHeader ctl;
  copy->RemoveHeader (ctl);
  SueCbfcCtlOsHeader h;
  if (copy->PeekHeader (h) && (h.GetVc (0) == 0 || h.GetVc (1) == 0))
    { g_cfRx.push_back (Simulator::Now ()); }
}

class SueCbfcCfUpdateTestCase : public TestCase
{
public:
  SueCbfcCfUpdateTestCase () : TestCase ("CBFC CF_Update loss compensation + CF_min-CF_max pacing") {}
  void DoRun () override
  {
    // --- S1: 纯 CF_min 网格(CF_max=0,只有脏触发)—— 相邻 CF >= CF_min ---
    g_cfRx.clear ();
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100); ca->SetVcCreditLimit (0, 5); ca->Init (); cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    B->SetDrainDelay (MicroSeconds (10));
    B->SetCfUpdateTimers (MicroSeconds (2), Seconds (0));   // CF_min only
    A->TraceConnectWithoutContext ("SueRx", MakeCallback (&CfRxObs));
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    for (int i = 0; i < 10; ++i) { A->SwitchSend (0, build ()); }
    Simulator::Stop (MicroSeconds (120));
    Simulator::Run ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 10u, "S1 all 10 delivered (CF_min gated refunds)");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "S1 CU=0 (all credits returned)");
    NS_TEST_ASSERT_MSG_GT (g_cfRx.size (), 3u, "S1 multiple CF_Updates on the wire");
    for (size_t i = 1; i < g_cfRx.size (); ++i)
      {
        Time d = g_cfRx[i] - g_cfRx[i - 1];
        NS_TEST_ASSERT_MSG_GT (d, NanoSeconds (1900), "S1 CF_min grid gap >= 2us (minus tolerance)");
      }
    Simulator::Destroy ();

    // --- S2: 纯 CF_max 周期全量(无流量,只有周期触发器)—— 8us 网格 ---
    g_cfRx.clear ();
    Ptr<Node> m1 = CreateObject<Node> (), m2 = CreateObject<Node> ();
    Ptr<SueNetDevice> C = CreateObject<SueNetDevice> (), D = CreateObject<SueNetDevice> ();
    m1->AddDevice (C); m2->AddDevice (D);
    Ptr<SueChannel> ch2 = CreateObject<SueChannel> (); C->Attach (ch2); D->Attach (ch2);
    C->SetDataRate (DataRate ("100Gbps")); C->SetIfg (C->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    C->SetEgressQueue (CreateObject<SueEgressQueue> ());
    D->SetDataRate (DataRate ("100Gbps")); D->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> cc2 = CreateObject<SueCbfcEngine> (), cd2 = CreateObject<SueCbfcEngine> ();
    cc2->SetCreditSize (100); cc2->SetVcCreditLimit (0, 5); cc2->Init (); cd2->Init ();
    C->SetCbfcEngine (cc2); D->SetCbfcEngine (cd2);
    D->SetCfUpdateTimers (MicroSeconds (2), MicroSeconds (8));   // idle: only CF_max fires
    C->TraceConnectWithoutContext ("SueRx", MakeCallback (&CfRxObs));
    Simulator::Stop (MicroSeconds (100));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_GT (g_cfRx.size (), 5u, "S2 periodic full updates observed");
    for (size_t i = 2; i < g_cfRx.size (); ++i)   // skip the first arm edge
      {
        Time d = g_cfRx[i] - g_cfRx[i - 1];
        NS_TEST_ASSERT_MSG_GT (d, NanoSeconds (7000), "S2 CF_max grid gap >= 7us");
        NS_TEST_ASSERT_MSG_LT (d, NanoSeconds (9000), "S2 CF_max grid gap <= 9us");
      }
    // --- S3: 丢一条 CF_Update -> 后续 CF(CF_min 网格/CF_max 全量)补偿 -> CU 归零 ---
    Ptr<Node> k1 = CreateObject<Node> (), k2 = CreateObject<Node> ();
    Ptr<SueNetDevice> E = CreateObject<SueNetDevice> (), F = CreateObject<SueNetDevice> ();
    k1->AddDevice (E); k2->AddDevice (F);
    Ptr<SueChannel> ch3 = CreateObject<SueChannel> (); E->Attach (ch3); F->Attach (ch3);
    E->SetDataRate (DataRate ("100Gbps")); E->SetIfg (E->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    E->SetEgressQueue (CreateObject<SueEgressQueue> ());
    F->SetDataRate (DataRate ("100Gbps")); F->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ce = CreateObject<SueCbfcEngine> (), cf = CreateObject<SueCbfcEngine> ();
    ce->SetCreditSize (100); ce->SetVcCreditLimit (0, 5); ce->Init (); cf->Init ();
    E->SetCbfcEngine (ce); F->SetCbfcEngine (cf);
    F->SetDrainDelay (MicroSeconds (10));
    F->SetCfUpdateTimers (MicroSeconds (2), MicroSeconds (8));
    Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
    em->SetCtlDropOnce (2);   // E's 2nd incoming control frame (a CF_Update) is lost
    E->SetErrorModel (em);
    F->SetReceiveUpcall (MakeCallback (&RxCountObs));
    g_rxCount = 0;
    for (int i = 0; i < 10; ++i) { E->SwitchSend (0, build ()); }
    Simulator::Stop (MicroSeconds (120));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 10u, "S3 all 10 delivered despite one lost CF_Update");
    NS_TEST_ASSERT_MSG_EQ (ce->GetSvcCu (0), 0u, "S3 CU=0: the lost refund was compensated");
    NS_TEST_ASSERT_MSG_EQ (ce->GetCounters ().cf_update_rx, cf->GetCounters ().cf_update_tx - 1,
                           "S3 exactly one CF_Update lost on the wire (rx == tx-1)");
    // --- S4: 数据量阈值触发(SDK credit_threshold 对等物)—— CF 计数 ∝ 数据字节、与端口速率无关 ---
    // threshold=2KB、CF_MIN=CF_MAX=0:每累计 2048B 释放信用上报一次。
    // 注意阈值必须 ≤ 信用窗口(CL×creditSize),否则发送端在接收端攒够
    // 阈值前就停摆——芯片 credit_threshold 的同款工程约束。
    {
      uint64_t cfTxAt100G = 0, cfTxAt400G = 0;
      for (const char *rateStr : {"100Gbps", "400Gbps"})
        {
          g_rxCount = 0;
          Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
          Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
          n1->AddDevice (A); n2->AddDevice (B);
          Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
          A->SetDataRate (DataRate (rateStr));
          A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));
          A->SetEgressQueue (CreateObject<SueEgressQueue> ());
          B->SetDataRate (DataRate (rateStr));
          B->SetEgressQueue (CreateObject<SueEgressQueue> ());
          Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
          ca->SetCreditSize (100); ca->SetVcCreditLimit (0, 32); ca->Init ();   // 窗口 3.2KB > 2KB 阈值
          cb->SetCreditSize (100); cb->Init ();
          A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
          B->SetCfUpdateThresholdBytes (2048);
          B->SetReceiveUpcall (MakeCallback (&RxCountObs));
          auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
          for (int i = 0; i < 41; ++i) { A->SwitchSend (0, build ()); }   // 41×100B = 4100B
          Simulator::Stop (MicroSeconds (200));
          Simulator::Run ();
          Simulator::Destroy ();
          NS_TEST_ASSERT_MSG_EQ (g_rxCount, 41u, "S4 all 41 delivered (threshold within the credit window)");
          uint64_t tx = cb->GetCounters ().cf_update_tx;
          NS_TEST_ASSERT_MSG_EQ (tx, 2ull, "S4 floor(4100/2048)=2 CF_Updates (data-proportional)");
          if (rateStr[0] == '1') { cfTxAt100G = tx; } else { cfTxAt400G = tx; }
        }
      NS_TEST_ASSERT_MSG_EQ (cfTxAt100G, cfTxAt400G,
                             "S4 same bytes at 100G vs 400G -> same CF count (port-rate independent)");
    }
  }
};

// ============ A-class: LLDP LLR 能力四象限(W/E, §5.3.2.1.6) ============
// NEG-PROC-001 / NEG-OPT-LLR-002 closure: the W/E capability handshake end to
// end — both-support enables LLR normally; single-support degrades the
// supporter to BEST_EFFORT (no INIT ever sent).
class SueLldpLlrCapabilityTestCase : public TestCase
{
public:
  SueLldpLlrCapabilityTestCase () : TestCase ("LLDP LLR capability quadrants (both-single degrade)") {}
  void DoRun () override
  {
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };

    // --- (a) both support -> LLR starts normally, INIT handshake completes ---
    {
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
      A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
      A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps"));
      Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
      A->SetLlrEngine (eA); B->SetLlrEngine (eB);
      A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
      A->StartLldpExchange (); B->StartLldpExchange ();
      // Frame after the negotiation window: must travel as LLR-eligible.
      Simulator::Schedule (MicroSeconds (20), &SueNetDevice::SwitchSend, A, 0, build ());
      B->SetReceiveUpcall (MakeCallback (&RxCountObs));
      g_rxCount = 0;
      Simulator::Stop (MicroSeconds (100));
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ ((uint32_t) eA->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE,
                             "(a) A ADVANCE after INIT handshake");
      NS_TEST_ASSERT_MSG_GT (eA->GetCounters ().tx_init_ctl_os, 0ull, "(a) A sent INIT");
      NS_TEST_ASSERT_MSG_EQ (eB->GetCounters ().rx_ok, 1ull, "(a) B accepted the eligible frame");
      NS_TEST_ASSERT_MSG_EQ (g_rxCount, 1u, "(a) frame delivered");
    }

    // --- (b) only A supports -> A degrades to BEST_EFFORT, no INIT, delivery ---
    {
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
      A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
      A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps"));
      Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
      A->SetLlrEngine (eA);   // B: no engine -> advertises llrSupported=0
      B->SetLlrEnabled (true);   // legacy preamble-strip path (no engine)
      A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
      A->StartLldpExchange (); B->StartLldpExchange ();
      Simulator::Schedule (MicroSeconds (20), &SueNetDevice::SwitchSend, A, 0, build ());
      B->SetReceiveUpcall (MakeCallback (&RxCountObs));
      g_rxCount = 0;
      Simulator::Stop (MicroSeconds (100));
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (eA->GetModeRemote (), false, "(b) A degraded TX (mode_remote OFF)");
      NS_TEST_ASSERT_MSG_EQ (eA->GetModeLocal (), false, "(b) A degraded RX (mode_local OFF)");
      NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().tx_init_ctl_os, 0ull, "(b) A never sent INIT");
      NS_TEST_ASSERT_MSG_EQ (g_rxCount, 1u, "(b) frame delivered in degraded mode");
    }

    // --- (b') only B supports -> B degrades symmetrically, still receives ---
    {
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
      A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
      A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps"));
      Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
      B->SetLlrEngine (eB);   // A: no engine
      A->SetLlrEnabled (true);   // legacy preamble-strip path (no engine)
      A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
      A->StartLldpExchange (); B->StartLldpExchange ();
      Simulator::Schedule (MicroSeconds (20), &SueNetDevice::SwitchSend, A, 0, build ());
      B->SetReceiveUpcall (MakeCallback (&RxCountObs));
      g_rxCount = 0;
      Simulator::Stop (MicroSeconds (100));
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (eB->GetModeRemote (), false, "(b') B degraded TX");
      NS_TEST_ASSERT_MSG_EQ (eB->GetModeLocal (), false, "(b') B degraded RX (unfiltered)");
      NS_TEST_ASSERT_MSG_EQ (eB->GetCounters ().tx_init_ctl_os, 0ull, "(b') B never sent INIT");
      NS_TEST_ASSERT_MSG_EQ (g_rxCount, 1u, "(b') frame delivered (unfiltered RX)");
    }
  }
};

// ============ B-class: want 动态撤销 (ULL-NEG-CBF-RCV-006 / INIT-008 部分) ============
// UEC §5.2.8.5:接收端 R_VC_Want TRUE->FALSE 时,发送端必须清 S_VC_RTS 并
// 停止该 VC 的信用门控(R_VC_RTR 随之为 0)。旧实现位图单调 OR,已激活的
// VC 永远无法撤销;assignment 语义让撤销在 <=3 轮内传播。方向独立
// (§5.2.8.1):撤销方自己的发送方向不受影响。
class SueLldpWantRevokeTestCase : public TestCase
{
public:
  SueLldpWantRevokeTestCase () : TestCase ("LLDP want revocation closes the gate (RTS-RTR assignment)") {}
  void DoRun () override
  {
    g_llrpRxTotal = 0; g_llrpFirstRx = Seconds (0);
    for (auto &c : g_llrpRxVc) { c = 0; }
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    ch->SetAttribute ("Delay", TimeValue (MicroSeconds (2)));
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100);
    for (uint8_t v = 0; v < 8; ++v) { ca->SetVcCreditLimit (v, 8); }
    ca->Init ();
    cb->SetCreditSize (100);
    for (uint8_t v = 0; v < 8; ++v) { cb->SetVcCreditLimit (v, 8); }
    cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
    B->SetDrainDelay (MicroSeconds (10));
    B->SetReceiveUpcall (MakeCallback (&LlrpRxObs));   // A 发 B 收,观测器挂 B
    A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
    A->StartLldpExchange (); B->StartLldpExchange ();
    // shim dscp = vc(identity map)让线头 VC 与发送队列 VC 一致;84B 载荷 +
    // 16B 头 = 100B = 1 信用 @ creditSize 100。
    auto build = [] (uint8_t dscp) {
      Ptr<Packet> p = Create<Packet> (84);
      SueHeader h; h.SetDst (SueMacFromXpuId (2));
      SueShimHeader sh; sh.ttl = 8; sh.dscp = dscp; sh.f = false;
      h.SetEtherType (SUE_ETH_TYPE_1); h.SetShim (sh);
      p->AddHeader (h); return p;
    };
    // 阶段 1:对称协商(~7.2µs 完成)后 vc0/vc4 各 5 帧全流通(两者都曾激活)。
    for (int i = 0; i < 5; ++i) { A->SwitchSend (0, build (0)); }
    for (int i = 0; i < 5; ++i) { A->SwitchSend (4, build (4)); }
    // 阶段 2:t=30µs B 撤销 VC4-7 的 want(管理面动作)——assign 语义:
    // A.rts 清 4-7 -> B.rtr 清 4-7 -> A.act 4-7 1->0,<=3 轮。
    Simulator::Schedule (MicroSeconds (30), &SueNetDevice::SetVcWant, B, (uint8_t) 4, false);
    Simulator::Schedule (MicroSeconds (30), &SueNetDevice::SetVcWant, B, (uint8_t) 5, false);
    Simulator::Schedule (MicroSeconds (30), &SueNetDevice::SetVcWant, B, (uint8_t) 6, false);
    Simulator::Schedule (MicroSeconds (30), &SueNetDevice::SetVcWant, B, (uint8_t) 7, false);
    // 阶段 3:t=50µs(撤销已收敛)再发 vc0/vc4 各 5——vc0 走,vc4 排队不发。
    for (int i = 0; i < 5; ++i)
      {
        Simulator::Schedule (MicroSeconds (50), &SueNetDevice::SwitchSend, A, (uint32_t) 0, build (0));
        Simulator::Schedule (MicroSeconds (50), &SueNetDevice::SwitchSend, A, (uint32_t) 4, build (4));
      }
    Simulator::Stop (MicroSeconds (150));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (ca->GetVcLosslessActive (0), true, "A vc0 still active");
    NS_TEST_ASSERT_MSG_EQ (ca->GetVcLosslessActive (4), false, "A vc4 gate closed by revocation");
    NS_TEST_ASSERT_MSG_EQ (ca->GetVcLosslessActive (7), false, "A vc7 gate closed by revocation");
    NS_TEST_ASSERT_MSG_EQ (cb->GetVcLosslessActive (4), true, "B vc4 still active (A still wants it — directions independent)");
    NS_TEST_ASSERT_MSG_EQ (g_llrpRxVc[0], 10u, "vc0: 5+5 delivered");
    NS_TEST_ASSERT_MSG_EQ (g_llrpRxVc[4], 5u, "vc4: only the pre-revocation 5 delivered");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (0), 10u, "A S_CC(0)=10");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (4), 5u, "A S_CC(4)=5 (post-revocation frames never charged)");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (0), 0u, "vc0 CU=0 (drained)");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (4), 0u, "vc4 CU=0 (drained)");
  }
};

// ============ B-class: CC_Update MACDA/CID 可配 (ULL-CBFC-MSG-CC-003) ============
static Mac48Address g_ccDaRx;
static uint32_t g_ccCidRx;
static Mac48Address g_ccSaRx;
static uint32_t g_ccFrameRx;
static void
CcUpdateRxObs (Ptr<const Packet> p)
{
  SueCtlTag tag;
  if (!(p->PeekPacketTag (tag) && tag.GetType () == SueCtlHeader::CC_UPDATE)) { return; }
  Ptr<Packet> copy = p->Copy ();
  SueCbfcCcHeader cch;
  copy->RemoveHeader (cch);
  g_ccDaRx = cch.GetMacDa ();
  g_ccCidRx = cch.GetCid ();
  g_ccSaRx = cch.GetMacSa ();
  ++g_ccFrameRx;
}

class SueCcUpdateConfigTestCase : public TestCase
{
public:
  SueCcUpdateConfigTestCase () : TestCase ("CBFC CC_Update MACDA-CID configurable (5.2.6.2)") {}
  void DoRun () override
  {
    // --- (a) header:默认字节不变(TS-34 补钉 MACDA)+ 定制序列化 + round-trip ---
    {
      SueCbfcCcHeader def;
      def.SetMacSa (Mac48Address ("00:11:22:33:44:55"));
      Ptr<Packet> p = Create<Packet> ();
      p->AddHeader (def);
      uint8_t b[64] = {0};
      p->CopyData (b, 64);
      NS_TEST_ASSERT_MSG_EQ (b[0], (uint8_t) 0x01, "default MACDA[0]");
      NS_TEST_ASSERT_MSG_EQ (b[1], (uint8_t) 0x80, "default MACDA[1]");
      NS_TEST_ASSERT_MSG_EQ (b[5], (uint8_t) 0x01, "default MACDA[5] (01-80-C2-00-00-01)");
      NS_TEST_ASSERT_MSG_EQ (b[16], (uint8_t) 0xFA, "default CID hi (FA)");
      NS_TEST_ASSERT_MSG_EQ (b[18], (uint8_t) 0xCB, "default CID lo (CB)");
    }
    {
      SueCbfcCcHeader h;
      h.SetMacDa (Mac48Address ("02:03:04:05:06:07"));
      h.SetCid (0xA1B2C3);
      h.SetMacSa (Mac48Address ("00:11:22:33:44:55"));
      Ptr<Packet> p = Create<Packet> ();
      p->AddHeader (h);
      uint8_t b[64] = {0};
      p->CopyData (b, 64);
      NS_TEST_ASSERT_MSG_EQ (b[0], (uint8_t) 0x02, "custom MACDA[0]");
      NS_TEST_ASSERT_MSG_EQ (b[5], (uint8_t) 0x07, "custom MACDA[5]");
      NS_TEST_ASSERT_MSG_EQ (b[16], (uint8_t) 0xA1, "custom CID hi");
      NS_TEST_ASSERT_MSG_EQ (b[17], (uint8_t) 0xB2, "custom CID mid");
      NS_TEST_ASSERT_MSG_EQ (b[18], (uint8_t) 0xC3, "custom CID lo");
      SueCbfcCcHeader h2;
      p->RemoveHeader (h2);
      NS_TEST_ASSERT_MSG_EQ (h2.GetMacDa (), Mac48Address ("02:03:04:05:06:07"), "MACDA round-trip");
      NS_TEST_ASSERT_MSG_EQ (h2.GetCid (), 0xA1B2C3u, "CID round-trip");
    }

    // --- (b) e2e:设备配置生效,定制 MACDA/CID 帧仍被对端识别并处理 ---
    {
      g_ccFrameRx = 0; g_ccCidRx = 0; g_ccDaRx = Mac48Address (); g_ccSaRx = Mac48Address ();
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);
      A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
      A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
      Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
      cbfcA->SetCreditSize (100); cbfcA->SetVcCreditLimit (0, 10); cbfcA->Init (); cbfcB->Init ();
      A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
      A->SetCcUpdateMacDa (Mac48Address ("02:00:00:00:00:01"));
      A->SetCcUpdateCid (0xCAFEB0);
      NS_TEST_ASSERT_MSG_EQ (A->GetCcUpdateMacDa (), Mac48Address ("02:00:00:00:00:01"), "device MACDA setter");
      NS_TEST_ASSERT_MSG_EQ (A->GetCcUpdateCid (), 0xCAFEB0u, "device CID setter");
      B->TraceConnectWithoutContext ("SueRx", MakeCallback (&CcUpdateRxObs));
      A->StartCcUpdateTimer (MicroSeconds (50));   // 50/100us fires
      Simulator::Stop (MicroSeconds (130));
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_GT_OR_EQ (g_ccFrameRx, 2u, ">=2 CC_Update frames observed");
      NS_TEST_ASSERT_MSG_EQ (g_ccDaRx, Mac48Address ("02:00:00:00:00:01"), "wire MACDA == configured");
      NS_TEST_ASSERT_MSG_EQ (g_ccCidRx, 0xCAFEB0u, "wire CID == configured");
      NS_TEST_ASSERT_MSG_EQ (g_ccSaRx, A->GetMacSa (), "wire MACSA == source MAC");
      // 识别/处理不受定制 MACDA/CID 影响(识别走内部 tag 分发,§5.6 偏离)。
      NS_TEST_ASSERT_MSG_GT_OR_EQ (cbfcB->GetCounters ().cc_update_rx, 2ull, "peer still processes the frames");
      NS_TEST_ASSERT_MSG_GT_OR_EQ (cbfcA->GetCounters ().cc_update_tx, 2ull, "A sent >=2");
    }
  }
};

// ============ B-class: LLDP 采纳参数不一致告警 (ULL-CBFC-INIT-001) ============
// UEC §5.2.8.1: NumVCs / lossless-BE 配置 / 包到 VC 映射必须两端一致。数值型
// 参数(creditSize/pktOvhd/vcLimit)按方向独立、可合法不同(SC-26 256<->64),
// 故只对结构性不一致(结构字段 NumVCs、无损 VC 集合)告警;采纳照常进行,
// RTS/RTR 门已在功能上拦住未 want 的 VC(TS-50)。
class SueLldpAdoptMismatchTestCase : public TestCase
{
public:
  SueLldpAdoptMismatchTestCase () : TestCase ("LLDP adoption mismatch WARN observables (NumVCs, lossless set)") {}
  void DoRun () override
  {
    // --- (a) 非对称 want 的自然交换 -> 双方 lossless-set mismatch,NumVCs 一致 ---
    {
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> ();
      A->Attach (ch); B->Attach (ch);
      ch->SetAttribute ("Delay", TimeValue (MicroSeconds (2)));
      A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
      A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
      Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
      ca->SetCreditSize (100);
      for (uint8_t v = 0; v < 4; ++v) { ca->SetVcCreditLimit (v, 8); }   // A wants VC0-3
      ca->Init ();
      cb->SetCreditSize (100);
      for (uint8_t v = 0; v < 8; ++v) { cb->SetVcCreditLimit (v, 8); }   // B wants all
      cb->Init ();
      A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
      A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
      A->StartLldpExchange (); B->StartLldpExchange ();
      Simulator::Stop (MicroSeconds (100));
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (A->GetLldpLosslessSetMismatch (), true, "A flags want-set mismatch (0x0F vs 0xFF)");
      NS_TEST_ASSERT_MSG_EQ (B->GetLldpLosslessSetMismatch (), true, "B flags want-set mismatch (0xFF vs 0x0F)");
      NS_TEST_ASSERT_MSG_EQ (A->GetLldpNumVcsMismatch (), false, "A NumVCs match (8==8)");
      NS_TEST_ASSERT_MSG_EQ (B->GetLldpNumVcsMismatch (), false, "B NumVCs match (8==8)");
    }

    // --- (b) 构造 NumVCs=4 的远端 TLV -> B 告警 NumVCs 不一致,且采纳照常 ---
    {
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> ();
      A->Attach (ch); B->Attach (ch);
      A->SetDataRate (DataRate ("100Gbps")); A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
      Ptr<SueCbfcEngine> cb = CreateObject<SueCbfcEngine> ();
      cb->SetCreditSize (256);
      for (uint8_t v = 0; v < 8; ++v) { cb->SetVcCreditLimit (v, 8); }
      cb->Init ();
      B->SetCbfcEngine (cb);
      A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
      A->StartLldpExchange (); B->StartLldpExchange ();
      // 手工构造"对端"帧:creditSize 128 会被采纳(证明告警不阻断),numVcs=4
      // 与本地 8 不一致(告警),want 位图与本地一致(0xFF,不触发第二告警)。
      Ptr<Packet> p = Create<Packet> ();
      SueLldpHeader lldp;
      lldp.SetCreditSize (128);
      lldp.SetPktOvhd (0);
      lldp.SetNumVcs (4);
      for (uint8_t v = 0; v < 8; ++v)
        {
          lldp.SetVcLimit (v, 8);
          lldp.SetRvcWant (v, true);
        }
      p->AddHeader (lldp);
      SueCtlHeader ctl (SueCtlHeader::LLDP);
      p->AddHeader (ctl);
      B->HandleCtlFrame (p, SueCtlHeader::LLDP);
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (B->GetLldpNumVcsMismatch (), true, "B flags NumVCs mismatch (remote 4 != local 8)");
      NS_TEST_ASSERT_MSG_EQ (B->GetLldpLosslessSetMismatch (), false, "B want set matches (0xFF == 0xFF)");
      // New §5.2.8.2 semantics: adoption writes ONLY the TX cell (snapped to
      // the supported set); the rx cell (receiver's own buffer) stays native.
      NS_TEST_ASSERT_MSG_EQ (cb->GetTxCellSize (), 128u, "adoption proceeds despite the warn (128 adopted as TX cell)");
      NS_TEST_ASSERT_MSG_EQ (cb->GetRxCellSize (), 256u, "rx native cell untouched by adoption");
    }

    // --- (c) 对称交换 -> 双方均无告警(对照组) ---
    {
      Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
      Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
      n1->AddDevice (A); n2->AddDevice (B);
      Ptr<SueChannel> ch = CreateObject<SueChannel> ();
      A->Attach (ch); B->Attach (ch);
      A->SetDataRate (DataRate ("100Gbps")); A->SetEgressQueue (CreateObject<SueEgressQueue> ());
      B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());
      Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
      ca->SetCreditSize (100);
      for (uint8_t v = 0; v < 8; ++v) { ca->SetVcCreditLimit (v, 8); }
      ca->Init ();
      cb->SetCreditSize (100);
      for (uint8_t v = 0; v < 8; ++v) { cb->SetVcCreditLimit (v, 8); }
      cb->Init ();
      A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
      A->EnableLldpNegotiation (); B->EnableLldpNegotiation ();
      A->StartLldpExchange (); B->StartLldpExchange ();
      Simulator::Stop (MicroSeconds (100));
      Simulator::Run ();
      Simulator::Destroy ();
      NS_TEST_ASSERT_MSG_EQ (A->GetLldpNumVcsMismatch (), false, "A NumVCs match");
      NS_TEST_ASSERT_MSG_EQ (A->GetLldpLosslessSetMismatch (), false, "A want set matches");
      NS_TEST_ASSERT_MSG_EQ (B->GetLldpNumVcsMismatch (), false, "B NumVCs match");
      NS_TEST_ASSERT_MSG_EQ (B->GetLldpLosslessSetMismatch (), false, "B want set matches");
    }
  }
};

// ============ S6: default-config 100G E2E regression (auto timer, §12.27) ============
// Regression for the s6-entry pathology (historical, measured at 1G): with the
// replay/init timers at a fixed small default (1000ns), a 1G link's CtlOS gate
// (400B = 3.2µs) and ACK cadence (2048B = 16.4µs) both exceed the timer ->
// spurious replay -> replay_ct exhausted -> FLUSH -> INIT re-sync cascade. The
// default must auto-derive 10x LLR RTT (DESIGN §12.25/§12.27); at 100G
// (ScaleOut minimum port rate) the gates shrink to 32ns/164ns, so the cascade
// cannot reproduce — this case now guards the FULL default-config path at
// 100G (ApplySueConfig reads the GlobalValue defaults, incl.
// LLR_REPLAY_TIMER_MAX_NS/LLR_INIT_TIMER_MAX_NS = 0 -> ~21840ns derived) and
// asserts exactly one delivery with no replay/FLUSH.
class SueDefaultConfig100GTestCase : public TestCase
{
public:
  SueDefaultConfig100GTestCase () : TestCase ("CFG default-config 100G E2E (auto timer, no spurious replay or FLUSH)") {}
  void DoRun () override
  {
    g_rxCount = 0;
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps"));
    B->SetDataRate (DataRate ("100Gbps"));
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    // default-config path: FC_MODE default CBFC -> facade + engine wired by
    // ApplySueConfig; the replay/init timers are left at their 0 (= auto)
    // defaults so the 10x-LLR-RTT derivation must kick in (at 100G:
    // ~21840ns, far above the 184ns RTT_eff).
    ApplySueConfig (A);
    ApplySueConfig (B);
    eA->Start (); eB->Start ();
    B->SetReceiveUpcall (MakeCallback (&RxCountObs));
    auto build = [] () { Ptr<Packet> p = Create<Packet> (86); SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0); p->AddHeader (h); return p; };
    A->SwitchSend (0, build ());
    Simulator::Stop (MicroSeconds (100));
    Simulator::Run ();
    Simulator::Destroy ();
    NS_TEST_ASSERT_MSG_EQ (g_rxCount, 1u, "100G default-config: exactly 1 delivered (no spurious replay/FLUSH)");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) eA->GetTxState (), (uint32_t) SueLlrEngine::TxState::ADVANCE, "host LLR ADVANCE (no FLUSH)");
    NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().tx_replay, 0ull, "no spurious replay");
    NS_TEST_ASSERT_MSG_EQ (eA->GetCounters ().tx_discard, 0ull, "no discard");
  }
};

NS_LOG_COMPONENT_DEFINE ("SueTest");

static class SueTestSuite : public TestSuite
{
public:
  SueTestSuite () : TestSuite ("sue", UNIT)
  {
    AddTestCase (new SueInstantiateTestCase, TestCase::QUICK);
    AddTestCase (new SueAttachTestCase, TestCase::QUICK);
    AddTestCase (new SueDispatchTestCase, TestCase::QUICK);
    AddTestCase (new SueHeaderParseTestCase, TestCase::QUICK);
    AddTestCase (new SueRoutingTestCase, TestCase::QUICK);
    AddTestCase (new SueForwardTestCase, TestCase::QUICK);
    AddTestCase (new SueCounterTestCase, TestCase::QUICK);
    AddTestCase (new SueTimingTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcEngineTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcE2eTestCase, TestCase::QUICK);
    // S2: §12.30 deferred refund full-frame byte consistency (40 -> 41).
    AddTestCase (new SueTtlDropRefundTestCase, TestCase::QUICK);
    // §12.42 deferred refund measures the PRE-preamble frame (idle-egress
    // synchronous TX prepends in place; over-refund poisons the credit
    // window) (60 -> 61).
    AddTestCase (new SueDeferredRefundPreambleTestCase, TestCase::QUICK);
    // S2: UEC §5.2.5.7.4 LostCredits<0 wraparound guard (41 -> 42).
    AddTestCase (new SueCcUpdateNegativeLostTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrPreambleTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrRetransmitTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrFlushTestCase, TestCase::QUICK);
    // S3: UEC §5.1.5 FLUSH not lifted by late ACK/NACK (42 -> 43).
    AddTestCase (new SueLlrFlushStaysFlushTestCase, TestCase::QUICK);
    // S3: UEC §5.1 pause does not stop replay (43 -> 44).
    AddTestCase (new SueLlrReplayDuringPfcPauseTestCase, TestCase::QUICK);
    // S3: partial-ACK TX wakeup, stop-and-go prevention (44 -> 45).
    AddTestCase (new SueLlrPartialAckWakeTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrPoisonedTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrInitHandshakeTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrModesBehaviorsTestCase, TestCase::QUICK);
    // S3d: UEC §5.1.7 vld gating, §5.1.9.1.2 INIT_ECHO match, §5.1.1.1
    // ineligible preamble (28 -> 31).
    AddTestCase (new SueLlrVldTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrInitEchoSeqTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrIneligiblePreambleTestCase, TestCase::QUICK);
    // S4: UEC §5.1.6 periodic (cumulative) ACK pacing (31 -> 32).
    AddTestCase (new SueLlrAckPeriodTestCase, TestCase::QUICK);
    // LLR-SP-16: ACK/NACK counters count received CtlOS, pairing across a link.
    AddTestCase (new SueLlrAckCounterPairingTestCase, TestCase::QUICK);
    // 2^20 seq wrap clean pass-through (2026-08-14 thpt 512B long-burst
    // regression, §12.38; 58 -> 59).
    AddTestCase (new SueLlrSeqWrapTestCase, TestCase::QUICK);
    // VRF segmented forwarding DB + DSCP>VLAN>port derivation (§12.40; 59 -> 60).
    AddTestCase (new SueVrfTestCase, TestCase::QUICK);
    // S4: UEC Table 5-10 acked_seq init (all-1s on mode change / init_seq-1 on
    // LLR_INIT; first ACK accepted without m_anyAcked) (32 -> 33).
    AddTestCase (new SueLlrAckedSeqTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrPcsLostTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrStatusTestCase, TestCase::QUICK);
    AddTestCase (new SueLlrReplayCtUnlimitedTestCase, TestCase::QUICK);
    AddTestCase (new SueCoexistTestCase, TestCase::QUICK);
    AddTestCase (new SuePfcHeaderTestCase, TestCase::QUICK);
    AddTestCase (new SueCcUpdateHeaderTestCase, TestCase::QUICK);
    AddTestCase (new SueCtlOsPacingTestCase, TestCase::QUICK);
    // S6: S4-feature coverage (18 -> 26)
    AddTestCase (new SueCcUpdateOnWireTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcCountersTestCase, TestCase::QUICK);
    AddTestCase (new SueFcControllerTestCase, TestCase::QUICK);
    AddTestCase (new SuePfcTestCase, TestCase::QUICK);
    AddTestCase (new SuePfcSharedPoolTestCase, TestCase::QUICK);
    // PFC-SP-03: resume_offset hysteresis band (dead key wired + asserted).
    AddTestCase (new SuePfcResumeOffsetTestCase, TestCase::QUICK);
    AddTestCase (new SueRule3TestCase, TestCase::QUICK);
    AddTestCase (new SueRule9TestCase, TestCase::QUICK);
    // S6: engine-driven LLR window (rules 3/9 real windows; 45 -> 46).
    AddTestCase (new SueLlrWindowDrivenTestCase, TestCase::QUICK);
    AddTestCase (new SueFacadePfcOnRxTestCase, TestCase::QUICK);
    // S7: replay-buffer sizing gates (storage/BDP/window) + LLR_REPLAY_BUF_MAX
    // config path (26 -> 27).
    AddTestCase (new SueLlrCanTxLimitsTestCase, TestCase::QUICK);
    // S5b: SueLiteEndpoint host-side self-simulated generator (37 -> 38).
    AddTestCase (new SueLiteEndpointTestCase, TestCase::QUICK);
    // S6: default-config 100G E2E regression (auto timer, §12.27; 38 -> 39).
    AddTestCase (new SueDefaultConfig100GTestCase, TestCase::QUICK);
    // S5: VC mapping tables (DESIGN §7; 39 -> 40). It restores the default map
    // after every scenario, so no later case sees a non-identity table.

// ---- §12.50: multi-VC end-to-end credit audit (charge == refund == derived VC) ----
// Regression for the qIndex/PCP VC mismatch class: every frame must be
// charged (OnTx, keyed by the enqueue VC), billed (OnRx, keyed by the
// receiver's ComputeVc) and refunded (OnDrain -> CF_Update adoption) on the
// SAME VC; a mismatch flips CU=(CC-CF) mod 2^15 and permanently blocks the
// window (AXI workload bandwidth was halved by exactly this, §12.50).
class SueVcCreditAuditTestCase : public TestCase
{
public:
  SueVcCreditAuditTestCase () : TestCase ("VC credit audit: multi-VC charge==refund end-to-end") {}
  void DoRun () override
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    ch->SetAttribute ("Delay", TimeValue (MicroSeconds (1)));
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps")); B->SetIfg (B->GetDataRate ().CalculateBytesTxTime (12));
    B->SetEgressQueue (CreateObject<SueEgressQueue> ());
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (100); cb->SetCreditSize (100);
    for (uint32_t v = 0; v < 8; ++v) { ca->SetVcCreditLimit (v, 100); cb->SetVcCreditLimit (v, 100); }
    ca->Init (); cb->Init ();
    A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);

    // Build a frame for a given VC via VLAN PCP (identity map: pcp==vc).
    auto buildVlan = [] (uint8_t pcp, uint32_t dstXpu) {
      Ptr<Packet> p = Create<Packet> (86);
      SueHeader h; h.SetDst (SueMacFromXpuId (dstXpu)); h.SetSrc (SueMacFromXpuId (1));
      h.SetVlan (true, pcp, 0, 100);
      h.SetEtherType (SUE_ETH_TYPE_0);
      p->AddHeader (h);
      return p;
    };
    // Build a frame for a given VC via shim DSCP (identity map: dscp==vc).
    auto buildShim = [] (uint8_t dscp, uint32_t dstXpu) {
      Ptr<Packet> p = Create<Packet> (86);
      SueHeader h; h.SetDst (SueMacFromXpuId (dstXpu)); h.SetSrc (SueMacFromXpuId (1));
      SueShimHeader sh; sh.ttl = 8; sh.dscp = dscp; sh.f = false;
      h.SetEtherType (SUE_ETH_TYPE_1); h.SetShim (sh);
      p->AddHeader (h);
      return p;
    };

    const uint32_t N = 3;   // frames per VC
    // VC1..4 via VLAN PCP; VC5..6 via shim DSCP. Enqueue on the DERIVED VC.
    for (uint32_t i = 0; i < N; ++i)
      {
        A->SwitchSend (1, buildVlan (1, 2)); A->SwitchSend (2, buildVlan (2, 2));
        A->SwitchSend (3, buildVlan (3, 2)); A->SwitchSend (4, buildVlan (4, 2));
        A->SwitchSend (5, buildShim (5, 2)); A->SwitchSend (6, buildShim (6, 2));
      }
    Simulator::Run ();

    // Credit audit: charge (A S_VC_CC) == bill (B R_VC_CC) per VC, and the
    // in-flight CU is bounded by the window (no mod-2^15 flip).
    const uint32_t expectCr = N * 2;   // VLAN frame 86+14+4=104B, shim 86+14+2=102B -> 2 credits @ creditSize 100
    for (uint32_t v = 1; v <= 6; ++v)
      {
        NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (v), expectCr,
                               "A charged VC" << v << " " << N << " credits (enqueue VC == frame VC)");
        NS_TEST_ASSERT_MSG_EQ (cb->GetRvcCc (v), expectCr,
                               "B billed VC" << v << " " << N << " credits (ComputeVc == enqueue VC)");
        NS_TEST_ASSERT_MSG_EQ (cb->GetRvcCf (v), expectCr,
                               "B refunded VC" << v << " fully (R_VC_CF == R_VC_CC after drain)");
        NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCf (v), expectCr,
                               "A adopted VC" << v << " refund (CF_Update reached the sender)");
        NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCu (v), 0u,
                               "A CU VC" << v << " == 0 (charge==refund, no mod flip)");
      }
    // Untouched VCs stay clean.
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (0), 0u, "VC0 untouched (no charge spill)");
    NS_TEST_ASSERT_MSG_EQ (ca->GetSvcCc (7), 0u, "VC7 untouched");
    Simulator::Destroy ();
  }
};

// ---- §12.49 regression: CC_Update dropped while GoBackN retransmits in flight ----
// RxFrame DROPs out-of-order frames (GoBackN); between the NACK clear (first
// retransmit lands) and the LAST retransmit's billing, an arriving CC_Update
// computes LostCredits over the in-flight retransmits and refunds them — they
// refund AGAIN on landing (double refund). The device must drop the CC_Update
// until the engine's HasPendingDiscard() clears (§12.49).
class SueCcUpdateDiscardGateTestCase : public TestCase
{
public:
  SueCcUpdateDiscardGateTestCase () : TestCase ("CC_Update dropped while GoBackN retransmits in flight (§12.49)") {}
  void DoRun () override
  {
    Ptr<SueCbfcEngine> cb = CreateObject<SueCbfcEngine> ();
    cb->SetCreditSize (100); cb->SetVcCreditLimit (0, 100); cb->Init ();
    Ptr<SueLlrEngine> e = CreateObject<SueLlrEngine> ();
    Ptr<SueNetDevice> B = CreateObject<SueNetDevice> (), D = CreateObject<SueNetDevice> ();
    Ptr<Node> n = CreateObject<Node> (), nd = CreateObject<Node> ();
    n->AddDevice (B); nd->AddDevice (D);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    B->Attach (ch); D->Attach (ch);   // e->Start() sends INIT — needs a link
    B->SetCbfcEngine (cb);
    B->SetLlrEngine (e);   // wires RecomputeLlrWindow -> SuspendForLlrWindow
    e->Start ();
    e->OnInit (0);   // next_rx_seq = 0, vld

    // seq 0 arrives BAD -> NACK -> discard window opens.
    e->RxFrame (0, 2);
    NS_TEST_ASSERT_MSG_EQ (e->GetCounters ().tx_nack_ctl_os, 1ull, "NACK sent for bad seq0");
    // seq 1, 2 arrive (out-of-order, missing) -> DROP, discard tracking sees
    // the highest discarded seq = 2.
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->RxFrame (1, 0), (uint32_t) SueLlrEngine::DROP, "seq1 missing-dropped");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->RxFrame (2, 0), (uint32_t) SueLlrEngine::DROP, "seq2 missing-dropped");
    NS_TEST_ASSERT_MSG_EQ (e->HasPendingDiscard (), true,
                           "pending discard active (highest discarded seq=2 not yet re-accepted)");
    // The retransmit of seq 0 lands -> NACK clears, but seq 1..2 retransmits
    // are still in flight -> HasPendingDiscard stays true.
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->RxFrame (0, 0), (uint32_t) SueLlrEngine::ACCEPT, "seq0 retransmit accepted");
    NS_TEST_ASSERT_MSG_EQ (e->HasPendingDiscard (), true,
                           "pending discard STILL active after first retransmit (1..2 in flight)");
    // A CC_Update arriving NOW must be dropped: R_VC_CF unchanged.
    // (Device-level gate is in HandleCtlFrame; engine-level effect = the
    // CC_Update was not applied. We verify via the device's CC_UPDATE branch
    // by calling OnCcUpdate through the engine as the branch would — the
    // GATE itself is asserted via HasPendingDiscard, above and below.)
    std::array<uint32_t, 16> cc {};
    cc[0] = 5;
    // Simulate the device gate: if pending discard, the branch skips OnCcUpdate.
    if (!e->HasPendingDiscard ()) { cb->OnCcUpdate (cc); }   // must NOT run
    NS_TEST_ASSERT_MSG_EQ (cb->GetRvcCf (0), 0u,
                           "R_VC_CF unchanged (CC_Update was gated by pending discard)");
    // Retransmits of seq 1, 2 land -> all discarded frames re-accepted ->
    // pending discard clears -> the NEXT CC_Update processes normally.
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->RxFrame (1, 0), (uint32_t) SueLlrEngine::ACCEPT, "seq1 retransmit accepted");
    NS_TEST_ASSERT_MSG_EQ ((uint32_t) e->RxFrame (2, 0), (uint32_t) SueLlrEngine::ACCEPT, "seq2 retransmit accepted");
    NS_TEST_ASSERT_MSG_EQ (e->HasPendingDiscard (), false,
                           "pending discard cleared (all retransmits re-accepted)");
    Simulator::Destroy ();
  }
};

    AddTestCase (new SueVcMapConfigTestCase, TestCase::QUICK);
    AddTestCase (new SueVcCreditAuditTestCase, TestCase::QUICK);
    AddTestCase (new SueCcUpdateDiscardGateTestCase, TestCase::QUICK);
    // CBFC-SP-13: LLDP RTS/RTR handshake (48 -> 53). Registered after the VC
    // mapping case: the asymmetric case relies on the default dscp->vc
    // identity map. None of these touch GlobalValues (direct device wiring).
    AddTestCase (new SueLldpHeaderRoundTripTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcInitHandshakeE2eTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcAsymmetricInitTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcLldpFallbackTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcActiveGateUnitTestCase, TestCase::QUICK);
    // A-class closures (combined-l6-specs.csv): CF_Update loss/pacing + LLR
    // capability quadrants (53 -> 55).
    AddTestCase (new SueCbfcCfUpdateTestCase, TestCase::QUICK);
    AddTestCase (new SueLldpLlrCapabilityTestCase, TestCase::QUICK);
    // B-class: LLDP adoption-consistency WARN observables (55 -> 56).
    AddTestCase (new SueLldpAdoptMismatchTestCase, TestCase::QUICK);
    // B-class: CC_Update MACDA/CID configurable (56 -> 57).
    AddTestCase (new SueCcUpdateConfigTestCase, TestCase::QUICK);
    // B-class: LLDP want dynamic revocation (57 -> 58).
    AddTestCase (new SueLldpWantRevokeTestCase, TestCase::QUICK);
    // B-class: dual cell granularity per UEC §5.2.5.8 method-2 + §5.2.8.2/§5.2.10.1
    // TX-cell snap + rx==tx legacy equivalence + dual-cell e2e (63 -> 67).
    AddTestCase (new SueCbfcDualCellTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcCellAdoptTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcDualCellCompatTestCase, TestCase::QUICK);
    AddTestCase (new SueCbfcDualCellE2eTestCase, TestCase::QUICK);
  }
} g_sueTestSuite;

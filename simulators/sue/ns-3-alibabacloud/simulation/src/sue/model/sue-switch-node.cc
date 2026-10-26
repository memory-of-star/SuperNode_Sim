/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE Scale-Up Ethernet - SUESwitchNode (S1: AFH Gen1 forwarding + AFH counters).
 */
#include "sue-switch-node.h"
#include "sue-net-device.h"
#include "sue-cbfc-engine.h"
#include "sue-fc-controller.h"
#include "sue-routing.h"
#include "ns3/sue-config.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SUESwitchNode");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SUESwitchNode);

TypeId
SUESwitchNode::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SUESwitchNode")
    .SetParent<Node> ()
    .AddConstructor<SUESwitchNode> ()
  ;
  return tid;
}

SUESwitchNode::SUESwitchNode ()
{
  NS_LOG_FUNCTION_NOARGS ();
  ResetCounters ();
}

SUESwitchNode::~SUESwitchNode ()
{
  NS_LOG_FUNCTION_NOARGS ();
}

void
SUESwitchNode::DoDispose (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  m_ports.clear ();
  m_pending.clear ();
  m_rt = 0;
  Node::DoDispose ();
}

uint32_t
SUESwitchNode::AddPort (Ptr<SueNetDevice> dev)
{
  NS_LOG_FUNCTION (this << dev);
  uint32_t idx = m_ports.size ();
  m_ports.push_back (dev);
  m_portIdxByDev[dev] = idx;            // §12.40 ingress-dev -> port index
  m_pending.resize (m_ports.size ());   // per-(port, VC) backpressure buckets (§12.30)
  AddDevice (dev);   // aggregate to this node so dev->GetNode() resolves (channel dispatch)
  dev->SetReceiveUpcall (MakeCallback (&SUESwitchNode::SwitchReceiveFromDevice, this));
  return idx;
}

Ptr<SueNetDevice>
SUESwitchNode::GetPort (uint32_t i) const
{
  return m_ports[i];
}

uint32_t
SUESwitchNode::GetNPorts () const
{
  return m_ports.size ();
}

void
SUESwitchNode::SetRoutingTable (Ptr<SueRoutingTable> rt)
{
  SetRoutingTable (0, rt);
}

Ptr<SueRoutingTable>
SUESwitchNode::GetRoutingTable () const
{
  return m_rt;
}

void
SUESwitchNode::SetRoutingTable (uint32_t vrf, Ptr<SueRoutingTable> rt)
{
  if (vrf == 0) { m_rt = rt; }
  else { m_rtByVrf[vrf] = rt; }
}

Ptr<SueRoutingTable>
SUESwitchNode::GetRoutingTable (uint32_t vrf) const
{
  if (vrf == 0) { return m_rt; }
  auto it = m_rtByVrf.find (vrf);
  return (it == m_rtByVrf.end ()) ? nullptr : it->second;
}

void
SUESwitchNode::SetEcmpSeed (uint32_t s)
{
  if (m_rt) { m_rt->SetEcmpSeed (s); }
  for (auto &kv : m_rtByVrf) { kv.second->SetEcmpSeed (s); }
}

void
SUESwitchNode::SetForwardingBudget (Time t)
{
  m_fwdBudget = t;
}

void
SUESwitchNode::SwitchReceiveFromDevice (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h)
{
  NS_LOG_FUNCTION (this << dev << p);
  ++m_rxPkts;
  uint32_t xpu = h.GetXpuId ();
  // §12.40 VRF: derived from the ingress port + frame fields (shim DSCP, then
  // VLAN vid, then port mapping — user-decided precedence DSCP > VLAN > port;
  // un-mapped = VRF0). The frame carries no VRF on the wire (Switch-ASIC-AFH
  // §6.6.4); the lookup is keyed by (xpuId, vrf) against the segmented table.
  uint32_t vrf = ComputeVrf (GetPortIdx (dev), h);
  // §6.9 ECMP entropy: MAC SA + MAC DA + shim FlowLabel (when F=1) — FNV-1a
  // over the 13 bytes. Per-flow affinity: frames of one flow stay on one
  // plane while different flows spread across the ECMP group (the legacy
  // XPU-id-only hash made the plane a function of the destination parity).
  uint8_t ent[13];
  h.GetSrc ().CopyTo (ent);
  h.GetDst ().CopyTo (ent + 6);
  ent[12] = (h.HasShim () && h.GetShim ().f) ? h.GetShim ().flowLabel : 0;
  uint64_t flowEntropy = 1469598103934665603ull;
  for (int i = 0; i < 13; ++i)
    {
      flowEntropy ^= ent[i];
      flowEntropy *= 1099511628211ull;
    }
  int32_t outPort = GetOutPort (xpu, vrf, flowEntropy);
  if (outPort < 0)
    {
      ++m_lookupMiss;
      ++m_vrfMiss[vrf];
      NS_LOG_LOGIC ("AFH lookup miss for xpu=0x" << std::hex << xpu
                    << " vrf=" << vrf << ", drop");
      // §12.30: under the deferred refund a drop must release the ingress
      // credit (R_VC_CC) like any frame the switch decides not to forward —
      // else the credit leaks and the sender window never reopens.
      if (dev->GetDeferCreditRefund ()) { dev->RefundCredit (ComputeVc (h), p->GetSize ()); }
      return;
    }

  // TTL (shim only): decrement; drop if reaches 0.
  if (h.HasShim ())
    {
      // Capture the full on-wire frame size (header included) BEFORE stripping
      // the header: the ingress charge (R_VC_CC, Receive) accounts the whole
      // MAC frame, so the deferred refund must release the same number of
      // PktCredits — otherwise a frame whose size straddles a credit boundary
      // leaks one credit per TTL drop and the sender window shrinks over time.
      uint32_t frameSize = p->GetSize ();
      p->RemoveHeader (h);
      h.DecrementTtl ();
      if (h.GetTtl () == 0)
        {
          ++m_ttlZeroDrop;
          NS_LOG_LOGIC ("TTL==0, drop");
          if (dev->GetDeferCreditRefund ()) { dev->RefundCredit (ComputeVc (h), frameSize); }
          return;
        }
      p->AddHeader (h);
    }

  uint8_t vc = ComputeVc (h);
  // vc_rx/vc_tx count frames that passed ingress (lookup hit + TTL>0) and are
  // being forwarded; ingress-dropped frames (lookup_miss / TTL==0) are NOT
  // counted here -- see m_lookupMiss / m_ttlZeroDrop. (DESIGN.md §6.11 semantics)
  if (vc < qCnt) { ++m_vcRx[vc]; }
  ++m_vrfRx[vrf];   // §12.40 per-segment forwarded count
  SendToDev (dev, (uint32_t) outPort, vc, p);
}

int32_t
SUESwitchNode::GetOutPort (uint32_t xpuId) const
{
  if (!m_rt) { return -1; }
  int32_t port = m_rt->GetOutPort (xpuId);
  if (port < 0 || (uint32_t) port >= m_ports.size ()) { return -1; }
  return port;
}

int32_t
SUESwitchNode::GetOutPort (uint32_t xpuId, uint64_t flowEntropy) const
{
  return GetOutPort (xpuId, 0, flowEntropy);
}

int32_t
SUESwitchNode::GetOutPort (uint32_t xpuId, uint32_t vrf, uint64_t flowEntropy) const
{
  // §12.40: the forwarding DB is segmented per VRF — pick the table programmed
  // for this VRF (VRF0 = the legacy table); a VRF without a table is a lookup
  // miss (never falls back to VRF0). Each table object is a plain LPM table.
  Ptr<SueRoutingTable> rt = (vrf == 0) ? m_rt : GetRoutingTable (vrf);
  if (!rt) { return -1; }
  int32_t port = rt->GetOutPort (xpuId, flowEntropy);
  if (port < 0 || (uint32_t) port >= m_ports.size ()) { return -1; }
  return port;
}

uint32_t
SUESwitchNode::GetPortIdx (Ptr<SueNetDevice> dev) const
{
  auto it = m_portIdxByDev.find (dev);
  return (it == m_portIdxByDev.end ()) ? 0 : it->second;
}

uint32_t
SUESwitchNode::ComputeVrf (uint32_t portIdx, const SueHeader &h) const
{
  // Switch-ASIC-AFH.md §6.6.4: VRF from Port / VLAN / DSCP sources.
  // User-decided precedence: DSCP > VLAN > port (§12.40); un-mapped = VRF0.
  // The shim DSCP is only available when the frame carries a parsed shim
  // (EtherType-configured, SueConfig::HasShim); VLAN only when the header
  // carries a VLAN tag.
  if (h.HasShim ())
    {
      uint32_t v = SueConfig::DscpToVrf (h.GetShim ().dscp);
      if (v != 0) { return v; }
    }
  if (h.HasVlan ())
    {
      uint32_t v = SueConfig::VlanToVrf (h.GetVid ());
      if (v != 0) { return v; }
    }
  return SueConfig::PortToVrf (portIdx);
}

void
SUESwitchNode::SendToDev (Ptr<SueNetDevice> ingress, uint32_t portIdx, uint8_t vc, Ptr<Packet> p)
{
  NS_LOG_FUNCTION (this << ingress << portIdx << (uint32_t) vc);
  ++m_txPkts;
  if (vc < qCnt) { ++m_vcTx[vc]; }
  Ptr<SueNetDevice> egress = m_ports[portIdx];
  if (m_fwdBudget > TimeStep (0))
    {
      // Forwarding latency + egress wire time <= budget. Wire is measured on
      // the size DequeueAndTransmit hands to TransmitStart (8B LLR preamble
      // prepended at dequeue; non-LLR egress overestimates by 8B, negligible
      // at link rate). Egress-queue congestion waiting is on top of the
      // budget (拥塞调度另算). The scheduled event inherits this node's LP
      // (MTP residency, §10.5) and admits-or-pends at fire time.
      Time wire = egress->GetDataRate ().CalculateBytesTxTime (p->GetSize () + 8);
      Time fwd = m_fwdBudget - wire;
      if (fwd > TimeStep (0))
        {
          Simulator::Schedule (fwd, &SUESwitchNode::Forward, this, ingress, portIdx, vc, p);
          return;
        }
    }
  Forward (ingress, portIdx, vc, p);
}

void
SUESwitchNode::Forward (Ptr<SueNetDevice> ingress, uint32_t portIdx, uint8_t vc, Ptr<Packet> p)
{
  NS_LOG_FUNCTION (this << portIdx << (uint32_t) vc);
  Ptr<SueNetDevice> egress = m_ports[portIdx];
  // §12.42: measure the frame BEFORE SwitchSend. On an idle egress, SwitchSend
  // runs a synchronous DequeueAndTransmit which prepends the 8B LLR preamble
  // IN PLACE onto this very packet object; measuring p->GetSize() afterwards
  // returns 2056B for a 2048B frame — straddling the 256B credit boundary
  // (9 credits instead of 8). The deferred refund would then release one
  // credit MORE than the ingress charged (OnRx measured the preamble-free
  // frame): the sender's S_VC_CF runs past S_VC_CC, CU = (CC-CF) mod 2^15
  // flips to ~2^15 and the credit window is permanently poisoned (first hit:
  // VRF 32-plane all-to-all stall at ~1% progress, §12.41). Same invariance
  // as the TTL-drop refund above: refund exactly the charged PktCredits.
  const uint32_t frameSize = p->GetSize ();
  // CBFC backpressure (§12.30): while the egress VC queue is at its depth
  // limit, hold the frame here (ingress credit stays consumed — the receiver
  // does NOT refund it) instead of enqueueing unboundedly.
  if (egress->GetDeferCreditRefund () && !egress->EgressCanAccept (vc, frameSize))
    {
      m_pending[portIdx][vc].push_back ({ingress, vc, p});
      ++m_pendPkts;
      if (++m_pendDepth > m_pendMax) { m_pendMax = m_pendDepth; }
      return;
    }
  egress->SwitchSend (vc, p);
  // Admitted: the ingress credit this frame consumed is now released (the
  // frame is owned by the egress queue; the wire slot is what gates further
  // accept from the egress side). Only a DEFERRED-refund ingress needs the
  // explicit release — a non-deferred ingress (e.g. a PFC port in a mixed
  // FC_MODE topology) already drained synchronously in Receive; refunding
  // again would double-decrement its buffer accounting (premature resume,
  // PFC losslessness broken). Refund the PRE-SwitchSend size (§12.42).
  if (ingress && ingress->GetDeferCreditRefund ()) { ingress->RefundCredit (vc, frameSize); }
}

void
SUESwitchNode::ServicePending (uint32_t portIdx)
{
  NS_LOG_FUNCTION (this << portIdx);
  if (portIdx >= m_pending.size ()) { return; }
  Ptr<SueNetDevice> egress = m_ports[portIdx];
  // Admit pending frames as egress slots free, FIFO within each VC (the egress
  // device's RR+credit gate orders across VCs). Runs on this node's LP (fired
  // from the egress device's freed-slot hook, which is armed on the same node).
  for (uint32_t vc = 0; vc < qCnt; ++vc)
    {
      auto &dq = m_pending[portIdx][vc];
      while (!dq.empty ())
        {
          PendingFrame f = dq.front ();
          // §12.42: measure before SwitchSend (see Forward — the freed hook
          // fires with the egress busy, so the synchronous-prepend hazard is
          // not live here today; the capture keeps both admission paths
          // identical so neither can regress independently).
          const uint32_t frameSize = f.p->GetSize ();
          if (!egress->EgressCanAccept (vc, frameSize)) { break; }
          dq.pop_front ();
          --m_pendDepth;
          egress->SwitchSend (vc, f.p);
          // Deferred-refund ingress only (see Forward — mixed FC_MODE ports).
          if (f.ingress && f.ingress->GetDeferCreditRefund ())
            { f.ingress->RefundCredit (vc, frameSize); }
        }
    }
}

void
SUESwitchNode::ApplyCbfcBackpressure (uint64_t limitBytes)
{
  NS_LOG_FUNCTION (this << limitBytes);
  for (uint32_t i = 0; i < m_ports.size (); ++i)
    {
      Ptr<SueNetDevice> port = m_ports[i];
      Ptr<SueFcController> fc = port->GetFcController ();
      if (!fc || fc->GetMode () != SueFcController::CBFC) { continue; }
      port->SetDeferCreditRefund (true);
      Ptr<SueCbfcEngine> cbfc = fc->GetCbfc ();
      for (uint32_t vc = 0; vc < qCnt; ++vc)
        {
          // 0 = auto: the per-VC egress buffer equals the receiver-side credit
          // window this port advertises (creditLimit × creditSize), so the
          // egress can never hold more than one full credit window per VC.
          uint64_t lim = (limitBytes == 0)
            ? (uint64_t) cbfc->GetSvcCreditLimit (vc) * (uint64_t) cbfc->GetCreditSize ()
            : limitBytes;
          port->SetEgressQueueVcLimit (vc, lim);
        }
      // Freed-slot hook -> ServicePending on this port. Called from the egress
      // device's DequeueAndTransmit (same LP), so ServicePending runs on this
      // node's LP (MTP residency, §10.5).
      port->SetEgressFreedHook ([this, i] () { ServicePending (i); });
    }
}

uint8_t
SUESwitchNode::ComputeVc (const SueHeader &h) const
{
  // DESIGN §7: table-driven, end-to-end consistent with the sender (SueLite-
  // Endpoint SendFrame) and the host receive path — all read the same SueConfig
  // map. Default table = direct map, so this is behavior-identical unless the
  // conf overrides DSCP_TO_VC_MAP / PCP_TO_VC_MAP / DEFAULT_VC.
  if (h.HasShim ()) { return SueConfig::DscpToVc (h.GetShim ().dscp); }
  if (h.HasVlan ()) { return SueConfig::PcpToVc (h.GetPcp (), h.GetDei ()); }
  return SueConfig::DefaultVc ();
}

// --- AFH counters ---
uint64_t SUESwitchNode::GetCntRxPkts () const { return m_rxPkts; }
uint64_t SUESwitchNode::GetCntTxPkts () const { return m_txPkts; }
uint64_t SUESwitchNode::GetCntLookupMiss () const { return m_lookupMiss; }
uint64_t SUESwitchNode::GetCntTtlZeroDrop () const { return m_ttlZeroDrop; }
uint64_t SUESwitchNode::GetCntVcRx (uint8_t vc) const { return vc < qCnt ? m_vcRx[vc] : 0; }
uint64_t SUESwitchNode::GetCntVcTx (uint8_t vc) const { return vc < qCnt ? m_vcTx[vc] : 0; }
uint64_t SUESwitchNode::GetCntPendPkts () const { return m_pendPkts; }
uint64_t SUESwitchNode::GetCntPendMax () const { return m_pendMax; }
uint64_t SUESwitchNode::GetCntVrfRx (uint32_t vrf) const
{
  auto it = m_vrfRx.find (vrf);
  return (it == m_vrfRx.end ()) ? 0 : it->second;
}
uint64_t SUESwitchNode::GetCntVrfMiss (uint32_t vrf) const
{
  auto it = m_vrfMiss.find (vrf);
  return (it == m_vrfMiss.end ()) ? 0 : it->second;
}
void SUESwitchNode::ResetCounters ()
{
  m_rxPkts = m_txPkts = m_lookupMiss = m_ttlZeroDrop = 0;
  for (uint32_t i = 0; i < qCnt; ++i) { m_vcRx[i] = m_vcTx[i] = 0; }
  m_vrfRx.clear ();
  m_vrfMiss.clear ();
}

} // namespace ns3

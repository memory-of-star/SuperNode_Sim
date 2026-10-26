/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SueLiteEndpoint implementation (DESIGN.md §6.12/§12.21/§12.22/§12.23).
 * See sue-lite-endpoint.h for the design contract (Object base, MTP-safe TX,
 * VC wire encoding, ENABLE_SCALEUP_WORKLOAD gate).
 */
#include "sue-lite-endpoint.h"
#include "sue-entry.h"
#include "sue-config.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-mtp.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/global-value.h"
#include "ns3/log.h"
#include <sstream>

NS_LOG_COMPONENT_DEFINE ("SueLiteEndpoint");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueLiteEndpoint);

TypeId
SueLiteEndpoint::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueLiteEndpoint")
    .SetParent<Object> ()
    .AddConstructor<SueLiteEndpoint> ()
  ;
  return tid;
}

SueLiteEndpoint::SueLiteEndpoint ()
  : m_etherType (SueConfig::EthType0 ())
{
}

// --- binding ---

void
SueLiteEndpoint::SetDevice (Ptr<SueNetDevice> dev)
{
  m_dev = dev;
  // A device never added to a node (isolated unit-test wiring) must not
  // segfault here — fail with a readable assertion instead.
  NS_ASSERT_MSG (dev != 0, "SueLiteEndpoint::SetDevice(null)");
  NS_ASSERT_MSG (dev->GetNode () != 0,
                 "SueLiteEndpoint::SetDevice: device not added to a node (GetNode() null)");
  m_nodeId = dev->GetNode ()->GetId ();
  // Install this endpoint's receive upcall (replaces HostDefaultRx). The
  // channel seam (§10.5) context-routes SueNetDevice::Receive onto the owning
  // node's LP, so Rx always runs LP-safe under MTP.
  dev->SetReceiveUpcall (MakeCallback (&SueLiteEndpoint::Rx, this));
}

void
SueLiteEndpoint::SetSourceXpu (uint32_t xpuId)
{
  m_srcXpu = xpuId;
}

// --- frame format ---

void
SueLiteEndpoint::SetDestinationXpu (uint32_t xpuId)
{
  m_dstXpu = xpuId;
  m_dstCycle.clear ();
  m_cycleIdx = 0;
}

void
SueLiteEndpoint::SetDestinationCycle (const std::vector<uint32_t> &xpUs)
{
  m_dstCycle = xpUs;
  m_dstXpu = 0xFFFFFFFF;
  m_cycleIdx = 0;
}

void
SueLiteEndpoint::SetPayloadSize (uint32_t bytes) { m_payloadSize = bytes; }

void
SueLiteEndpoint::SetVc (uint8_t vc)
{
  // Legacy convenience (§6.12): identical to SetDscp(vc) + SetPcp(vc). Under
  // the default direct maps this puts the frame on wire VC = vc.
  const uint8_t v = vc & 0x07;
  m_dscp = v;
  m_pcp = v;
  if (v != 0 && !m_useShim && !m_useVlan)
    {
      NS_LOG_WARN ("SueLiteEndpoint: VC " << (uint32_t) v
                   << " has no wire encoding (need shim.dscp or VLAN.PCP); frames will arrive as "
                   << (uint32_t) SueConfig::DefaultVc ());
    }
}

void
SueLiteEndpoint::SetDscp (uint8_t dscp)
{
  m_dscp = dscp & 0x3F;
  if (m_dscp != 0 && !m_useShim && !m_useVlan)
    {
      NS_LOG_WARN ("SueLiteEndpoint: DSCP " << (uint32_t) m_dscp
                   << " has no wire encoding (need shim.dscp or VLAN.PCP); frames will arrive as "
                   << (uint32_t) SueConfig::DefaultVc ());
    }
}

void
SueLiteEndpoint::SetDei (bool dei)
{
  m_dei = dei;
}

void
SueLiteEndpoint::SetPcp (uint8_t pcp)
{
  m_pcp = pcp & 0x07;
  if (m_pcp != 0 && !m_useShim && !m_useVlan)
    {
      NS_LOG_WARN ("SueLiteEndpoint: PCP " << (uint32_t) m_pcp
                   << " has no wire encoding (need shim.dscp or VLAN.PCP); frames will arrive as "
                   << (uint32_t) SueConfig::DefaultVc ());
    }
}

void
SueLiteEndpoint::SetUseShim (bool on)
{
  m_useShim = on;
  if (m_useShim && m_useVlan)
    {
      // ComputeVc prefers shim.dscp over vlan.pcp; warn so the caller knows
      // which VC encoding wins on the wire.
      NS_LOG_WARN ("SueLiteEndpoint: both shim and VLAN enabled; VC comes from shim.dscp (ComputeVc priority)");
    }
}

void
SueLiteEndpoint::SetUseVlan (bool on) { m_useVlan = on; }

void
SueLiteEndpoint::SetTtl (uint8_t ttl)
{
  if (ttl > 0x0F)
    {
      NS_LOG_WARN ("SueLiteEndpoint: TTL " << (uint32_t) ttl
                   << " exceeds the 4-bit shim field (max 15); clamping to 0 on the wire would make the switch drop at TTL==0");
      ttl = 0x0F;
    }
  m_ttl = ttl;
}

void
SueLiteEndpoint::SetEtherType (uint16_t et)
{
  m_etherType = et;
  m_etherTypeExplicit = true;
  if (m_useShim != SueConfig::HasShim (et))
    {
      NS_LOG_WARN ("SueLiteEndpoint: EtherType 0x" << std::hex << et << std::dec
                   << " HasShim=" << SueConfig::HasShim (et)
                   << " but useShim=" << m_useShim
                   << "; the receiver parses the shim per its own HasShim config and may mismatch");
    }
}

uint16_t
SueLiteEndpoint::ResolveEtherType () const
{
  if (m_etherTypeExplicit) { return m_etherType; }
  if (m_useShim)
    {
      // auto-pick a shim-bearing EtherType (default config: EthType1)
      return SueConfig::HasShim (SueConfig::EthType1 ())
             ? SueConfig::EthType1 () : SueConfig::EthType0 ();
    }
  // no shim: prefer EthType0 unless the current config makes it shim-bearing
  return SueConfig::HasShim (SueConfig::EthType0 ())
         ? SueConfig::EthType1 () : SueConfig::EthType0 ();
}

// --- scheduling ---

void
SueLiteEndpoint::SetInterval (Time interval) { m_interval = interval; }
void
SueLiteEndpoint::SetFrameCount (uint32_t n) { m_frameCount = n; }
void
SueLiteEndpoint::SetStartTime (Time t) { m_startTime = t; }
void
SueLiteEndpoint::SetDither (Time dither) { m_dither = dither; }

void
SueLiteEndpoint::Start ()
{
  SueMtpContractCheck ();   // MTP enabled after the cached lookup -> loud abort
  if (m_running || !m_dev) { return; }
  if (m_dstXpu == 0xFFFFFFFF && m_dstCycle.empty ())
    {
      NS_LOG_ERROR ("SueLiteEndpoint: Start() without a destination (SetDestinationXpu or SetDestinationCycle)");
      return;
    }
  // ENABLE_SCALEUP_WORKLOAD gate (DESIGN §6.12/§12.23): 1 = future external
  // astra-sim workload, not wired this round -> warn and fall back to the
  // built-in generator so configs that set it are not silently inert.
  UintegerValue v;
  GlobalValue::GetValueByName ("ENABLE_SCALEUP_WORKLOAD", v);
  if (v.Get () != 0)
    {
      NS_LOG_WARN ("SueLiteEndpoint: ENABLE_SCALEUP_WORKLOAD=1 (external astra-sim workload) is not wired yet; "
                   "falling back to the built-in self-simulated generator");
    }
  m_running = true;
  m_sent = 0;
  // §10.5 MTP: drive the FIRST frame from the owning node's LP via
  // ScheduleWithContext; Partition() re-homes it onto node m_nodeId before any
  // thread starts. The return value is deliberately not stored: under the
  // fork's MTP ScheduleWithContext is fire-and-forget (void), and Stop()'s
  // cancellation targets only the self-chained EventId (m_sendEvent).
  Simulator::ScheduleWithContext (m_nodeId, m_startTime + m_dither,
                                  &SueLiteEndpoint::SendFrame, this);
}

void
SueLiteEndpoint::Stop ()
{
  m_running = false;
  if (m_sendEvent.IsRunning ())
    {
      Simulator::Cancel (m_sendEvent);
      m_sendEvent = EventId ();
    }
}

void
SueLiteEndpoint::SendFrame ()
{
  // §10.5 residency invariant: every TX arm must run on the owning node's LP.
  // The first frame (ScheduleWithContext) and each self-chained plain Schedule
  // (which inherits the node LP) both satisfy this; the check aborts otherwise.
  SueMtpResidencyCheck (m_nodeId);
  if (!m_running || !m_dev) { return; }

  // Pick this frame's destination: cycle mode round-robins the list, else the
  // single configured destination.
  uint32_t dst = m_dstXpu;
  if (!m_dstCycle.empty ())
    {
      dst = m_dstCycle[m_cycleIdx % m_dstCycle.size ()];
      ++m_cycleIdx;
    }

  Ptr<Packet> p = Create<Packet> (m_payloadSize);
  SueHeader h;
  h.SetSrc (SueMacFromXpuId (m_srcXpu));
  h.SetDst (SueMacFromXpuId (dst));
  h.SetEtherType (ResolveEtherType ());
  // Local egress-queue VC must equal what every downstream decoder computes
  // from the wire header via the same SueConfig map (§7): shim.dscp > vlan.pcp
  // > DEFAULT_VC. The queue index is the MAP-derived VC, not the raw class.
  uint8_t qvc = SueConfig::DefaultVc ();
  if (m_useShim)
    {
      SueShimHeader sh;
      sh.ttl = m_ttl;
      sh.dscp = m_dscp;   // full 6-bit flow class (VC via DSCP_TO_VC_MAP)
      h.SetShim (sh);
      qvc = SueConfig::DscpToVc (m_dscp);
    }
  if (m_useVlan)
    {
      h.SetVlan (true, m_pcp, m_dei, 0);   // PCP flow class (VC via PCP_TO_VC_MAP)
      if (!m_useShim) { qvc = SueConfig::PcpToVc (m_pcp, m_dei); }
    }
  p->AddHeader (h);

  m_dev->SwitchSend (qvc, p);
  ++m_txCount;
  ++m_sent;

  // Self-chain the next frame. Plain Schedule inherits the current LP (the
  // node's), so the residency invariant holds and Cancel() from Stop() is
  // LP-safe. m_frameCount 0 = unlimited.
  if (m_frameCount == 0 || m_sent < m_frameCount)
    {
      m_sendEvent = Simulator::Schedule (m_interval, &SueLiteEndpoint::SendFrame, this);
    }
}

void
SueLiteEndpoint::Rx (Ptr<SueNetDevice>, Ptr<Packet> p, SueHeader h)
{
  ++m_rxCount;
  m_rxBytes += p->GetSize ();
  // VC attribution identical to SueNetDevice::ComputeVc (§7): table-driven
  // from the same SueConfig map the sender and switch use, so GetRxByVc
  // reflects the configured DSCP_TO_VC_MAP / PCP_TO_VC_MAP / DEFAULT_VC.
  uint8_t vc = h.HasShim () ? SueConfig::DscpToVc (h.GetShim ().dscp)
              : (h.HasVlan () ? SueConfig::PcpToVc (h.GetPcp (), h.GetDei ()) : SueConfig::DefaultVc ());
  ++m_rxByVc[vc];
  ++m_rxByDst[h.GetXpuId ()];       // MAC DA high 10 bits = addressed XPU
  ++m_rxBySrc[SueXpuFromMac (h.GetSrc ())];   // MAC SA high 10 bits = sender
}

// --- stats ---

uint64_t SueLiteEndpoint::GetTxCount () const { return m_txCount; }
uint64_t SueLiteEndpoint::GetRxCount () const { return m_rxCount; }
uint64_t SueLiteEndpoint::GetRxBytes () const { return m_rxBytes; }
uint64_t SueLiteEndpoint::GetRxByVc (uint8_t vc) const
{
  return (vc < 8) ? m_rxByVc[vc] : 0;
}
uint64_t SueLiteEndpoint::GetRxByDst (uint32_t xpuId) const
{
  auto it = m_rxByDst.find (xpuId);
  return (it != m_rxByDst.end ()) ? it->second : 0;
}
uint64_t SueLiteEndpoint::GetRxBySrc (uint32_t xpuId) const
{
  auto it = m_rxBySrc.find (xpuId);
  return (it != m_rxBySrc.end ()) ? it->second : 0;
}
const std::map<uint32_t, uint64_t> &
SueLiteEndpoint::GetRxBySrcMap () const
{
  return m_rxBySrc;
}

// --- AstraNetworkAPI-style reserved stubs (future workload path) ---

int64_t
SueLiteEndpoint::sim_send (uint32_t src, uint32_t dst, void *payload)
{
  NS_LOG_WARN ("SueLiteEndpoint::sim_send reserved (external workload not wired, §6.12): src="
               << src << " dst=" << dst);
  return -1;
}

int64_t
SueLiteEndpoint::sim_recv (uint32_t src, uint32_t dst, void *payload)
{
  NS_LOG_WARN ("SueLiteEndpoint::sim_recv reserved (external workload not wired, §6.12): src="
               << src << " dst=" << dst);
  return -1;
}

int64_t
SueLiteEndpoint::sim_schedule (uint32_t src, uint32_t dst, uint64_t cycle)
{
  NS_LOG_WARN ("SueLiteEndpoint::sim_schedule reserved (external workload not wired, §6.12): src="
               << src << " dst=" << dst << " cycle=" << cycle);
  return -1;
}

// --- install helper ---

std::vector<Ptr<SueLiteEndpoint>>
SueInstallLiteEndpoints (std::function<void (Ptr<SueLiteEndpoint>, uint32_t)> cfg)
{
  std::vector<Ptr<SueLiteEndpoint>> eps;
  for (uint32_t h = 0; h < SueGetNHosts (); ++h)
    {
      Ptr<SueLiteEndpoint> ep = CreateObject<SueLiteEndpoint> ();
      ep->SetSourceXpu (h);   // host index == XPU-id (§5.1/§6.8)
      ep->SetDevice (SueGetDevice (SueGetHostNodeId (h), 0));
      if (cfg) { cfg (ep, h); }
      eps.push_back (ep);
    }
  return eps;
}

} // namespace ns3

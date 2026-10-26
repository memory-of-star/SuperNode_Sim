/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-lldp-header.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE ("SueLldpHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueLldpHeader);

TypeId
SueLldpHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueLldpHeader")
    .SetParent<Header> ()
    .SetGroupName ("Sue")
    .AddConstructor<SueLldpHeader> ()
  ;
  return tid;
}

TypeId
SueLldpHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

uint32_t
SueLldpHeader::GetSerializedSize (void) const
{
  // llrSupported(1) + creditSize(4) + pktOvhd(4) + numVcs(1)
  // + rvcWant(1) + svcRts(1) + rvcRtr(1) + vcLimit[8](32)
  return 1 + 4 + 4 + 1 + 1 + 1 + 1 + 8 * 4;
}

void
SueLldpHeader::Serialize (Buffer::Iterator start) const
{
  start.WriteU8 (m_llrSupported);
  start.WriteHtonU32 (m_creditSize);
  start.WriteHtonU32 ((uint32_t) m_pktOvhd);
  start.WriteU8 (m_numVcs);
  start.WriteU8 (m_rvcWant);
  start.WriteU8 (m_svcRts);
  start.WriteU8 (m_rvcRtr);
  for (uint8_t v = 0; v < 8; ++v) { start.WriteHtonU32 (m_vcLimit[v]); }
}

uint32_t
SueLldpHeader::Deserialize (Buffer::Iterator start)
{
  m_llrSupported = start.ReadU8 ();
  m_creditSize = start.ReadNtohU32 ();
  m_pktOvhd = (int32_t) start.ReadNtohU32 ();
  m_numVcs = start.ReadU8 ();
  m_rvcWant = start.ReadU8 ();
  m_svcRts = start.ReadU8 ();
  m_rvcRtr = start.ReadU8 ();
  for (uint8_t v = 0; v < 8; ++v) { m_vcLimit[v] = start.ReadNtohU32 (); }
  return GetSerializedSize ();
}

void
SueLldpHeader::Print (std::ostream &os) const
{
  os << "llrSupported=" << (uint32_t) m_llrSupported << " creditSize=" << m_creditSize
     << " pktOvhd=" << m_pktOvhd << " numVcs=" << (uint32_t) m_numVcs
     << " rvcWant=0x" << std::hex << (uint32_t) m_rvcWant
     << " svcRts=0x" << (uint32_t) m_svcRts
     << " rvcRtr=0x" << (uint32_t) m_rvcRtr << std::dec;
}

void
SueLldpHeader::SetVcLimit (uint8_t vc, uint32_t limit)
{
  if (vc < 8) { m_vcLimit[vc] = limit; }
}

uint32_t
SueLldpHeader::GetVcLimit (uint8_t vc) const
{
  return vc < 8 ? m_vcLimit[vc] : 0;
}

// §5.2.8.4 initialization bitmap accessors (CBFC-SP-13).
#define SUE_LLDP_BITMAP_ACCESSORS(Name, member)                                                    \
  void SueLldpHeader::Set##Name (uint8_t vc, bool on)                                              \
  {                                                                                                \
    if (vc >= 8) { return; }                                                                       \
    if (on) { member |= (uint8_t) (1u << vc); }                                                    \
    else { member &= (uint8_t) ~(1u << vc); }                                                      \
  }                                                                                                \
  bool SueLldpHeader::Get##Name (uint8_t vc) const                                                 \
  {                                                                                                \
    return vc < 8 && (member & (1u << vc)) != 0;                                                   \
  }

SUE_LLDP_BITMAP_ACCESSORS (RvcWant, m_rvcWant)
SUE_LLDP_BITMAP_ACCESSORS (SvcRts, m_svcRts)
SUE_LLDP_BITMAP_ACCESSORS (RvcRtr, m_rvcRtr)

#undef SUE_LLDP_BITMAP_ACCESSORS

} // namespace ns3

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-cbfc-header.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueCbfcCtlOsHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueCbfcCtlOsHeader);

TypeId
SueCbfcCtlOsHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueCbfcCtlOsHeader")
    .SetParent<Header> ()
    .AddConstructor<SueCbfcCtlOsHeader> ()
  ;
  return tid;
}

TypeId SueCbfcCtlOsHeader::GetInstanceTypeId () const { return GetTypeId (); }

SueCbfcCtlOsHeader::SueCbfcCtlOsHeader () {}

void
SueCbfcCtlOsHeader::SetEntry (uint8_t idx, uint32_t vc, uint32_t count)
{
  if (idx == 0) { m_vc1 = vc & 0x1F; m_cf1 = count & 0x7FFF; }
  else { m_vc2 = vc & 0x1F; m_cf2 = count & 0x7FFF; }
}

void
SueCbfcCtlOsHeader::Serialize (Buffer::Iterator start) const
{
  start.WriteU8 (0x55); start.WriteU8 (0x55);   // D0-D1 reserved
  start.WriteU8 ((uint8_t) (((m_vc1 & 0x1F) << 3) | ((m_cf1 >> 12) & 0x07)));
  start.WriteU8 ((uint8_t) ((m_cf1 >> 4) & 0xFF));
  start.WriteU8 ((uint8_t) (((m_cf1 & 0x0F) << 4) | 0x05));
  start.WriteU8 ((uint8_t) (((m_vc2 & 0x1F) << 3) | ((m_cf2 >> 12) & 0x07)));
  start.WriteU8 ((uint8_t) ((m_cf2 >> 4) & 0xFF));
  start.WriteU8 ((uint8_t) (((m_cf2 & 0x0F) << 4) | 0x05));
}

uint32_t
SueCbfcCtlOsHeader::Deserialize (Buffer::Iterator start)
{
  start.ReadU8 (); start.ReadU8 ();   // D0-D1 reserved
  uint8_t d2 = start.ReadU8 ();
  uint8_t d3 = start.ReadU8 ();
  uint8_t d4 = start.ReadU8 ();
  m_vc1 = (d2 >> 3) & 0x1F;
  m_cf1 = ((uint32_t) (d2 & 0x07) << 12) | ((uint32_t) d3 << 4) | ((d4 >> 4) & 0x0F);
  uint8_t d5 = start.ReadU8 ();
  uint8_t d6 = start.ReadU8 ();
  uint8_t d7 = start.ReadU8 ();
  m_vc2 = (d5 >> 3) & 0x1F;
  m_cf2 = ((uint32_t) (d5 & 0x07) << 12) | ((uint32_t) d6 << 4) | ((d7 >> 4) & 0x0F);
  return 8;
}

void
SueCbfcCtlOsHeader::Print (std::ostream &os) const
{
  os << "cbfc-ctlos(vc1=" << m_vc1 << ",cf1=" << m_cf1 << ",vc2=" << m_vc2 << ",cf2=" << m_cf2 << ")";
}

} // namespace ns3

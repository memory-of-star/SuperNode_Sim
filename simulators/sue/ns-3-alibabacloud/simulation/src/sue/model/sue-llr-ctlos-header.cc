/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-llr-ctlos-header.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueLlrCtlOsHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueLlrCtlOsHeader);

TypeId
SueLlrCtlOsHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueLlrCtlOsHeader")
    .SetParent<Header> ()
    .AddConstructor<SueLlrCtlOsHeader> ()
  ;
  return tid;
}

TypeId SueLlrCtlOsHeader::GetInstanceTypeId () const { return GetTypeId (); }

SueLlrCtlOsHeader::SueLlrCtlOsHeader () {}

void SueLlrCtlOsHeader::SetAck (uint32_t seq) { m_subtype = 0x01; m_seq = seq & 0xFFFFF; }
void SueLlrCtlOsHeader::SetNack (uint32_t seq) { m_subtype = 0x02; m_seq = seq & 0xFFFFF; }
void SueLlrCtlOsHeader::SetInit (uint32_t initSeq, uint16_t initData)
{ m_subtype = 0x03; m_seq = initSeq & 0xFFFFF; m_initData = initData; }
void SueLlrCtlOsHeader::SetInitEcho (uint32_t initSeq, uint16_t initData)
{ m_subtype = 0x04; m_seq = initSeq & 0xFFFFF; m_initData = initData; }

void
SueLlrCtlOsHeader::Serialize (Buffer::Iterator start) const
{
  start.WriteU8 (m_subtype);                                   // D0
  start.WriteU8 ((uint8_t) (((m_seq >> 16) & 0x0F) << 4 | 0x07)); // D1: seq[19:16] | marker
  start.WriteU8 ((uint8_t) ((m_seq >> 8) & 0xFF));              // D2
  start.WriteU8 ((uint8_t) (m_seq & 0xFF));                     // D3
  start.WriteU8 ((uint8_t) ((m_initData >> 8) & 0xFF));         // D4
  start.WriteU8 ((uint8_t) (m_initData & 0xFF));                // D5
  start.WriteU8 (0x55);                                         // D6
  start.WriteU8 (0x55);                                         // D7
}

uint32_t
SueLlrCtlOsHeader::Deserialize (Buffer::Iterator start)
{
  m_subtype = start.ReadU8 ();
  uint8_t d1 = start.ReadU8 ();
  uint32_t hi = (d1 >> 4) & 0x0F;
  uint32_t mid = start.ReadU8 ();
  uint32_t lo = start.ReadU8 ();
  m_seq = (hi << 16) | (mid << 8) | lo;
  uint8_t d4 = start.ReadU8 ();
  uint8_t d5 = start.ReadU8 ();
  m_initData = ((uint16_t) d4 << 8) | d5;
  start.ReadU8 (); start.ReadU8 ();   // D6, D7 reserved
  return 8;
}

void
SueLlrCtlOsHeader::Print (std::ostream &os) const
{
  os << "llr-ctlos(sub=0x" << std::hex << (uint32_t) m_subtype << std::dec
     << ",seq=" << m_seq << ")";
}

} // namespace ns3

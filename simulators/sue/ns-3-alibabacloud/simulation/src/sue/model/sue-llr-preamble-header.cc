/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-llr-preamble-header.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueLlrPreambleHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueLlrPreambleHeader);

TypeId
SueLlrPreambleHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueLlrPreambleHeader")
    .SetParent<Header> ()
    .AddConstructor<SueLlrPreambleHeader> ()
  ;
  return tid;
}

TypeId
SueLlrPreambleHeader::GetInstanceTypeId () const { return GetTypeId (); }

void
SueLlrPreambleHeader::SetEligible (bool e, uint32_t seq)
{
  m_eligible = e;
  m_seq = seq & 0xFFFFF;   // 20-bit
}

void
SueLlrPreambleHeader::Serialize (Buffer::Iterator start) const
{
  start.WriteU8 (0xFB);                                         // B0
  if (m_eligible)
    {
      start.WriteU8 ((uint8_t) (((m_seq >> 16) & 0x0F) << 4 | 0x07));   // B1: seq[19:16] | 0x7
      start.WriteU8 ((uint8_t) ((m_seq >> 8) & 0xFF));                  // B2
      start.WriteU8 ((uint8_t) (m_seq & 0xFF));                         // B3
    }
  else
    {
      start.WriteU8 (0x55); start.WriteU8 (0x55); start.WriteU8 (0x55); // B1..B3 (ineligible)
    }
  start.WriteU8 (0x55); start.WriteU8 (0x55); start.WriteU8 (0x55);  // B4, B5, B6
  start.WriteU8 (0xD5);                                        // B7 (SFD)
}

uint32_t
SueLlrPreambleHeader::Deserialize (Buffer::Iterator start)
{
  start.ReadU8 ();   // B0 = 0xFB (not strictly checked; preamble marker)
  uint8_t b1 = start.ReadU8 ();
  if ((b1 & 0x0F) == 0x07)
    {
      m_eligible = true;
      uint32_t hi = (b1 >> 4) & 0x0F;
      uint32_t mid = start.ReadU8 ();
      uint32_t lo = start.ReadU8 ();
      m_seq = (hi << 16) | (mid << 8) | lo;
    }
  else
    {
      m_eligible = false;
      m_seq = 0;
      start.ReadU8 (); start.ReadU8 ();   // consume B2, B3
    }
  start.ReadU8 (); start.ReadU8 ();   // B4, B5
  start.ReadU8 ();                    // B6
  start.ReadU8 ();                    // B7 (SFD)
  return 8;
}

void
SueLlrPreambleHeader::Print (std::ostream &os) const
{
  os << "llr-preamble(" << (m_eligible ? "eligible" : "ineligible")
     << ",seq=" << m_seq << ")";
}

} // namespace ns3

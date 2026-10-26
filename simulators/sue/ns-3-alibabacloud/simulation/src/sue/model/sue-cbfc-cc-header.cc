/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-cbfc-cc-header.h"
#include "ns3/crc32.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueCbfcCcHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueCbfcCcHeader);

TypeId
SueCbfcCcHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueCbfcCcHeader")
    .SetParent<Header> ()
    .AddConstructor<SueCbfcCcHeader> ()
  ;
  return tid;
}

TypeId SueCbfcCcHeader::GetInstanceTypeId () const { return GetTypeId (); }

SueCbfcCcHeader::SueCbfcCcHeader () {}

void SueCbfcCcHeader::SetCc (uint32_t vc, uint32_t cc) { if (vc < NVC) { m_cc[vc] = cc & (CC_MOD - 1); } }
uint32_t SueCbfcCcHeader::GetCc (uint32_t vc) const { return vc < NVC ? m_cc[vc] : 0; }

void
SueCbfcCcHeader::Serialize (Buffer::Iterator start) const
{
  // Build the 60B Ethernet body in a local buffer so the CRC-32 FCS can be
  // computed over it (Buffer::Iterator is forward-only).
  uint8_t frame[60] = {0};

  // MACDA: slow-protocol multicast by default, configurable (§5.2.6.2)
  m_macDa.CopyTo (frame);
  // MACSA
  m_macSa.CopyTo (frame + 6);
  // EtherType = 0x8808 (MAC control)
  frame[12] = 0x88; frame[13] = 0x08;
  // Opcode = 0xFFFE (UEC CC_Update)
  frame[14] = 0xFF; frame[15] = 0xFE;
  // CID (24-bit, default FA-7A-CB, configurable §5.2.6.2)
  frame[16] = (uint8_t) ((m_cid >> 16) & 0xFF);
  frame[17] = (uint8_t) ((m_cid >> 8) & 0xFF);
  frame[18] = (uint8_t) (m_cid & 0xFF);
  // MsgType = 0x01 (VC[0:15])
  frame[19] = 0x01;

  // Pack 16 x 20-bit CC values contiguously, MSB-first, into frame[20..59]
  // (the CC field is 40B starting at byte 20 of the frame). Counter value vc
  // occupies bits [vc*20, vc*20+19]; bit 0 of the field is the MSB of byte 20
  // (Ethernet network byte order).
  uint32_t bit = 0;
  for (uint32_t v = 0; v < NVC; ++v)
    {
      uint32_t val = m_cc[v] & (CC_MOD - 1);
      for (int b = 19; b >= 0; --b)
        {
          if (val & (1u << b)) { frame[20 + (bit >> 3)] |= (uint8_t) (1u << (7 - (bit & 7))); }
          ++bit;
        }
    }

  start.Write (frame, 60);
  start.WriteHtonU32 (CRC32Calculate (frame, 60));
}

uint32_t
SueCbfcCcHeader::Deserialize (Buffer::Iterator start)
{
  uint8_t frame[60];
  start.Read (frame, 60);
  m_macDa.CopyFrom (frame);
  m_macSa.CopyFrom (frame + 6);
  m_cid = ((uint32_t) frame[16] << 16) | ((uint32_t) frame[17] << 8) | frame[18];
  // Unpack 40B of 20-bit CC values (MSB-first contiguous, field at byte 20).
  uint32_t bit = 0;
  for (uint32_t v = 0; v < NVC; ++v)
    {
      uint32_t val = 0;
      for (int b = 19; b >= 0; --b)
        {
          if (frame[20 + (bit >> 3)] & (1u << (7 - (bit & 7)))) { val |= (1u << b); }
          ++bit;
        }
      m_cc[v] = val;
    }
  start.Read (frame, 4);   // skip FCS (not validated)
  return FRAME_SIZE;
}

void
SueCbfcCcHeader::Print (std::ostream &os) const
{
  os << "cc-update(cc0=" << m_cc[0] << ")";
}

} // namespace ns3

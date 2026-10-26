/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-pfc-header.h"
#include "ns3/crc32.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SuePfcHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SuePfcHeader);

TypeId
SuePfcHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SuePfcHeader")
    .SetParent<Header> ()
    .AddConstructor<SuePfcHeader> ()
  ;
  return tid;
}

TypeId SuePfcHeader::GetInstanceTypeId () const { return GetTypeId (); }

SuePfcHeader::SuePfcHeader () {}

void
SuePfcHeader::SetQuanta (uint32_t vc, uint16_t quanta)
{
  if (vc < NVC)
    {
      m_pev |= (uint16_t) (1u << vc);   // mark this priority in the PEV
      m_quanta[vc] = quanta;
    }
}

uint16_t
SuePfcHeader::GetQuanta (uint32_t vc) const
{
  return vc < NVC ? m_quanta[vc] : 0;
}

void
SuePfcHeader::Serialize (Buffer::Iterator start) const
{
  // Build the 60B Ethernet body in a local buffer so the CRC-32 FCS can be
  // computed over it (Buffer::Iterator is forward-only).
  uint8_t frame[60] = {0};

  // MACDA: slow-protocol multicast 01-80-C2-00-00-01
  frame[0] = 0x01; frame[1] = 0x80; frame[2] = 0xC2;
  frame[3] = 0x00; frame[4] = 0x00; frame[5] = 0x01;
  // MACSA
  m_macSa.CopyTo (frame + 6);
  // EtherType = 0x8808 (MAC control)
  frame[12] = 0x88; frame[13] = 0x08;
  // Opcode = 0x0101 (PAUSE)
  frame[14] = 0x01; frame[15] = 0x01;
  // priority_enable_vector
  frame[16] = (uint8_t) ((m_pev >> 8) & 0xFF);
  frame[17] = (uint8_t) (m_pev & 0xFF);
  // 8 x pause_time_quanta
  for (uint32_t i = 0; i < NVC; ++i)
    {
      frame[18 + 2 * i]     = (uint8_t) ((m_quanta[i] >> 8) & 0xFF);
      frame[18 + 2 * i + 1] = (uint8_t) (m_quanta[i] & 0xFF);
    }
  // frame[34..59] stays 0x00 (pad to Ethernet minimum 60B)

  start.Write (frame, 60);
  start.WriteHtonU32 (CRC32Calculate (frame, 60));
}

uint32_t
SuePfcHeader::Deserialize (Buffer::Iterator start)
{
  uint8_t frame[60];
  start.Read (frame, 60);
  m_macSa.CopyFrom (frame + 6);
  m_pev = (uint16_t) (((uint16_t) frame[16] << 8) | frame[17]);
  for (uint32_t i = 0; i < NVC; ++i)
    {
      m_quanta[i] = (uint16_t) (((uint16_t) frame[18 + 2 * i] << 8) | frame[18 + 2 * i + 1]);
    }
  start.Read (frame, 4);   // skip FCS (not validated)
  return FRAME_SIZE;
}

void
SuePfcHeader::Print (std::ostream &os) const
{
  os << "pfc(pev=0x" << std::hex << m_pev << std::dec
     << " q0=" << m_quanta[0] << ")";
}

} // namespace ns3

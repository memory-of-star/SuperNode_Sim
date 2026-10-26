/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-ctl-header.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueCtlHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueCtlHeader);

TypeId
SueCtlHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueCtlHeader")
    .SetParent<Header> ()
    .AddConstructor<SueCtlHeader> ()
  ;
  return tid;
}

TypeId SueCtlHeader::GetInstanceTypeId () const { return GetTypeId (); }

SueCtlHeader::SueCtlHeader () {}
SueCtlHeader::SueCtlHeader (CtlType t) : m_type (t) {}

void
SueCtlHeader::Serialize (Buffer::Iterator start) const
{
  start.WriteU8 ((uint8_t) m_type);
}

uint32_t
SueCtlHeader::Deserialize (Buffer::Iterator start)
{
  m_type = (CtlType) start.ReadU8 ();
  return 1;
}

void
SueCtlHeader::Print (std::ostream &os) const
{
  os << "sue-ctl(type=0x" << std::hex << (uint32_t) m_type << std::dec << ")";
}

} // namespace ns3

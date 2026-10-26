/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-ctl-tag.h"
#include "ns3/type-id.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueCtlTag);

TypeId
SueCtlTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueCtlTag")
    .SetParent<Tag> ()
    .AddConstructor<SueCtlTag> ()
  ;
  return tid;
}

TypeId SueCtlTag::GetInstanceTypeId () const { return GetTypeId (); }
SueCtlTag::SueCtlTag () {}

void SueCtlTag::Serialize (TagBuffer buf) const { buf.WriteU8 (m_type); }
void SueCtlTag::Deserialize (TagBuffer buf) { m_type = buf.ReadU8 (); }
void SueCtlTag::Print (std::ostream &os) const { os << "CtlTag(" << (uint32_t) m_type << ")"; }

} // namespace ns3

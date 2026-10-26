/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-frame-bad-tag.h"
#include "ns3/type-id.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueFrameBadTag);

TypeId
SueFrameBadTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueFrameBadTag")
    .SetParent<Tag> ()
    .AddConstructor<SueFrameBadTag> ()
  ;
  return tid;
}

TypeId SueFrameBadTag::GetInstanceTypeId () const { return GetTypeId (); }

} // namespace ns3

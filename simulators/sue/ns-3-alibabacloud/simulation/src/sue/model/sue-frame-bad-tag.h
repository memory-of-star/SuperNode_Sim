/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE frame_bad tag (DESIGN.md §6.10 POISONED reporting). Marks a packet
 * whose FCS was POISONED (accepted by LLR but upper layer must know the
 * data is unreliable). Checked by the receive upcall via PeekPacketTag.
 */
#ifndef SUE_FRAME_BAD_TAG_H
#define SUE_FRAME_BAD_TAG_H

#include "ns3/tag.h"

namespace ns3 {

class SueFrameBadTag : public Tag
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return 0; }
  virtual void Serialize (TagBuffer buf) const { (void) buf; }
  virtual void Deserialize (TagBuffer buf) { (void) buf; }
  virtual void Print (std::ostream &os) const { os << "FrameBad"; }
};

} // namespace ns3

#endif /* SUE_FRAME_BAD_TAG_H */

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE control frame tag. Marks a Packet as a control frame (LLR CtlOS,
 * CBFC CF_Update, PFC pause) so the receiver can dispatch it to the
 * appropriate engine instead of the data path. Tag is metadata (not
 * serialized to wire). On-wire identification: CtlOS carry the 1B
 * SueCtlHeader; PFC/CC_Update are real Ethernet frames identified by
 * EtherType=0x8808 + Opcode (DESIGN §5.6/§5.10).
 */
#ifndef SUE_CTL_TAG_H
#define SUE_CTL_TAG_H

#include "ns3/tag.h"
#include "sue-ctl-header.h"
#include <cstdint>

namespace ns3 {

class SueCtlTag : public Tag
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return 1; }
  virtual void Serialize (TagBuffer buf) const;
  virtual void Deserialize (TagBuffer buf);
  virtual void Print (std::ostream &os) const;

  SueCtlTag ();
  void SetType (SueCtlHeader::CtlType t) { m_type = (uint8_t) t; }
  SueCtlHeader::CtlType GetType () const { return (SueCtlHeader::CtlType) m_type; }

private:
  uint8_t m_type{0};
};

} // namespace ns3

#endif /* SUE_CTL_TAG_H */

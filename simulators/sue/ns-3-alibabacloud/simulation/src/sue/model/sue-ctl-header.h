/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE control frame header (DESIGN.md §5.5/§5.7). The 1B on-wire type
 * discriminator for CtlOS short messages (LLR ACK/NACK/INIT/INIT_ECHO,
 * CBFC CF_Update): [SueCtlHeader 1B][8B payload]. PFC and CC_Update are
 * REAL Ethernet frames (DESIGN §5.6/§5.10) and carry no SueCtlHeader; they
 * identify themselves on the wire by EtherType=0x8808 + Opcode.
 *
 * RX dispatch for ALL control frames is via SueCtlTag (metadata, not wire
 * bytes); this enum's values are the tag's CtlType keys.
 *
 * Types:
 *   0x01 LLR_ACK      (8B CtlOS: seq[20b])
 *   0x02 LLR_NACK     (8B CtlOS: seq[20b])
 *   0x03 LLR_INIT     (8B CtlOS: seq[20b] + init_data[16b])
 *   0x04 LLR_INIT_ECHO(8B CtlOS: seq[20b] + init_data[16b])
 *   0x10 CF_Update    (8B CtlOS: 2x VC+count)
 *   0x11 CC_Update    (64B real Ethernet frame, §5.6; no SueCtlHeader)
 *   0x20 PFC_PAUSE    (64B real 802.1Qbb pause frame, §5.10; no SueCtlHeader)
 *   0x21 PFC_RESUME   (same; quanta=0 = resume)
 */
#ifndef SUE_CTL_HEADER_H
#define SUE_CTL_HEADER_H

#include "ns3/header.h"
#include <cstdint>

namespace ns3 {

class SueCtlHeader : public Header
{
public:
  enum CtlType : uint8_t {
    LLR_ACK = 0x01, LLR_NACK = 0x02, LLR_INIT = 0x03, LLR_INIT_ECHO = 0x04,
    CF_UPDATE = 0x10, CC_UPDATE = 0x11,
    PFC_PAUSE = 0x20, PFC_RESUME = 0x21,
    LLDP = 0x30,   // UE link negotiation (UEC §5.3, NEG-SP-01/02)
  };

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return 1; }
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  SueCtlHeader ();
  SueCtlHeader (CtlType t);

  CtlType GetType () const { return m_type; }
  void SetType (CtlType t) { m_type = t; }

private:
  CtlType m_type{LLR_ACK};
};

} // namespace ns3

#endif /* SUE_CTL_HEADER_H */

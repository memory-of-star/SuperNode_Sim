/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE CBFC CC_Update frame (DESIGN.md §5.6, UEC §5.2.6.2). Real 64B
 * Ethernet frame carried as a single Packet (no SueCtlHeader). Dispatch
 * to the CBFC engine is via SueCtlTag metadata; on-wire identification is
 * EtherType=0x8808 + Opcode=0xFFFE.
 *
 * Wire (64B): MACDA=01-80-C2-00-00-01 | MACSA | EtherType=0x8808
 *   | Opcode=0xFFFE | CID=FA-7A-CB | MsgType(0x01=VC[0:15])
 *   | 16 x CC[20b] (40B, MSB-first contiguous bit packing) | 4B CRC-32 FCS.
 * This generation uses 8 VCs; VC[8:15] are sent as 0. CC counters are
 * 20-bit (mod CC_MOD); SetCc masks to that width.
 */
#ifndef SUE_CBFC_CC_HEADER_H
#define SUE_CBFC_CC_HEADER_H

#include "ns3/header.h"
#include "ns3/mac48-address.h"
#include <array>
#include <cstdint>

namespace ns3 {

class SueCbfcCcHeader : public Header
{
public:
  static const uint32_t NVC = 16;
  static const uint32_t CC_MOD = 1u << 20;   // 20-bit CC counters
  static const uint32_t FRAME_SIZE = 64;     // on-wire size

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return FRAME_SIZE; }
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  SueCbfcCcHeader ();

  void SetMacSa (Mac48Address sa) { m_macSa = sa; }
  Mac48Address GetMacSa () const { return m_macSa; }
  // MACDA/CID configurable (UEC §5.2.6.2 "MACDA/MACSA/CID 建议可配");
  // defaults below are the spec values so the wire is byte-identical
  // unless explicitly reconfigured.
  void SetMacDa (Mac48Address da) { m_macDa = da; }
  Mac48Address GetMacDa () const { return m_macDa; }
  void SetCid (uint32_t cid) { m_cid = cid & 0xFFFFFF; }
  uint32_t GetCid () const { return m_cid; }

  void SetCc (uint32_t vc, uint32_t cc);
  uint32_t GetCc (uint32_t vc) const;

private:
  Mac48Address m_macSa;
  Mac48Address m_macDa{"01:80:c2:00:00:01"};
  uint32_t m_cid{0xFA7ACB};
  std::array<uint32_t, NVC> m_cc{};
};

} // namespace ns3

#endif /* SUE_CBFC_CC_HEADER_H */

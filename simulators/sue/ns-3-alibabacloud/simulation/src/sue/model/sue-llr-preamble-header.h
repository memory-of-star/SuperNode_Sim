/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE LLR preamble header (DESIGN.md §5.4, UEC §5.1.1.1). 8B, carries a
 * 20-bit sequence number for LLR-eligible frames. Prepended by SueNetDevice
 * TX; stripped by RX before OnRx (so CBFC credit bytes = MAC frame).
 *
 *   B0=0xFB
 *   B1 = eligible:   [seq[19:16] (high nibble) | 0x07 (low nibble marker)]
 *       ineligible: 0x55
 *   B2=seq[15:8]  B3=seq[7:0]  B4..B6=0x55  B7=0xD5 (SFD)
 */
#ifndef SUE_LLR_PREAMBLE_HEADER_H
#define SUE_LLR_PREAMBLE_HEADER_H

#include "ns3/header.h"
#include <cstdint>

namespace ns3 {

class SueLlrPreambleHeader : public Header
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return 8; }
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  void SetEligible (bool e, uint32_t seq);
  bool IsEligible () const { return m_eligible; }
  uint32_t GetSeq () const { return m_seq; }   // 0..2^20-1

private:
  bool m_eligible{false};
  uint32_t m_seq{0};   // 20-bit
};

} // namespace ns3

#endif /* SUE_LLR_PREAMBLE_HEADER_H */

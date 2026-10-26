/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE CBFC CF_Update CtlOS payload (UEC §5.2.6.1, 8B). Carried after
 * SueCtlHeader. Carries two (VC_index, CF_count) pairs per CtlOS.
 *
 * Wire (after SueCtlHeader 1B):
 *   D0-D1                        = reserved (0x55)
 *   D2[7:3]=CF1_VC(5b) | D2[2:0]=CF1_count[14:12]
 *   D3                          = CF1_count[11:4]
 *   D4[7:4]=CF1_count[3:0] | D4[3:0]=0x55 (was O-code in UEC)
 *   D5[7:3]=CF2_VC(5b) | D5[2:0]=CF2_count[14:12]
 *   D6                          = CF2_count[11:4]
 *   D7[7:4]=CF2_count[3:0] | D7[3:0]=0x55
 *
 * 15-bit CF count, matching the engine's CF_MOD = 2^15 (sue-cbfc-engine.h).
 * An 11b wire would silently truncate counts >= 2048; 5b+15b = 20b per VC
 * fits the 8B payload with room to spare.
 */
#ifndef SUE_CBFC_HEADER_H
#define SUE_CBFC_HEADER_H

#include "ns3/header.h"
#include <cstdint>

namespace ns3 {

class SueCbfcCtlOsHeader : public Header
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return 8; }
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  SueCbfcCtlOsHeader ();
  /** Set one CF_Update entry (vc + cf count). The other entry stays 0. */
  void SetEntry (uint8_t idx, uint32_t vc, uint32_t count);

  uint32_t GetVc (uint8_t idx) const { return idx == 0 ? m_vc1 : m_vc2; }
  uint32_t GetCount (uint8_t idx) const { return idx == 0 ? m_cf1 : m_cf2; }

private:
  uint32_t m_vc1{0}, m_cf1{0};
  uint32_t m_vc2{0}, m_cf2{0};
};

} // namespace ns3

#endif /* SUE_CBFC_HEADER_H */

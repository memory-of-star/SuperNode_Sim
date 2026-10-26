/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE PFC pause frame (DESIGN.md §5.10, IEEE 802.1Qbb). Real Ethernet
 * frame carried as a single Packet (no SueCtlHeader). Dispatch to the
 * PFC engine is via SueCtlTag metadata; on-wire identification is the
 * EtherType=0x8808 + Opcode=0x0101 (MAC control).
 *
 * Wire (64B): MACDA=01-80-C2-00-00-01 | MACSA | EtherType=0x8808
 *   | Opcode=0x0101 | 2B priority_enable_vector | 2B x 8 pause_time_quanta
 *   | 26B pad (to 60B Ethernet minimum) | 4B CRC-32 FCS.
 * 802.1Qbb semantic: quanta>0 = pause that priority; quanta=0 = resume.
 * FCS is present for wire fidelity; it is not validated on RX (the error
 * model is independent).
 */
#ifndef SUE_PFC_HEADER_H
#define SUE_PFC_HEADER_H

#include "ns3/header.h"
#include "ns3/mac48-address.h"
#include <cstdint>

namespace ns3 {

class SuePfcHeader : public Header
{
public:
  static const uint32_t NVC = 8;              // 8 priorities == 8 VCs
  static const uint32_t FRAME_SIZE = 64;      // on-wire size (incl pad + FCS)

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return FRAME_SIZE; }
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  SuePfcHeader ();

  void SetMacSa (Mac48Address sa) { m_macSa = sa; }
  Mac48Address GetMacSa () const { return m_macSa; }

  /** Mark priority vc in the frame (sets its PEV bit) and write its quanta.
   *  quanta==0 encodes "resume" per 802.1Qbb. */
  void SetQuanta (uint32_t vc, uint16_t quanta);
  uint16_t GetQuanta (uint32_t vc) const;
  uint16_t GetPev () const { return m_pev; }

private:
  Mac48Address m_macSa;
  uint16_t m_pev{0};
  uint16_t m_quanta[NVC]{};
};

} // namespace ns3

#endif /* SUE_PFC_HEADER_H */

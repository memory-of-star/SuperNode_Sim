/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Minimal UE link-negotiation frame (UEC §5.3, NEG-SP-01/02).
 *
 * Carries the SEMANTIC fields of the Options TLV (LLR capability), the CBFC
 * TLV (credit size / per-VC limits) and the §5.2.8.4 lossless-VC
 * initialization bitmaps (R_VC_Want / S_VC_RTS / R_VC_RTR) — the exact UEC
 * LLDPDU byte layout (TLV type/length/CID/subtype/version, nearest-bridge
 * group MAC, YANG database semantics) is management-plane material and is NOT
 * reproduced (§5.3.2 deviation, documented). One frame per link endpoint,
 * sent once at link start; the peer applies the negotiated values before the
 * LLR INIT handshake (sue-net-device.cc negotiation path). With
 * LLDP_NEGOTIATE=1 the exchange runs the multi-round RTS/RTR handshake
 * (§5.2.8.4 minimal model, CBFC-SP-13): each round carries all three bitmaps.
 */
#ifndef SUE_LLDP_HEADER_H
#define SUE_LLDP_HEADER_H

#include "ns3/header.h"
#include <cstdint>

namespace ns3 {

class SueLldpHeader : public Header
{
public:
  static TypeId GetTypeId (void);
  TypeId GetInstanceTypeId (void) const override;
  uint32_t GetSerializedSize (void) const override;
  void Serialize (Buffer::Iterator start) const override;
  uint32_t Deserialize (Buffer::Iterator start) override;
  void Print (std::ostream &os) const override;

  // Options TLV semantics: does this endpoint support LLR?
  void SetLlrSupported (bool on) { m_llrSupported = on ? 1 : 0; }
  bool GetLlrSupported () const { return m_llrSupported != 0; }

  // CBFC TLV semantics (receiver-side advertisement):
  void SetCreditSize (uint32_t bytes) { m_creditSize = bytes; }
  uint32_t GetCreditSize () const { return m_creditSize; }
  void SetPktOvhd (int32_t bytes) { m_pktOvhd = bytes; }
  int32_t GetPktOvhd () const { return m_pktOvhd; }
  void SetNumVcs (uint8_t n) { m_numVcs = n; }
  uint8_t GetNumVcs () const { return m_numVcs; }
  /** Per-VC credit limit advertised by the receiver (0 = VC not offered). */
  void SetVcLimit (uint8_t vc, uint32_t limit);
  uint32_t GetVcLimit (uint8_t vc) const;

  // §5.2.8.4 lossless-VC initialization bitmaps (CBFC-SP-13). One bit per VC
  // (NVC=8): R_VC_Want (receiver wants this VC initialized), S_VC_RTS (sender
  // ready to send), R_VC_RTR (receiver ready to receive). Full bitmap access
  // is what the device-side handshake state machine needs.
  void SetRvcWant (uint8_t vc, bool on);
  bool GetRvcWant (uint8_t vc) const;
  uint8_t GetRvcWantBitmap () const { return m_rvcWant; }
  void SetSvcRts (uint8_t vc, bool on);
  bool GetSvcRts (uint8_t vc) const;
  uint8_t GetSvcRtsBitmap () const { return m_svcRts; }
  void SetRvcRtr (uint8_t vc, bool on);
  bool GetRvcRtr (uint8_t vc) const;
  uint8_t GetRvcRtrBitmap () const { return m_rvcRtr; }

private:
  uint8_t m_llrSupported{0};
  uint32_t m_creditSize{0};
  int32_t m_pktOvhd{0};
  uint8_t m_numVcs{0};
  uint8_t m_rvcWant{0};
  uint8_t m_svcRts{0};
  uint8_t m_rvcRtr{0};
  uint32_t m_vcLimit[8]{};
};

} // namespace ns3

#endif /* SUE_LLDP_HEADER_H */

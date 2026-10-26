/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE LLR CtlOS payload header (UEC §5.1.3, 8B). Carried after SueCtlHeader.
 * ACK/NACK: 20-bit seq. INIT/INIT_ECHO: 20-bit init_seq + 16-bit init_data.
 *
 * Wire (after SueCtlHeader 1B):
 *   D0 = msg_subtype (0x01 ACK, 0x02 NACK, 0x03 INIT, 0x04 INIT_ECHO)
 *   D1-D3 = seq[19:0] (D1[7:4]=seq[19:16], D1[3:0]=0x7 marker for eligible
 *           CtlOS, D2=seq[15:8], D3=seq[7:0])
 *   D4-D5 = init_data (only INIT/INIT_ECHO)
 *   D6-D7 = reserved (0x55)
 * Total CtlOS payload: 8B (matches UEC §5.1.3 xMII format, simplified).
 */
#ifndef SUE_LLR_CTLOS_HEADER_H
#define SUE_LLR_CTLOS_HEADER_H

#include "ns3/header.h"
#include <cstdint>

namespace ns3 {

class SueLlrCtlOsHeader : public Header
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId () const;
  virtual uint32_t GetSerializedSize () const { return 8; }
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  SueLlrCtlOsHeader ();
  void SetAck (uint32_t seq);
  void SetNack (uint32_t seq);
  void SetInit (uint32_t initSeq, uint16_t initData);
  void SetInitEcho (uint32_t initSeq, uint16_t initData);

  uint8_t GetSubtype () const { return m_subtype; }
  uint32_t GetSeq () const { return m_seq; }
  uint16_t GetInitData () const { return m_initData; }

private:
  uint8_t m_subtype{0x01};   // ACK default
  uint32_t m_seq{0};         // 20-bit
  uint16_t m_initData{0};
};

} // namespace ns3

#endif /* SUE_LLR_CTLOS_HEADER_H */

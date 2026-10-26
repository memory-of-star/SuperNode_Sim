/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE AFH Gen1 L2 frame header (DESIGN.md §5.1/§5.2/§5.3/§5.8).
 *
 * Wire layout (no preamble; preamble is a separate LLR header in S3):
 *   [MAC DA 6][MAC SA 6] [VLAN 4 if present: TPID 0x8100 | PCP/DEI/VID]
 *   [EtherType 2] [shim 2B/3B if the frame's EtherType is configured with a
 *    shim (per-etype *HAS_SHIM, §5.2/§9)] payload...
 *
 * Four combinations (§5.8): ±VLAN × ±shim. The two EtherTypes are SYMMETRIC SUE
 * protocol identifiers (defaults 0xFCF1/0xFCF2); shim presence is a per-etype
 * config decision, not a role of the EtherType value (chip model, §5.2).
 *
 * XPU-id: high 10 bits of MAC DA (bits 47..38), non-standard MAC addressing
 * (no I/G, U/L bit semantics; §5.1). 2^10 = 1024 >= 512-card target.
 */
#ifndef SUE_HEADER_H
#define SUE_HEADER_H

#include "ns3/header.h"
#include "ns3/mac48-address.h"
#include "sue-shim-header.h"
#include <vector>

namespace ns3 {

// Configurable EtherTypes (DESIGN.md §5.2). Two symmetric SUE protocol
// identifiers; shim presence is the per-etype SUE_ETH_TYPE_{0,1}_HAS_SHIM
// config (SueConfig::HasShim, §9), not a property of the value. Defaults are
// private values. At runtime SueConfig::EthType0()/EthType1()/HasShim()
// (GlobalValues, §9) override these; they remain the compile-time fallback for
// headers constructed before the config layer is registered.
static const uint16_t SUE_ETH_TYPE_0  = 0xFCF1;
static const uint16_t SUE_ETH_TYPE_1  = 0xFCF2;
static const uint16_t SUE_VLAN_TPID   = 0x8100;

class SueHeader : public Header
{
public:
  /** Default EtherType reads the runtime GlobalValue (§9); falls back to
   *  SUE_ETH_TYPE_0 if the config layer is unregistered. */
  SueHeader ();
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual void Print (std::ostream &os) const;

  // --- construction ---
  void SetDst (Mac48Address d);
  void SetSrc (Mac48Address s);
  void SetVlan (bool on, uint8_t pcp = 0, uint8_t dei = 0, uint16_t vid = 0);
  /** Set EtherType (SUE_ETH_TYPE_0 or SUE_ETH_TYPE_1). */
  void SetEtherType (uint16_t et);
  /** Attach shim (shimPresent=true). Does NOT touch the EtherType: the caller
   *  must pick an EtherType configured with a shim (SueConfig::HasShim, §9) so
   *  the receiver parses the shim. */
  void SetShim (const SueShimHeader &sh);
  /** Detach shim (shimPresent=false). EtherType unchanged; caller must use an
   *  EtherType configured without a shim for the round-trip to be consistent. */
  void ClearShim ();

  // --- accessors ---
  Mac48Address GetDst () const;
  Mac48Address GetSrc () const;
  bool HasVlan () const;
  uint8_t GetPcp () const;
  uint8_t GetDei () const;
  uint16_t GetVid () const;
  uint16_t GetEtherType () const;
  bool HasShim () const;
  const SueShimHeader &GetShim () const;
  SueShimHeader &GetShimRef ();

  /** High 10 bits of MAC DA -> XPU-id (0..1023). */
  uint32_t GetXpuId () const;
  /** Shim TTL (0 if no shim). */
  uint8_t GetTtl () const;
  /** Decrement shim TTL (no-op if no shim). */
  void DecrementTtl ();

private:
  Mac48Address m_dst;
  Mac48Address m_src;
  bool m_vlan{false};
  uint8_t m_pcp{0};
  uint8_t m_dei{0};
  uint16_t m_vid{0};
  uint16_t m_etherType{SUE_ETH_TYPE_0};
  bool m_shimPresent{false};
  SueShimHeader m_shim;
};

/** Encode an XPU-id into the high 10 bits of a MAC address (bits 47..38,
 *  §5.1) — the exact inverse of SueHeader::GetXpuId (and of the rx-side
 *  decode at sue-header.cc:153). One shared definition for all senders
 *  (endpoint / scratch / tests) instead of the historical per-file static
 *  copies; each file's static version is left untouched (DESIGN §12.22). */
Mac48Address SueMacFromXpuId (uint32_t xpuId);
/** Decode the high 10 bits of a MAC address (bits 47..38) into an XPU-id —
 *  the same decode SueHeader::GetXpuId applies to MAC DA, here for an
 *  arbitrary MAC (e.g. MAC SA, for rx-by-source attribution). */
uint32_t SueXpuFromMac (Mac48Address m);

} // namespace ns3

#endif /* SUE_HEADER_H */

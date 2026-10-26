/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE AFH Gen1 shim header (DESIGN.md §5.3).
 * 2B (F=0, no Flow Label) or 3B (F=1, +Flow Label). F is bit 1 (the "2nd bit").
 * Plain struct with Pack/Unpack to a byte buffer (no ns-3 Header overhead);
 * embedded by SueHeader.
 */
#ifndef SUE_SHIM_HEADER_H
#define SUE_SHIM_HEADER_H

#include <cstdint>

namespace ns3 {

struct SueShimHeader
{
  bool ar{false};        // bit0: adaptive-routing eligibility (reserved this phase)
  bool f{false};         // bit1: length selector (0=2B, 1=3B / Flow Label present)
  bool c{false};         // bit2: CNM generation eligible (parsed, no action)
  bool cnm{false};       // bit3: this frame is a CNM (parsed, no action)
  uint8_t ttl{0};        // bits4-7 (4 bits, 0-15)
  uint8_t dscp{0};       // bits8-13 (6 bits, 0-63) -- VC mapping source
  uint8_t ecn{0};        // bits14-15 (2 bits, 0-3) (parsed, no action)
  uint8_t flowLabel{0};  // bits16-23 (8 bits) -- present iff f==true

  /** Wire size in bytes: 2 if !f, 3 if f. */
  uint8_t Size () const { return f ? 3 : 2; }

  /** Write Size() bytes to buf (caller ensures >= Size()). */
  void Pack (uint8_t* buf) const
  {
    buf[0] = (uint8_t) ((ar ? 1 : 0) << 7 | (f ? 1 : 0) << 6 | (c ? 1 : 0) << 5 |
                        (cnm ? 1 : 0) << 4 | (ttl & 0x0F));
    buf[1] = (uint8_t) (((dscp & 0x3F) << 2) | (ecn & 0x03));
    if (f) { buf[2] = flowLabel; }
  }

  /** Read from buf; infers 2B/3B from F bit. Reads exactly Size() bytes. */
  void Unpack (const uint8_t* buf)
  {
    ar   = (buf[0] >> 7) & 0x01;
    f    = (buf[0] >> 6) & 0x01;
    c    = (buf[0] >> 5) & 0x01;
    cnm  = (buf[0] >> 4) & 0x01;
    ttl  = buf[0] & 0x0F;
    dscp = (buf[1] >> 2) & 0x3F;
    ecn  = buf[1] & 0x03;
    flowLabel = f ? buf[2] : 0;
  }
};

} // namespace ns3

#endif /* SUE_SHIM_HEADER_H */

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE routing table (DESIGN.md §6.9).
 * LPM keyed by XPU-id (high 10 bits of MAC DA). ECMP groups supported,
 * deterministic hash pick (no adaptive routing).
 */
#ifndef SUE_ROUTING_H
#define SUE_ROUTING_H

#include "ns3/object.h"
#include <cstdint>
#include <vector>

namespace ns3 {

class SueRoutingTable : public Object
{
public:
  static TypeId GetTypeId (void);

  /**
   * Add a route: traffic to XPU-ids whose top `prefixLen` bits equal the top
   * `prefixLen` bits of `xpuId` goes to `ecmpPorts` (1 entry = single path,
   * >1 = ECMP group). prefixLen in [0..10]; 0 = default route.
   */
  void AddPrefix (uint32_t xpuId, uint8_t prefixLen, std::vector<uint32_t> ecmpPorts);

  /** Longest-prefix match. Returns true and fills `ports` on hit. */
  bool Lookup (uint32_t xpuId, std::vector<uint32_t> &ports) const;

  /** Convenience: return the chosen egress port (deterministic ECMP pick) or -1.
   *  Legacy entropy (XPU-id only); bit-identical to the original formula. */
  int32_t GetOutPort (uint32_t xpuId) const;
  /** ECMP pick with flow entropy (DESIGN §6.9): `flowEntropy` should mix MAC
   *  SA + MAC DA + shim FlowLabel (the switch builds it via FNV-1a, see
   *  SUESwitchNode::SwitchReceiveFromDevice). Deterministic; 0 = legacy pick. */
  int32_t GetOutPort (uint32_t xpuId, uint64_t flowEntropy) const;

  void Clear ();
  void SetEcmpSeed (uint32_t s);

private:
  struct Entry
  {
    uint32_t prefix;             // top `len` bits of xpuId
    uint8_t len;                 // 0..10
    std::vector<uint32_t> ports; // 1+ next-hop ports
  };
  std::vector<Entry> m_entries;  // kept sorted by len descending
  uint32_t m_seed{0};
};

} // namespace ns3

#endif /* SUE_ROUTING_H */

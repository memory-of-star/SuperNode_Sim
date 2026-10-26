/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE Scale-Up Ethernet - SUESwitchNode (S1: AFH Gen1 forwarding + AFH counters).
 * Mirrors NVSwitchNode structure (independent). Forwards by XPU-id LPM,
 * decrements shim TTL, maps VC (DSCP/PCP), sends to egress port. No AR.
 * AFH counters (§6.11): rx/tx pkts, lookup_miss, ttl_zero_drop, per-VC rx/tx.
 * See DESIGN.md §6.6/§6.9/§6.11.
 */
#ifndef SUE_SWITCH_NODE_H
#define SUE_SWITCH_NODE_H

#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/callback.h"
#include "sue-header.h"
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace ns3 {

class SueNetDevice;
class SueRoutingTable;
class Packet;

class SUESwitchNode : public Node
{
public:
  static const uint32_t qCnt = 8;       // virtual channels
  static const uint32_t pCnt = 256;     // port radix (256x200G single-switch fabric, §12.41)

  static TypeId GetTypeId (void);
  SUESwitchNode ();
  virtual ~SUESwitchNode ();

  /** Add a SueNetDevice port (aggregates to this node, sets its receive upcall). */
  uint32_t AddPort (Ptr<SueNetDevice> dev);
  Ptr<SueNetDevice> GetPort (uint32_t i) const;
  uint32_t GetNPorts () const;

  /** Routing table (LPM by XPU-id). VRF0 shorthand — same as the 2-arg form
   *  with vrf=0 (§12.40). */
  void SetRoutingTable (Ptr<SueRoutingTable> rt);
  Ptr<SueRoutingTable> GetRoutingTable () const;
  /** VRF-segmented table (§12.40): lookups for frames whose VRF==vrf use this
   *  table. vrf=0 aliases the legacy table; a VRF without a table = lookup
   *  miss (segmented database, never falls back to VRF0). */
  void SetRoutingTable (uint32_t vrf, Ptr<SueRoutingTable> rt);
  Ptr<SueRoutingTable> GetRoutingTable (uint32_t vrf) const;
  void SetEcmpSeed (uint32_t s);

  /** Per-hop forwarding budget: forwarding latency + egress wire time <=
   *  budget; egress-queue congestion waiting is on top. 0 (default) = forward
   *  immediately (baseline). Set before traffic starts (SetupSueNetwork). */
  void SetForwardingBudget (Time t);

  /** CBFC egress→ingress backpressure (§6.7/§12.30): for every CBFC-mode port,
   *  defer the ingress credit refund (RefundCredit is called only once the
   *  switch admits the frame onto an egress port), bound the egress queue per
   *  VC (0 = auto = creditLimit × creditSize; >0 = explicit per-VC bytes), and
   *  hook each egress port's freed-slot event to ServicePending. Host devices
   *  and non-CBFC ports are untouched. */
  void ApplyCbfcBackpressure (uint64_t limitBytes);
  /** Admit pending frames (held while the egress queue was at its per-VC
   *  limit) as egress slots free. Called from the egress device's freed hook
   *  on this node's LP. */
  void ServicePending (uint32_t portIdx);

  /**
   * Per-port receive upcall: parse already done by SueNetDevice::Receive.
   * Routes by XPU-id, decrements shim TTL (drop if 0), maps VC, sends to egress.
   */
  void SwitchReceiveFromDevice (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h);

  // --- AFH counters (§6.11) ---
  uint64_t GetCntRxPkts () const;
  uint64_t GetCntTxPkts () const;
  uint64_t GetCntLookupMiss () const;
  uint64_t GetCntTtlZeroDrop () const;
  uint64_t GetCntVcRx (uint8_t vc) const;
  uint64_t GetCntVcTx (uint8_t vc) const;
  void ResetCounters ();

  // --- VRF counters (§12.40) ---
  /** Frames forwarded (lookup hit + TTL>0) whose derived VRF == vrf. */
  uint64_t GetCntVrfRx (uint32_t vrf) const;
  /** Lookup misses whose derived VRF == vrf (AFR_DST_LOOKUP_MISS per segment). */
  uint64_t GetCntVrfMiss (uint32_t vrf) const;

  // --- CBFC backpressure counters (§12.30) ---
  uint64_t GetCntPendPkts () const;   // cumulative frames held at the switch pending
  uint64_t GetCntPendMax () const;    // peak concurrent pending depth

protected:
  virtual void DoDispose (void);

private:
  /** Frame the switch received but could not yet admit to the (full) egress
   *  queue; its ingress credit stays consumed until it is admitted. */
  struct PendingFrame { Ptr<SueNetDevice> ingress; uint8_t vc; Ptr<Packet> p; };
  int32_t GetOutPort (uint32_t xpuId) const;
  /** §6.9 flow-entropy ECMP pick (SA/DA/FlowLabel); see SueRoutingTable. */
  int32_t GetOutPort (uint32_t xpuId, uint64_t flowEntropy) const;
  /** §12.40 VRF-segmented pick (vrf=0 = legacy table). */
  int32_t GetOutPort (uint32_t xpuId, uint32_t vrf, uint64_t flowEntropy) const;
  /** VRF derivation (Switch-ASIC-AFH.md §6.6.4; §12.40): shim DSCP mapping,
   *  then VLAN vid mapping, then ingress-port mapping; un-mapped = VRF0.
   *  User-decided precedence: DSCP > VLAN > port. */
  uint32_t ComputeVrf (uint32_t portIdx, const SueHeader &h) const;
  /** Reverse map lookup: ingress device -> port index (AddPort order). */
  uint32_t GetPortIdx (Ptr<SueNetDevice> dev) const;
  void SendToDev (Ptr<SueNetDevice> ingress, uint32_t portIdx, uint8_t vc, Ptr<Packet> p);
  /** Admit-or-pend: if the egress VC queue can accept, enqueue + refund the
   *  ingress credit; else hold in m_pending[portIdx][vc] (FIFO). Runs on this
   *  node's LP (direct call or the fwdBudget scheduled event). */
  void Forward (Ptr<SueNetDevice> ingress, uint32_t portIdx, uint8_t vc, Ptr<Packet> p);
  /** VC: shim DSCP & 0x7 > VLAN PCP > 0 (DESIGN.md §7; configurable later). */
  uint8_t ComputeVc (const SueHeader &h) const;

  std::vector<Ptr<SueNetDevice>> m_ports;
  std::map<Ptr<SueNetDevice>, uint32_t> m_portIdxByDev;   // ingress dev -> port index (§12.40)
  Ptr<SueRoutingTable> m_rt;                       // VRF0 table (legacy member)
  std::map<uint32_t, Ptr<SueRoutingTable>> m_rtByVrf;   // VRF>0 segmented tables (§12.40)
  Time m_fwdBudget{TimeStep (0)};   // forwarding budget (0 = immediate)
  // CBFC backpressure (§12.30): per-(egress port, VC) pending FIFOs + counters.
  std::vector<std::array<std::deque<PendingFrame>, qCnt>> m_pending;
  uint64_t m_pendPkts{0};   // cumulative pended
  uint64_t m_pendDepth{0};  // current pending depth
  uint64_t m_pendMax{0};    // peak pending depth

  // AFH counters
  uint64_t m_rxPkts{0};
  uint64_t m_txPkts{0};
  uint64_t m_lookupMiss{0};
  uint64_t m_ttlZeroDrop{0};
  uint64_t m_vcRx[qCnt]{};
  uint64_t m_vcTx[qCnt]{};
  // VRF counters (§12.40)
  std::map<uint32_t, uint64_t> m_vrfRx;
  std::map<uint32_t, uint64_t> m_vrfMiss;
};

} // namespace ns3

#endif /* SUE_SWITCH_NODE_H */

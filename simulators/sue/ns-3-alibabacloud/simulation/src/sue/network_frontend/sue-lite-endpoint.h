/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE host-side traffic endpoint (DESIGN.md §6.12, S5b): the self-simulated
 * traffic generator/receiver that drives a host's SueNetDevice from
 * application-level code, so S5b (512-card end-to-end stress) has a uniform,
 * configurable sender instead of per-scratch hand-written SendOne loops.
 *
 * Mounted on a host's device via SetDevice (replaces the module default
 * counting upcall HostDefaultRx once installed, so it sees every received
 * frame). TX is a periodic frame generator: MAC DA high 10 bits = destination
 * XPU-id, VC encoded on the wire via shim.dscp (low 3 bits) or VLAN PCP
 * (§5.8) matching SueNetDevice::ComputeVc. Supports a single destination or a
 * per-frame cycle over a destination list (ring / all-to-all traffic shapes).
 *
 * MTP (§10.5): SueLiteEndpoint extends Object, NOT Application (DESIGN
 * §12.21). Application::DoInitialize schedules StartApplication via plain
 * Simulator::Schedule on the public LP, which trips the residency assertion
 * under the fork's MTP. Instead TX is driven ScheduleWithContext(nodeId, ...)
 * for the first frame (re-homed onto the owning node's LP by Partition) and a
 * self-chained plain Simulator::Schedule inside SendFrame afterwards, which
 * inherits that LP. Every SendFrame arm is guarded by SueMtpResidencyCheck.
 *
 * ENABLE_SCALEUP_WORKLOAD (DESIGN §9/§6.12): 0 (default) = built-in generator.
 * 1 (future astra-sim workload) is registered but not wired: Start() logs a
 * warning and falls back to the built-in generator. The AstraNetworkAPI-style
 * sim_send/sim_recv/sim_schedule stubs exist for that future path; they log a
 * warning and return -1.
 */
#ifndef SUE_LITE_ENDPOINT_H
#define SUE_LITE_ENDPOINT_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"
#include "ns3/sue-header.h"
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace ns3 {

class SueNetDevice;
class Packet;

/** Host-side self-simulated traffic endpoint (DESIGN §6.12). See the header
 *  comment for the MTP / VC / workload-gate design. */
class SueLiteEndpoint : public Object
{
public:
  SueLiteEndpoint ();
  static TypeId GetTypeId (void);

  // --- binding ---
  /** Bind to the host's device: installs this endpoint's receive upcall
   *  (replacing HostDefaultRx) and records the owning node id. */
  void SetDevice (Ptr<SueNetDevice> dev);
  /** Local XPU-id (encoded into the MAC SA of sent frames). */
  void SetSourceXpu (uint32_t xpuId);

  // --- frame format ---
  /** Single fixed destination XPU-id. Clears any destination cycle. */
  void SetDestinationXpu (uint32_t xpuId);
  /** Per-frame cycle over destination XPU-ids (round-robin). Clears the
   *  single-destination setting. */
  void SetDestinationCycle (const std::vector<uint32_t> &xpUs);
  /** Payload size in bytes (default 86). */
  void SetPayloadSize (uint32_t bytes);
  /** VC 0..7, LEGACY convenience for the default (direct) map: sets BOTH the
   *  shim-DSCP flow class and the VLAN-PCP flow class to vc&0x07, so under the
   *  default DSCP_TO_VC_MAP / PCP_TO_VC_MAP (direct) the wire VC equals vc.
   *  For a custom map or a full 6-bit DSCP flow class use SetDscp instead. */
  void SetVc (uint8_t vc);
  /** Shim-DSCP flow class (6-bit, 0-63). The wire VC is derived from it via
   *  the DSCP_TO_VC_MAP table (DESIGN §7; default direct dscp&0x07). Warns if
   *  nonzero with neither shim nor VLAN enabled (no wire encoding). Call
   *  SetUseShim(true) BEFORE SetDscp to avoid the spurious warning. */
  void SetDscp (uint8_t dscp);
  /** VLAN-PCP flow class (3-bit, 0-7). Wire VC via PCP_TO_VC_MAP (default pcp
   *  identity). Warns if nonzero with neither encoding enabled. */
  void SetPcp (uint8_t pcp);
  /** VLAN DEI bit (with SetPcp: 16-entry PCP_TO_VC_MAP rows, §7). */
  void SetDei (bool dei);
  void SetUseShim (bool on);
  void SetUseVlan (bool on);
  /** Shim TTL (4-bit field, §5.3: 0-15). Default 8; values > 15 are clamped
   *  with a warning (a too-large value truncates to 0 on the wire, which the
   *  switch reads as TTL==0 and drops). */
  void SetTtl (uint8_t ttl);
  /** EtherType for sent frames. Warns if its per-etype *HAS_SHIM config (§9)
   *  disagrees with SetUseShim (the receiver would parse the shim differently
   *  than this endpoint encodes). If never called, the effective EtherType is
   *  auto-selected per frame: a shim-bearing one (EthType1 by default config)
   *  when useShim, else EthType0 — so the wire parse always matches the VC
   *  encoding. */
  void SetEtherType (uint16_t et);

  // --- scheduling ---
  void SetInterval (Time interval);
  /** Number of frames to send (0 = unlimited until Stop()). */
  void SetFrameCount (uint32_t n);
  void SetStartTime (Time t);
  /** Per-endpoint start offset added to SetStartTime (MTP equivalence
   *  dithering, DESIGN §12.20: distinct non-interval-multiple offsets per
   *  host keep every frame departure unique). */
  void SetDither (Time dither);
  /** Start the generator. No-op if already running or no device bound.
   *  Requires a destination (SetDestinationXpu or SetDestinationCycle). */
  void Start ();
  /** Stop the generator: cancels the pending self-chained send event and
   *  prevents further frames. The un-cancelled first-frame ScheduleWithContext
   *  event (fire-and-forget under MTP) is a no-op when it fires. */
  void Stop ();

  // --- stats ---
  uint64_t GetTxCount () const;
  uint64_t GetRxCount () const;
  uint64_t GetRxBytes () const;
  /** Received frames attributed to VC (shim.dscp&7 > vlan.pcp&7 > 0, same as
   *  SueNetDevice::ComputeVc). */
  uint64_t GetRxByVc (uint8_t vc) const;
  /** Received frames whose MAC DA high 10 bits == xpuId. */
  uint64_t GetRxByDst (uint32_t xpuId) const;
  /** Received frames whose MAC SA high 10 bits == xpuId (who sent to me). */
  uint64_t GetRxBySrc (uint32_t xpuId) const;
  const std::map<uint32_t, uint64_t> &GetRxBySrcMap () const;

  // --- AstraNetworkAPI-style reserved stubs (ENABLE_SCALEUP_WORKLOAD=1,
  // future astra-sim workload, §6.12). Log a warning and return -1. ---
  int64_t sim_send (uint32_t src, uint32_t dst, void *payload);
  int64_t sim_recv (uint32_t src, uint32_t dst, void *payload);
  int64_t sim_schedule (uint32_t src, uint32_t dst, uint64_t cycle);

private:
  void SendFrame ();
  /** Effective EtherType for the next frame: the explicitly-set one, else an
   *  auto-pick that matches useShim (see SetEtherType doc). */
  uint16_t ResolveEtherType () const;
  void Rx (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h);

  Ptr<SueNetDevice> m_dev;
  uint32_t m_nodeId{0};
  uint32_t m_srcXpu{0};
  uint32_t m_dstXpu{0xFFFFFFFF};   // 0xFFFFFFFF = no single destination set
  std::vector<uint32_t> m_dstCycle;
  uint32_t m_cycleIdx{0};
  uint32_t m_payloadSize{86};
  uint8_t m_dscp{0};   // 6-bit shim-DSCP flow class (wire shim.dscp; VC via DSCP_TO_VC_MAP, §7)
  uint8_t m_pcp{0};    // 3-bit VLAN-PCP flow class (wire PCP; VC via PCP_TO_VC_MAP, §7)
  bool m_dei{false};  // VLAN DEI bit (row select in the 16-entry PCP_TO_VC_MAP)
  bool m_useShim{false};
  bool m_useVlan{false};
  uint8_t m_ttl{8};
  uint16_t m_etherType{SUE_ETH_TYPE_0};
  bool m_etherTypeExplicit{false};
  Time m_interval{MicroSeconds (20)};
  uint32_t m_frameCount{0};        // 0 = unlimited
  uint32_t m_sent{0};
  Time m_startTime{Seconds (1)};
  Time m_dither{Seconds (0)};
  EventId m_sendEvent;
  bool m_running{false};

  uint64_t m_txCount{0};
  uint64_t m_rxCount{0};
  uint64_t m_rxBytes{0};
  uint64_t m_rxByVc[8]{0, 0, 0, 0, 0, 0, 0, 0};
  std::map<uint32_t, uint64_t> m_rxByDst;
  std::map<uint32_t, uint64_t> m_rxBySrc;
};

/** Install one SueLiteEndpoint on every host device (module default counting
 *  upcall is replaced). `cfg` (optional) configures each endpoint before it is
 *  returned; endpoints are bound but NOT started (Start() is the caller's
 *  step, so all hosts can be configured before any frame departs). Returns the
 *  endpoint list in host-index order. */
std::vector<Ptr<SueLiteEndpoint>>
SueInstallLiteEndpoints (std::function<void (Ptr<SueLiteEndpoint>, uint32_t hostIdx)> cfg = nullptr);

} // namespace ns3

#endif /* SUE_LITE_ENDPOINT_H */

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE configuration (DESIGN.md §9). GlobalValue-backed knobs + a SimAI.conf-
 * style parser (`SueReadConf`) + a per-device applier (`ApplySueConfig`) that
 * maps the GlobalValues onto the existing engine/device setters.
 *
 * The values are module-wide (one config file per run), matching how the
 * scale-out baseline's ReadConf/SetConfig consume a single SimAI.conf.
 */
#ifndef SUE_CONFIG_H
#define SUE_CONFIG_H

#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include <string>

namespace ns3 {

class SueNetDevice;

/** Runtime-configurable EtherTypes (defaults match the compile-time
 *  SUE_ETH_TYPE_* constants in sue-header.h). The two values are symmetric SUE
 *  protocol identifiers; shim presence is a per-etype *HAS_SHIM decision, not
 *  a property of the value (DESIGN §5.2/§9). Read through the GlobalValues so
 *  SueReadConf can override them before traffic starts. */
namespace SueConfig {
uint16_t EthType0 ();
uint16_t EthType1 ();
/** True if frames of `etherType` parse as shim-bearing (per *HAS_SHIM). */
bool HasShim (uint16_t etherType);
/** Drop the first-use caches (EtherType / *_HAS_SHIM + the VC maps). SueReadConf
 *  calls this after parsing (a re-read may change SUE_ETH_TYPE_*, *_HAS_SHIM,
 *  DSCP_TO_VC_MAP, PCP_TO_VC_MAP, DEFAULT_VC); call it too if you SetGlobal
 *  one of those keys after traffic has started reading them. */
void InvalidateCache ();
/** VC for a shim DSCP flow class (6-bit, 0-63) via the DSCP_TO_VC_MAP table
 *  (DESIGN §7; default = dscp&0x07 for all 64 values). End-to-end: the host
 *  send path, host receive attribution and the switch all decode the same VC
 *  from the same table. */
uint8_t DscpToVc (uint8_t dscp);
/** VC for a VLAN PCP (3-bit, 0-7) via the PCP_TO_VC_MAP table (§7; default =
 *  pcp identity). The map may be extended to 16 entries keyed (dei<<3|pcp) —
 *  the DEI-carrying overload; the single-arg form reads the DEI=0 row (with an
 *  8-entry map both rows are identical, so legacy callers are unaffected). */
uint8_t PcpToVc (uint8_t pcp);
uint8_t PcpToVc (uint8_t pcp, bool dei);
/** VC for frames with neither shim nor VLAN (DEFAULT_VC; default 0). §7. */
uint8_t DefaultVc ();
/** VRF for a shim DSCP (6-bit, 0-63) via DSCP_TO_VRF_MAP (DESIGN §12.40;
 *  default empty = VRF0; un-mapped = 0 meaning "try the next source"). */
uint32_t DscpToVrf (uint8_t dscp);
/** VRF for a VLAN vid (12-bit, 0-4095) via VLAN_TO_VRF_MAP (§12.40; default
 *  empty = VRF0; un-mapped = 0 meaning "try the next source"). */
uint32_t VlanToVrf (uint16_t vid);
/** VRF for an ingress port index (switch AddPort order, 0-63) via
 *  PORT_TO_VRF_MAP (§12.40; default empty = VRF0 — lowest precedence). */
uint32_t PortToVrf (uint32_t portIdx);
/** One-way link propagation delay (ns) set on every SUE channel at build
 *  time (SUE_LINK_PROPAGATION_NS). */
Time LinkPropagation ();
/** Switch per-hop forwarding budget (ns): forwarding latency + egress wire
 *  time <= budget (SUE_SWITCH_FWD_BUDGET_NS). */
Time SwitchForwardingBudget ();
/** CBFC per-VC switch egress buffer depth limit in bytes
 *  (CBFC_EGRESS_BUF_BYTES; 0 = auto = creditLimit × creditSize per VC).
 *  The switch egress→ingress backpressure gate (§6.7/§12.30). */
uint64_t CbfcEgressBufBytes ();
}

/** Parse a SimAI.conf-style file ("KEY VALUE" lines, '#' comments and blank
 *  lines skipped) into the registered GlobalValues. Unknown keys are skipped
 *  silently (mirrors the baseline ReadConf fallthrough). Returns false if the
 *  file cannot be opened. */
bool SueReadConf (const std::string &confFile);

/** Apply the registered GlobalValues to one SueNetDevice via its existing
 *  setters. When FC_MODE != OFF and the device has no flow-control facade yet,
 *  builds the engine(s) and facade and wires them in. Safe to call on a device
 *  with engines already configured (only fills unset parts). */
void ApplySueConfig (Ptr<SueNetDevice> dev);

} // namespace ns3

#endif /* SUE_CONFIG_H */

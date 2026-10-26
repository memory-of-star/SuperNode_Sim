/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE scale-up network entry (DESIGN.md §1.3/§6.8). Builds the SUE network
 * from a topo file + optional SimAI.conf, and dispatches on the ENABLE_SCALEUP
 * config knob. SUE-side integration only: the scale-out baseline (reference/)
 * keeps its own SetupNetwork; this is the single in-tree SUE entry point.
 */
#ifndef SUE_ENTRY_H
#define SUE_ENTRY_H

#include "ns3/ptr.h"
#include <string>
#include <cstdint>

namespace ns3 {

class SueNetDevice;

/** Build the SUE scale-up network from a topo file.
 *
 * Topo format ('#' comments / blank lines skipped; KEY VALUE lines):
 *   type=SUE           section marker (only this section is parsed)
 *   nodes <N>          total node count (hosts + switches)
 *   switch <id...>     space-separated node ids that are SUESwitchNode
 *                      (the remaining ids 0..N-1 are hosts)
 *   link <src> <dst> <rate> <llr_ena> <mode> <err_rate>
 *     rate    : ns-3 DataRate string, e.g. "100Gbps"
 *     llr_ena : 0|1  (attach + enable the LLR engine on both ends)
 *     mode    : CBFC | PFC | OFF (per-link flow control)
 *     err_rate: double, recorded into the LINK_ERROR_RATE_PER_LINK GlobalValue
 *               (deterministic corruptor sets remain the caller's job, §10.1)
 *
 * Per device: the FC facade is wired per the link line (OFF = explicit
 * empty facade, so ApplySueConfig won't auto-add one), LLR engine per
 * llr_ena, then ApplySueConfig() fills tunables from the (conf) GlobalValues.
 * Per switch: a SueRoutingTable installed by BFS (XPU-id LPM, 10-bit exact).
 * Per host: a counting receive upcall (override via SueGetDevice()+
 * SetReceiveUpcall). Time resolution is the caller's concern; use
 * SueEntryDispatch() for the config-driven resolution setup.
 */
void SetupSueNetwork (const std::string &topoFile, const std::string &confFile = "");

/** Entry dispatch (DESIGN.md §6.8). Reads the ENABLE_SCALEUP GlobalValue:
 *   != 0 -> SetResolution(PS) once (if still NS) + SueReadConf(confFile) +
 *           SetupSueNetwork(topoFile)
 *   == 0 -> externalSetup()  (scale-out path; keeps its own SetupNetwork)
 * Contract: this is the only in-tree integration point; reference/ main1 is
 * not modified (gitignored mirror keeps its own dispatch). */
void SueEntryDispatch (const std::string &topoFile, const std::string &confFile,
                       void (*externalSetup) (void) = 0);

/** Accessors into the last-built SUE network (all invalid-safe). */
Ptr<SueNetDevice> SueGetDevice (uint32_t nodeId, uint32_t devIdx = 0);
uint32_t SueGetNNodes ();
uint32_t SueGetNHosts ();
/** XPU-id (MAC DA high 10 bits) of the host at index `hostIdx` (0-based). */
uint32_t SueGetHostXpuId (uint32_t hostIdx);
/** Node id of the host at index `hostIdx` (0-based). Invalid index -> 0. */
uint32_t SueGetHostNodeId (uint32_t hostIdx);

} // namespace ns3

#endif /* SUE_ENTRY_H */

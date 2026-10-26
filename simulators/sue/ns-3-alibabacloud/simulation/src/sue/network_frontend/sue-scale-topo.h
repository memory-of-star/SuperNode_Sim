/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * ScaleUp topology generator (docs/scaleup-topology.md).
 *
 * Model: N "8-card machines" — each machine is 8 XPU nodes + 1 internal switch
 * chip (SUESwitchNode) + 2 uplinks; machines interconnect through a 2-plane
 * fat-spine layer (ONE spine per plane, each spine uplinking ALL N machines —
 * the s8 topology scaled up). Cross-machine paths have 2 equal-cost first hops
 * -> ECMP groups via the standard BFS routing install (sue-entry.cc
 * InstallRouting). Multi-spine-per-plane groups would isolate machine groups
 * unless spine-spine links are added (out of scope; see
 * docs/scaleup-topology.md).
 *
 * Node layout (nNodes = 9N + 2):
 *   machine i: XPU i*9+0 .. i*9+7, internal switch i*9+8
 *   plane 0 spine: 9N        plane 1 spine: 9N + 1
 * The emitted topo file feeds the existing SetupSueNetwork path unchanged.
 */
#ifndef SUE_SCALE_TOPO_H
#define SUE_SCALE_TOPO_H

#include <cstdint>
#include <iosfwd>
#include <string>

namespace ns3 {

/** Emit an N-machine scaleup topo in the standard SUE topo format.
 *  rate: link rate string ("400Gbps"). Returns the total node count. */
uint32_t SueWriteScaleUpTopo (std::ostream &out, uint32_t nMachines,
                              const std::string &rate = "400Gbps");

/** Node-count / spine-port layout stats for planning (no topo written). */
struct SueScaleUpTopoStats
{
  uint32_t machines;
  uint32_t xpus;
  uint32_t internalSwitches;
  uint32_t spinesPerPlane;
  uint32_t spineNodes;      // 2 * spinesPerPlane
  uint32_t totalNodes;      // 9N + 2P
  uint32_t totalLinks;      // 10N + 2N (uplinks) = 12N... = internal 8N + uplink 2N + spine->machine is the same 2N
};
SueScaleUpTopoStats SueScaleUpTopoPlan (uint32_t nMachines);

/** XPU node id / internal switch node id / spine node id helpers. */
uint32_t SueScaleUpXpuNodeId (uint32_t machine, uint32_t slot);
uint32_t SueScaleUpSwitchNodeId (uint32_t machine);
uint32_t SueScaleUpSpineNodeId (uint32_t plane, uint32_t spine, uint32_t nMachines);

} // namespace ns3

#endif /* SUE_SCALE_TOPO_H */

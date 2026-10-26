/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * ScaleUp topology generator implementation (docs/scaleup-topology.md).
 */
#include "sue-scale-topo.h"

#include <ostream>

namespace ns3 {

uint32_t
SueScaleUpXpuNodeId (uint32_t machine, uint32_t slot)
{
  return machine * 9 + slot;
}

uint32_t
SueScaleUpSwitchNodeId (uint32_t machine)
{
  return machine * 9 + 8;
}

uint32_t
SueScaleUpSpineNodeId (uint32_t plane, uint32_t spine, uint32_t nMachines)
{
  (void) spine;   // one fat spine per plane: it uplinks EVERY machine
  return 9 * nMachines + plane;
}

SueScaleUpTopoStats
SueScaleUpTopoPlan (uint32_t nMachines)
{
  SueScaleUpTopoStats st;
  st.machines = nMachines;
  st.xpus = 8 * nMachines;
  st.internalSwitches = nMachines;
  // One spine per plane, every spine uplinking ALL machines (the s8 2-plane
  // topology scaled up). Multi-spine-per-plane groups would isolate machine
  // groups from each other unless spine-spine links exist (out of scope).
  st.spinesPerPlane = 1;
  st.spineNodes = 2 * st.spinesPerPlane;
  st.totalNodes = 9 * nMachines + st.spineNodes;
  st.totalLinks = 10 * nMachines;   // 8 internal + 2 uplinks per machine
  return st;
}

uint32_t
SueWriteScaleUpTopo (std::ostream &out, uint32_t nMachines, const std::string &rate)
{
  const SueScaleUpTopoStats st = SueScaleUpTopoPlan (nMachines);
  out << "# scaleup topo: " << nMachines << " x 8-XPU machines, 2-plane fat spine"
      << " (docs/scaleup-topology.md)\n"
      << "type=SUE\n"
      << "nodes " << st.totalNodes << "\n";
  out << "switch";
  for (uint32_t i = 0; i < nMachines; ++i) { out << " " << SueScaleUpSwitchNodeId (i); }
  for (uint32_t s = 0; s < st.spineNodes; ++s) { out << " " << 9 * nMachines + s; }
  out << "\n";
  for (uint32_t i = 0; i < nMachines; ++i)
    {
      // 8 internal links: XPU <-> internal switch chip (1 hop intra-machine)
      for (uint32_t k = 0; k < 8; ++k)
        {
          out << "link " << SueScaleUpXpuNodeId (i, k) << " " << SueScaleUpSwitchNodeId (i)
              << " " << rate << " 1 CBFC 0\n";
        }
      // 2 uplinks: one per spine plane (equal-cost ECMP across planes; each
      // fat spine connects every machine, so any cross-machine path exists)
      out << "link " << SueScaleUpSwitchNodeId (i) << " "
          << SueScaleUpSpineNodeId (0, 0, nMachines)
          << " " << rate << " 1 CBFC 0\n";
      out << "link " << SueScaleUpSwitchNodeId (i) << " "
          << SueScaleUpSpineNodeId (1, 0, nMachines)
          << " " << rate << " 1 CBFC 0\n";
    }
  return st.totalNodes;
}

} // namespace ns3

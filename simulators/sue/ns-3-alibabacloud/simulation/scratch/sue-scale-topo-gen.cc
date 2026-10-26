/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * ScaleUp topo generator (docs/scaleup-topology.md): emit the N x 8-XPU
 * machine topo in the standard SUE topo format for inspection / reuse by
 * other drivers. Run:
 *   cmake --build build -j8 --target scratch_sue-scale-topo-gen
 *   SCALE_N=64 ./build/scratch/ns3.36.1-sue-scale-topo-gen-default
 * Writes /tmp/sue-scale-topo.txt; prints the layout stats.
 */
#include "ns3/sue-scale-topo.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace ns3;

int
main ()
{
  uint32_t n = 64;
  if (const char *e = std::getenv ("SCALE_N")) { n = std::atoi (e); }
  if (n < 1) { n = 64; }
  const SueScaleUpTopoStats st = SueScaleUpTopoPlan (n);

  const char *path = "/tmp/sue-scale-topo.txt";
  std::ofstream out (path);
  const uint32_t nodes = SueWriteScaleUpTopo (out, n, "400Gbps");
  out.close ();

  std::cout << "SCALE TOPO-GEN machines=" << st.machines << " xpus=" << st.xpus
            << " internalSwitches=" << st.internalSwitches
            << " spinesPerPlane=" << st.spinesPerPlane << " spineNodes=" << st.spineNodes
            << " totalNodes=" << st.totalNodes << " links=" << st.totalLinks
            << " -> " << path << "\n";
  bool ok = nodes == st.totalNodes;
  std::cout << (ok ? "SCALE TOPO-GEN PASS" : "SCALE TOPO-GEN FAIL") << std::endl;
  return ok ? 0 : 1;
}

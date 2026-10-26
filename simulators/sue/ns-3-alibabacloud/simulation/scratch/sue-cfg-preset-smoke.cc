/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * E1 conf-preset smoke test (docs/review-fix-plan.md Stage E1): every
 * delivered conf preset under conf/ must PARSE (SueReadConf true), WIRE a
 * real network through the config-driven entry (SueEntryDispatch), and
 * deliver a frame end to end. Previously no automated test ever read the
 * conf/ presets — the scratch programs mirror them inline and had already
 * drifted (credit limits, data-age). Run:
 *   cmake --build build -j8 --target scratch_sue-e1-conf-smoke
 *   ./build/scratch/ns3.36.1-sue-cfg-preset-smoke-default
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-config.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-header.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"

#include <fstream>
#include <iostream>
#include <string>

using namespace ns3;

static bool g_ok = true;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    bool c = (cond);                                                           \
    std::cout << "  [" << (c ? "ok" : "FAIL") << "] " << msg << "\n";          \
    g_ok = g_ok && c;                                                          \
  } while (0)


static uint32_t g_rx = 0;
static void
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  ++g_rx;
}

int
main ()
{
  // Resolve conf/ next to the source tree (__FILE__ is absolute in this
  // build): <sim>/scratch/sue-e1-conf-smoke.cc -> <sim>/conf/<name>.
  const std::string simDir = [] () {
    std::string f (__FILE__);
    return f.substr (0, f.rfind ("/scratch/"));
  } ();

  const char *topo = "/tmp/sue-e1-topo.txt";
  // 3 档预置(默认 100G / 200G / 400G;原 1G 档与独立 100g 档已合并移除,§12.36)
  const char *names[] = {"sue-default.conf", "sue-200g.conf", "sue-400g.conf"};
  for (const char *name : names)
    {
      const std::string conf = simDir + "/conf/" + name;
      std::cout << "[" << name << "] conf preset smoke\n";
      // Each preset must parse cleanly through the config entry...
      CHECK (SueReadConf (conf), "SueReadConf(" << name << ") parses");
      // ...and drive a real 3-node network end to end (host0 -> sw -> host2).
      std::ofstream (topo) << "type=SUE\n"
                           << "nodes 3\n"
                           << "switch 1\n"
                           << "link 0 1 100Gbps 1 CBFC 0\n"
                           << "link 1 2 100Gbps 1 CBFC 0\n";
      g_rx = 0;
      SueEntryDispatch (topo, conf, nullptr);
      CHECK (SueGetNNodes () == 3, "3 nodes built");
      SueGetDevice (2, 0)->SetReceiveUpcall (MakeCallback (&RxObs));
      Ptr<Packet> p = Create<Packet> (86);
      SueHeader h;
      // XPU-id = host index (0..NHosts-1), not node id: host node 2 is the
      // second host -> index 1 (SueGetHostXpuId).
      h.SetDst (SueMacFromXpuId (SueGetHostXpuId (1)));
      h.SetSrc (SueMacFromXpuId (SueGetHostXpuId (0)));
      h.SetEtherType (SUE_ETH_TYPE_0);
      p->AddHeader (h);
      SueGetDevice (0, 0)->SwitchSend (0, p);
      Simulator::Stop (MilliSeconds (2));
      Simulator::Run ();
      CHECK (g_rx == 1, "frame delivered end to end under the preset");
      if (g_rx != 1)
        {
          Ptr<SUESwitchNode> sw = DynamicCast<SUESwitchNode> (SueGetDevice (1, 0)->GetNode ());
          auto e0 = SueGetDevice (0, 0)->GetLlrEngine ();
          std::cout << "  [dbg] rx=" << g_rx << " host0.txState="
                    << (e0 ? (uint32_t) e0->GetTxState () : 99u)
                    << " host0.replay=" << (e0 ? e0->GetReplaySize () : 0u)
                    << " sw.rx=" << (sw ? sw->GetCntRxPkts () : 0u)
                    << " sw.tx=" << (sw ? sw->GetCntTxPkts () : 0u)
                    << " sw.miss=" << (sw ? sw->GetCntLookupMiss () : 0u) << "\n";
        }
      Simulator::Destroy ();
    }

  std::cout << (g_ok ? "CFG PRESET-SMOKE PASS" : "CFG PRESET-SMOKE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

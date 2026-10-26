/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE scale-up network entry implementation (DESIGN.md §6.8).
 * Topo parsing -> node/device/channel build -> FC/LLR wiring ->
 * ApplySueConfig tunables -> BFS routing. See sue-entry.h for the format.
 */
#include "sue-entry.h"
#include "sue-config.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-channel.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-error-model.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/sue-routing.h"
#include "ns3/node.h"
#include "ns3/data-rate.h"
#include "ns3/nstime.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/config.h"
#include "ns3/log.h"
#include <fstream>
#include <sstream>
#include <deque>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <cstdint>

NS_LOG_COMPONENT_DEFINE ("SueEntry");

namespace ns3 {

namespace {

// ---- last-built network state (one SUE network per process) ----
std::vector<Ptr<Node>> g_nodes;
std::set<uint32_t> g_switchIds;
std::vector<uint32_t> g_hostIds;   // host node ids, in order -> XPU-id = index

// Atomic: the default host upcall runs on each node's own LP thread under MTP
// (channel seam routes RX per node), so concurrent ++ would be a data race.
std::atomic<uint32_t> g_hostRx{0};
void
HostDefaultRx (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  g_hostRx.fetch_add (1, std::memory_order_relaxed);
}

struct LinkSpec
{
  uint32_t src, dst;
  DataRate rate;
  uint32_t llrEna;
  std::string mode;
  double errRate;
};

bool g_resSet = false;
/** PS resolution for the sub-ns LLR timers. Only upgrades from the ns3 default
 *  NS; a caller that already chose a resolution keeps theirs. */
void
MaybeSetPsResolution ()
{
  if (g_resSet) { return; }
  g_resSet = true;
  if (Time::GetResolution () == Time::NS) { Time::SetResolution (Time::PS); }
}

/** Create one device on `nodeId` and wire its FC facade / LLR engine. */
Ptr<SueNetDevice>
MakeDeviceOn (uint32_t nodeId, DataRate rate, uint32_t llrEna, const std::string &mode)
{
  Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
  Ptr<Node> n = g_nodes[nodeId];
  if (g_switchIds.count (nodeId))
    {
      DynamicCast<SUESwitchNode> (n)->AddPort (dev);   // sets upcall -> SwitchReceiveFromDevice
    }
  else
    {
      n->AddDevice (dev);
      dev->SetReceiveUpcall (MakeCallback (&HostDefaultRx));
    }
  dev->SetDataRate (rate);
  // IFG is set by ApplySueConfig from SUE_IFG_BYTES at the link rate (§5.4);
  // device default m_ifg=0 holds until then.
  dev->SetEgressQueue (CreateObject<SueEgressQueue> ());

  // FC facade per the link line. OFF links get an explicit empty facade so the
  // data path is uniform and ApplySueConfig won't auto-add a CBFC one later.
  Ptr<SueFcController> fc = CreateObject<SueFcController> ();
  if (mode == "CBFC") { fc->SetCbfc (CreateObject<SueCbfcEngine> ()); }
  else if (mode == "PFC") { fc->SetPfc (CreateObject<SuePfcEngine> ()); }
  dev->SetFcController (fc);

  if (llrEna)
    {
      // Engine wired but NOT started yet: the LLR INIT handshake must run
      // after ApplySueConfig has set the two-phase modes/behaviors (so
      // LLR_MODE_REMOTE 0 genuinely suppresses INIT) and after the channel
      // peer exists (so INIT/INIT_ECHO are delivered). SetupSueNetwork calls
      // Start() for every LLR device once configuration is applied (§6.8).
      Ptr<SueLlrEngine> llr = CreateObject<SueLlrEngine> ();
      dev->SetLlrEngine (llr);
      dev->SetLlrEnabled (true);
    }
  return dev;
}

/** BFS from each switch: install an XPU-id LPM entry per reachable host.
 *  Equal-cost multi-path aware: ALL shortest-path first-hop ports are collected
 *  into an ECMP group (unit-cost links -> every alternative is found within the
 *  same BFS layer, added while expanding a different root-adjacent branch). */
void
InstallRouting ()
{
  for (uint32_t s : g_switchIds)
    {
      Ptr<SUESwitchNode> sw = DynamicCast<SUESwitchNode> (g_nodes[s]);
      Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
      sw->SetRoutingTable (rt);

      // nodeId -> BFS depth; nodeId -> first-hop egress ports (ECMP group);
      // (nodeId, firstHopPort) -> expanded guard: a node shared by several
      // equal-cost paths must be expanded once PER first-hop port, or the
      // second port's subtree (e.g. hosts behind the second spine) never gets
      // added to the ECMP group.
      std::map<uint32_t, uint32_t> dist;
      std::map<uint32_t, std::set<uint32_t>> ports;
      std::map<uint32_t, std::set<uint32_t>> expanded;
      dist[s] = 0;
      std::deque<std::pair<uint32_t, uint32_t>> q;  // (nodeId, hopPort)
      q.push_back ({s, 0}); expanded[s].insert (0);
      while (!q.empty ())
        {
          uint32_t u = q.front ().first;
          uint32_t hop = q.front ().second;
          q.pop_front ();
          Ptr<Node> n = g_nodes[u];
          for (uint32_t d = 0; d < n->GetNDevices (); ++d)
            {
              Ptr<SueNetDevice> sd = DynamicCast<SueNetDevice> (n->GetDevice (d));
              if (!sd) { continue; }
              // Use the subclass accessor: GetChannel() (base
              // PointToPointNetDevice) returns the base's own null m_channel,
              // since SueNetDevice::Attach sets the subclass member.
              Ptr<SueChannel> ch = sd->GetSueChannel ();
              if (!ch) { continue; }
              for (size_t i = 0; i < ch->GetNDevices (); ++i)
                {
                  Ptr<NetDevice> peer = ch->GetDevice (i);
                  if (peer == sd) { continue; }            // skip self
                  Ptr<Node> vn = peer->GetNode ();
                  if (!vn) { continue; }
                  uint32_t v = vn->GetId ();
                  uint32_t vhop = (u == s) ? d : hop;
                  auto it = dist.find (v);
                  if (it == dist.end ())
                    {
                      // First discovery: min distance (unit-cost BFS).
                      dist[v] = dist[u] + 1;
                      ports[v].insert (vhop);
                    }
                  else if (it->second != dist[u] + 1)
                    {
                      continue;   // longer path: ignore
                    }
                  else
                    {
                      ports[v].insert (vhop);   // equal-cost alternative port
                    }
                  if (!expanded[v].count (vhop))
                    {
                      expanded[v].insert (vhop);
                      q.push_back ({v, vhop});
                    }
                }
            }
        }
      // install a 10-bit exact prefix per reachable host (XPU-id = host index);
      // multi-port prefixes become ECMP groups in SueRoutingTable::GetOutPort.
      for (uint32_t hi = 0; hi < g_hostIds.size (); ++hi)
        {
          auto it = ports.find (g_hostIds[hi]);
          if (it != ports.end ())
            {
              rt->AddPrefix (hi, 10,
                             std::vector<uint32_t> (it->second.begin (), it->second.end ()));
            }
        }
    }
}

} // anonymous namespace

Ptr<SueNetDevice>
SueGetDevice (uint32_t nodeId, uint32_t devIdx)
{
  if (nodeId >= g_nodes.size () || !g_nodes[nodeId]) { return 0; }
  if (devIdx >= g_nodes[nodeId]->GetNDevices ()) { return 0; }
  return DynamicCast<SueNetDevice> (g_nodes[nodeId]->GetDevice (devIdx));
}

uint32_t SueGetNNodes () { return g_nodes.size (); }
uint32_t SueGetNHosts () { return g_hostIds.size (); }
uint32_t SueGetHostXpuId (uint32_t hostIdx)
{
  return (hostIdx < g_hostIds.size ()) ? hostIdx : 0;
}
uint32_t SueGetHostNodeId (uint32_t hostIdx)
{
  return (hostIdx < g_hostIds.size ()) ? g_hostIds[hostIdx] : 0;
}

void
SetupSueNetwork (const std::string &topoFile, const std::string &confFile)
{
  // Reset the previous network state BEFORE parsing (the parser fills these).
  g_nodes.clear (); g_switchIds.clear (); g_hostIds.clear (); g_hostRx = 0;
  // Honor the confFile parameter (the header promises "topo + optional
  // SimAI.conf"): a direct caller passing a conf must get it applied — a
  // silently ignored conf was a config-not-taking-effect trap. Idempotent when
  // the caller (SueEntryDispatch) already read the same file.
  if (!confFile.empty ())
    {
      if (!SueReadConf (confFile))
        {
          NS_LOG_WARN ("SetupSueNetwork: SueReadConf failed for " << confFile
                       << " — proceeding with current GlobalValue state");
        }
    }

  // --- parse ---
  std::ifstream f (topoFile.c_str ());
  if (!f.is_open ()) { NS_LOG_ERROR ("SetupSueNetwork: cannot open topo " << topoFile); return; }
  std::vector<LinkSpec> links;
  uint32_t nNodes = 0;
  bool haveType = false;
  std::string line;
  while (std::getline (f, line))
    {
      std::string::size_type h = line.find ('#');
      if (h != std::string::npos) { line.erase (h); }
      std::istringstream iss (line);
      std::string key;
      if (!(iss >> key)) { continue; }
      if (key == "type=SUE") { haveType = true; }
      else if (key == "nodes") { uint32_t n; if (iss >> n) { nNodes = n; } }
      else if (key == "switch")
        {
          uint32_t id;
          while (iss >> id) { g_switchIds.insert (id); }
        }
      else if (key == "link")
        {
          LinkSpec l;
          std::string rate, mode;
          if (iss >> l.src >> l.dst >> rate >> l.llrEna >> mode >> l.errRate)
            {
              l.rate = DataRate (rate);
              l.mode = mode;
              links.push_back (l);
              // Record the per-link err_rate into the module-wide GlobalValue
              // (last link wins; the header comment promises this, and it makes
              // the value queryable/assertable). Random injection remains S5
              // (§10.1); the module still uses deterministic corruptor sets.
              Config::SetGlobal ("LINK_ERROR_RATE_PER_LINK", DoubleValue (l.errRate));
            }
          else { NS_LOG_WARN ("SetupSueNetwork: malformed link line: " << line); }
        }
      else { NS_LOG_LOGIC ("SetupSueNetwork: skipping unknown key '" << key << "'"); }
    }
  if (!haveType || nNodes == 0 || links.empty ())
    {
      NS_LOG_ERROR ("SetupSueNetwork: topo needs type=SUE / nodes / >=1 link: " << topoFile);
      return;
    }
  // Bounds validation BEFORE building: an out-of-range link endpoint or switch
  // id would index g_nodes out of bounds below (UB / segfault on malformed
  // topo files, e.g. "link 5 7 ..." with "nodes 5"). Reject with a clear error.
  for (const LinkSpec &l : links)
    {
      if (l.src >= nNodes || l.dst >= nNodes)
        {
          NS_LOG_ERROR ("SetupSueNetwork: link " << l.src << "->" << l.dst
                        << " references node id >= nodes(" << nNodes << "); rejecting topo " << topoFile);
          return;
        }
    }
  for (uint32_t sid : g_switchIds)
    {
      if (sid >= nNodes)
        {
          NS_LOG_ERROR ("SetupSueNetwork: switch id " << sid
                        << " >= nodes(" << nNodes << "); rejecting topo " << topoFile);
          return;
        }
    }

  // --- build nodes ---
  g_nodes.resize (nNodes);
  for (uint32_t i = 0; i < nNodes; ++i)
    {
      if (g_switchIds.count (i)) { g_nodes[i] = CreateObject<SUESwitchNode> (); }
      else { g_nodes[i] = CreateObject<Node> (); g_hostIds.push_back (i); }
    }

  // --- links: devices + channels. Device indices on a node are assigned in
  // link order, so record (nodeId, devIdx) -> link index for later lookup. ---
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> devLink;
  for (size_t li = 0; li < links.size (); ++li)
    {
      const LinkSpec &l = links[li];
      Ptr<SueNetDevice> da = MakeDeviceOn (l.src, l.rate, l.llrEna, l.mode);
      Ptr<SueNetDevice> db = MakeDeviceOn (l.dst, l.rate, l.llrEna, l.mode);
      devLink[{l.src, g_nodes[l.src]->GetNDevices () - 1}] = li;
      devLink[{l.dst, g_nodes[l.dst]->GetNDevices () - 1}] = li;
      Ptr<SueChannel> ch = CreateObject<SueChannel> ();
      ch->SetAttribute ("Delay", TimeValue (SueConfig::LinkPropagation ()));
      da->Attach (ch);
      db->Attach (ch);
      // PLAT-SP-08: per-link err_rate now DRIVES a probabilistic FCS model on
      // both link endpoints (previously a recorded GlobalValue only, DESIGN
      // §12.5). Random corruption is deterministic per run (dedicated RNG
      // stream per error model, seeded by the global seed). 0 = no model.
      if (l.errRate > 0.0)
        {
          // Per-link RNG stream indices: repeatable across network rebuilds.
          Ptr<SueErrorModel> emA = CreateObject<SueErrorModel> ();
          emA->SetRngStreamIdx (2 * li);
          emA->SetRandomErrorRate (l.errRate);
          da->SetErrorModel (emA);
          Ptr<SueErrorModel> emB = CreateObject<SueErrorModel> ();
          emB->SetRngStreamIdx (2 * li + 1);
          emB->SetRandomErrorRate (l.errRate);
          db->SetErrorModel (emB);
        }
    }

  // --- apply global tunables; re-assert per-link llr_ena (config default 1
  // would otherwise turn OFF links on); then bring LLR links up: Start() the
  // engine now that modes/behaviors are configured AND the peer device is
  // wired + attached, so the INIT handshake runs over the wire (a real
  // INIT/INIT_ECHO, not the GoAdvance fallback, §6.2/§6.8). ---
  for (uint32_t i = 0; i < nNodes; ++i)
    {
      Ptr<Node> n = g_nodes[i];
      for (uint32_t d = 0; d < n->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = DynamicCast<SueNetDevice> (n->GetDevice (d));
          if (!dev) { continue; }
          ApplySueConfig (dev);
          auto it = devLink.find ({i, d});
          dev->SetLlrEnabled (it != devLink.end () && links[it->second].llrEna);
          if (dev->GetLldpNegotiate ())
            {
              // NEG-SP-01/02: EVERY device exchanges its capability TLV (an
              // LLR-less peer advertises llrSupported=0 so its partner can
              // degrade); the LLR engines start from OnRemoteLldpTlv once the
              // peer TLV is applied. CBFC-SP-13: StartLldpExchange also gates
              // every VC off until the §5.2.8.4 RTS/RTR handshake completes.
              dev->StartLldpExchange ();
            }
          else if (dev->GetLlrEngine ())
            {
              dev->GetLlrEngine ()->Start ();
            }
        }
      // Switch per-hop forwarding budget (SUE_SWITCH_FWD_BUDGET_NS; 0 = off).
      Ptr<SUESwitchNode> sw = DynamicCast<SUESwitchNode> (n);
      if (sw)
        {
          sw->SetForwardingBudget (SueConfig::SwitchForwardingBudget ());
          // CBFC egress→ingress backpressure (CBFC_EGRESS_BUF_BYTES; 0 = auto).
          // Runs after ApplySueConfig (above) so the port engines already carry
          // their credit limits — the auto egress limit reads them (§12.30).
          sw->ApplyCbfcBackpressure (SueConfig::CbfcEgressBufBytes ());
        }
    }

  InstallRouting ();

  NS_LOG_INFO ("SetupSueNetwork: " << nNodes << " nodes, " << links.size ()
               << " links, " << g_hostIds.size () << " hosts, "
               << g_switchIds.size () << " switches");
}

void
SueEntryDispatch (const std::string &topoFile, const std::string &confFile,
                  void (*externalSetup) (void))
{
  // Read the conf first so a config-driven ENABLE_SCALEUP is honored; SUE_*
  // GlobalValues are namespace-separated, so this is harmless on the
  // scale-out path (its own ReadConf is untouched).
  if (!confFile.empty () && !SueReadConf (confFile))
    { NS_LOG_WARN ("SueEntryDispatch: SueReadConf failed for " << confFile); }
  UintegerValue v;
  GlobalValue::GetValueByName ("ENABLE_SCALEUP", v);
  if (v.Get () != 0)
    {
      MaybeSetPsResolution ();
      SetupSueNetwork (topoFile, confFile);
    }
  else if (externalSetup)
    {
      externalSetup ();
    }
}

} // namespace ns3

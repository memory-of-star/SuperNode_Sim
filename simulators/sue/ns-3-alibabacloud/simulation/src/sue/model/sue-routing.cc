/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE routing table implementation.
 */
#include "sue-routing.h"
#include "ns3/log.h"
#include "ns3/type-id.h"
#include <algorithm>

NS_LOG_COMPONENT_DEFINE ("SueRoutingTable");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueRoutingTable);

static const uint8_t XPU_ID_BITS = 10;

TypeId
SueRoutingTable::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueRoutingTable")
    .SetParent<Object> ()
    .AddConstructor<SueRoutingTable> ()
  ;
  return tid;
}

void
SueRoutingTable::AddPrefix (uint32_t xpuId, uint8_t prefixLen, std::vector<uint32_t> ecmpPorts)
{
  NS_LOG_FUNCTION (this << xpuId << (uint32_t) prefixLen << ecmpPorts.size ());
  if (prefixLen > XPU_ID_BITS) { prefixLen = XPU_ID_BITS; }
  Entry e;
  e.len = prefixLen;
  // store the top `prefixLen` bits as the match key
  e.prefix = (prefixLen == 0) ? 0 : (xpuId >> (XPU_ID_BITS - prefixLen));
  e.ports = std::move (ecmpPorts);
  m_entries.push_back (e);
  // keep longest-prefix first
  std::stable_sort (m_entries.begin (), m_entries.end (),
                    [] (const Entry &a, const Entry &b) { return a.len > b.len; });
}

bool
SueRoutingTable::Lookup (uint32_t xpuId, std::vector<uint32_t> &ports) const
{
  for (const Entry &e : m_entries)
    {
      uint32_t key = (e.len == 0) ? 0 : (xpuId >> (XPU_ID_BITS - e.len));
      if (key == e.prefix)
        {
          ports = e.ports;
          return true;
        }
    }
  return false;
}

int32_t
SueRoutingTable::GetOutPort (uint32_t xpuId) const
{
  return GetOutPort (xpuId, 0);
}

int32_t
SueRoutingTable::GetOutPort (uint32_t xpuId, uint64_t flowEntropy) const
{
  std::vector<uint32_t> ports;
  if (!Lookup (xpuId, ports) || ports.empty ())
    {
      return -1;
    }
  if (ports.size () == 1)
    {
      return (int32_t) ports.front ();
    }
  // Deterministic ECMP pick (no adaptive routing, DESIGN §6.9). flowEntropy
  // mixes MAC SA + MAC DA + shim FlowLabel (per-flow affinity); 0 keeps the
  // legacy XPU-id-only formula bit-identical (single-arg callers and tests).
  uint64_t h;
  if (flowEntropy == 0)
    {
      h = (uint64_t) ((uint32_t) (xpuId ^ m_seed) * 2654435761u);
    }
  else
    {
      // FNV-1a over the XPU-id, the seed, and the 64-bit flow entropy.
      h = 1469598103934665603ull;
      auto mix = [&h] (uint64_t v)
        {
          for (int i = 0; i < 8; ++i)
            {
              h ^= (v >> (8 * i)) & 0xFF;
              h *= 1099511628211ull;
            }
        };
      mix (xpuId ^ m_seed);
      mix (flowEntropy);
    }
  return (int32_t) ports[h % ports.size ()];
}

void SueRoutingTable::Clear () { m_entries.clear (); }
void SueRoutingTable::SetEcmpSeed (uint32_t s) { m_seed = s; }

} // namespace ns3

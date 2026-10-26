/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-error-model.h"
#include "ns3/packet.h"
#include "ns3/log.h"
#include "ns3/type-id.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/rng-stream.h"

#include <memory>

NS_LOG_COMPONENT_DEFINE ("SueErrorModel");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueErrorModel);

TypeId
SueErrorModel::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueErrorModel")
    .SetParent<Object> ()
    .AddConstructor<SueErrorModel> ()
  ;
  return tid;
}

namespace {
// Deterministic RNG stream assignment: build order is fixed (single-threaded
// setup), so stream index -> stream mapping is identical across runs and
// across thread counts (MTP hash equivalence holds for random-corruption
// scenarios seeded identically).
uint64_t g_nextRngStream = 0;
} // namespace

SueErrorModel::SueErrorModel ()
  : m_rngStreamIdx (g_nextRngStream++)
{
}

SueErrorModel::~SueErrorModel () = default;

void
SueErrorModel::SetRngStreamIdx (uint64_t i)
{
  m_rngStreamIdx = i;
  m_rng.reset ();
}

void SueErrorModel::SetBadEvery (uint32_t n) { m_badEvery = n; m_count = 0; }
void SueErrorModel::SetCorruptOnce (uint32_t at) { m_corruptOnce = at; m_count = 0; }
void SueErrorModel::SetCorruptFirstN (uint32_t n) { m_corruptFirstN = n; m_count = 0; }
void SueErrorModel::SetPoisonOnce (uint32_t at) { m_poisonOnce = at; m_count = 0; }
void SueErrorModel::SetCtlDropEvery (uint32_t n) { m_ctlDropEvery = n; m_ctlCount = 0; }
void SueErrorModel::SetCtlDropOnce (uint32_t at) { m_ctlDropOnce = at; m_ctlCount = 0; }

void
SueErrorModel::SetRandomErrorRate (double p)
{
  m_randomRate = (p < 0.0) ? 0.0 : (p > 1.0 ? 1.0 : p);
  m_count = 0;
}

void
SueErrorModel::Reset ()
{
  // One-shot knobs and counters reset; persistent configuration knobs
  // (m_badEvery, m_ctlDropEvery, m_randomRate) survive a Reset.
  m_count = 0;
  m_corruptOnce = 0;
  m_corruptFirstN = 0;
  m_poisonOnce = 0;
  m_ctlDropOnce = 0;
  m_ctlCount = 0;
}

bool
SueErrorModel::CheckPoisonTx ()
{
  ++m_poisonTxCount;
  if (m_poisonTxOnce > 0)
    {
      if (m_poisonTxCount == m_poisonTxOnce) { m_poisonTxOnce = 0; return true; }
      return false;
    }
  if (m_poisonTxEvery == 0) { return false; }
  return (m_poisonTxCount % m_poisonTxEvery) == 0;
}

void SueErrorModel::SetPoisonTxOnce (uint32_t at) { m_poisonTxOnce = at; }
void SueErrorModel::SetPoisonTxEvery (uint32_t n) { m_poisonTxEvery = n; }

bool
SueErrorModel::CheckCtlDrop ()
{
  ++m_ctlCount;
  if (m_ctlDropOnce > 0)
    {
      if (m_ctlCount == m_ctlDropOnce) { m_ctlDropOnce = 0; return true; }
      return false;
    }
  if (m_ctlDropEvery == 0) { return false; }
  return (m_ctlCount % m_ctlDropEvery == 0);
}

SueErrorModel::FcsStatus
SueErrorModel::CheckFcs (Ptr<Packet> p)
{
  (void) p;
  ++m_count;
  if (m_randomRate > 0.0)
    {
      if (!m_rng)
        {
          // Deterministic per (global seed, build-order stream index): same
          // corruption pattern across runs and thread counts.
          m_rng.reset (new RngStream (RngSeedManager::GetSeed (), m_rngStreamIdx, 0));
        }
      return (m_rng->RandU01 () < m_randomRate) ? BAD : GOOD;
    }
  if (m_poisonOnce > 0)
    {
      if (m_count == m_poisonOnce) { m_poisonOnce = 0; return POISONED; }
    }
  if (m_corruptFirstN > 0) { return m_count <= m_corruptFirstN ? BAD : GOOD; }
  if (m_corruptOnce > 0)
    {
      if (m_count == m_corruptOnce) { m_corruptOnce = 0; return BAD; }
      return GOOD;
    }
  if (m_badEvery == 0) { return GOOD; }
  return (m_count % m_badEvery == 0) ? BAD : GOOD;
}

bool
SueErrorModel::IsCorrupt (Ptr<Packet> p)
{
  return CheckFcs (p) == BAD;
}

} // namespace ns3

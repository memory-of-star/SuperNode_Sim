/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE CBFC engine implementation.
 */
#include "sue-cbfc-engine.h"
#include "ns3/log.h"
#include "ns3/type-id.h"
#include <algorithm>

NS_LOG_COMPONENT_DEFINE ("SueCbfcEngine");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueCbfcEngine);

TypeId
SueCbfcEngine::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueCbfcEngine")
    .SetParent<Object> ()
    .AddConstructor<SueCbfcEngine> ()
  ;
  return tid;
}

SueCbfcEngine::SueCbfcEngine () { Init (); }

void SueCbfcEngine::SetCreditSize (uint32_t b) { b = b ? b : 1; m_txCellSize = m_rxCellSize = b; }
void SueCbfcEngine::SetTxCellSize (uint32_t b) { m_txCellSize = b ? b : 1; }
void SueCbfcEngine::SetRxCellSize (uint32_t b) { m_rxCellSize = b ? b : 1; }
void SueCbfcEngine::SetPktOvhd (int32_t b) { m_pktOvhd = b; }
void SueCbfcEngine::SetVcCreditLimit (uint32_t vc, uint32_t limit) { if (vc < NVC) { S_VC_CL[vc] = limit; } }

void
SueCbfcEngine::SetTxCellChoices (const std::vector<uint32_t> &choices)
{
  m_txChoices = choices;
  std::sort (m_txChoices.begin (), m_txChoices.end ());
  m_txChoices.erase (std::unique (m_txChoices.begin (), m_txChoices.end ()),
                     m_txChoices.end ());
}

void
SueCbfcEngine::AdoptTxCellFromPeerRx (uint32_t peerRxCell)
{
  if (peerRxCell == 0) { return; }   // stale/absent TLV: keep the current cell
  if (m_txChoices.empty ())
    {
      m_txCellSize = peerRxCell;     // legacy behavior: adopt verbatim
      return;
    }
  uint32_t best = 0;
  for (uint32_t c : m_txChoices)
    {
      if (c <= peerRxCell && c > best) { best = c; }
    }
  if (best == 0)
    {
      // No supported cell <= the receiver's native cell: the §5.2.10.1
      // conservative direction cannot be honored. Take the smallest supported
      // cell and warn — the receiver will see CU as an UNDERestimate and the
      // §5.2.5.7.4 LostCredits<0 severe error will surface on the first
      // CC_Update. Configuration must fix the choice set.
      best = m_txChoices.front ();
      NS_LOG_WARN ("AdoptTxCellFromPeerRx: no supported cell <= peer rx cell "
                   << peerRxCell << "; using " << best
                   << " (Sender.CreditSize > Receiver.CreditSize violates the "
                   "conservative direction, UEC §5.2.10.1)");
    }
  m_txCellSize = best;
}

void
SueCbfcEngine::Init ()
{
  for (uint32_t i = 0; i < NVC; ++i)
    {
      S_VC_CC[i] = S_VC_CF[i] = 0;
      R_VC_CC[i] = R_VC_CF[i] = 0;
      m_vcLosslessActive[i] = true;   // direct-config default (see header note)
    }
  m_inLlrWindow = false;
}

void
SueCbfcEngine::SetVcLosslessActive (uint32_t vc, bool on)
{
  if (vc < NVC) { m_vcLosslessActive[vc] = on; }
}

bool
SueCbfcEngine::GetVcLosslessActive (uint32_t vc) const
{
  return vc < NVC ? m_vcLosslessActive[vc] : false;
}

uint32_t
SueCbfcEngine::PktCredits (uint32_t macFrameBytes) const
{
  int64_t b = (int64_t) macFrameBytes + m_pktOvhd;
  if (b < 0) { b = 0; }
  uint32_t c = (uint32_t) ((b + m_txCellSize - 1) / m_txCellSize);
  return c ? c : 1;   // a packet always consumes >= 1 credit
}

uint32_t
SueCbfcEngine::RxPktCredits (uint32_t macFrameBytes) const
{
  int64_t b = (int64_t) macFrameBytes + m_pktOvhd;
  if (b < 0) { b = 0; }
  uint32_t c = (uint32_t) ((b + m_rxCellSize - 1) / m_rxCellSize);
  return c ? c : 1;   // a sub-cell frame still occupies one cell (§5.2.5.8)
}

bool
SueCbfcEngine::CanSend (uint32_t vc, uint32_t macFrameBytes) const
{
  // §5.2.8.4 gate (CBFC-SP-13): a VC whose RTS/RTR handshake has not completed
  // must not use credits. Defaults active (direct config); the LLDP path
  // drives it through SetVcLosslessActive.
  if (vc >= NVC || !m_vcLosslessActive[vc] || S_VC_CL[vc] == 0) { return false; }
  // Cyclic-difference mod CF_MOD: the receiver's R_VC_CF (and the adopted
  // S_VC_CF) wrap at 2^15, so the difference must be taken mod 2^15 for CC and
  // CF to wrap at the SAME point (UEC 15-bit cyclic CF). CU only holds the
  // in-flight credit (< credit window << 2^15), so this is unambiguous no matter
  // how many times the cumulative counters wrap. Using CC_MOD (2^20) made the
  // sender adopt a wrapped CF -> CU ~ 2^20-δ >> CL -> permanent credit block.
  uint32_t cu = (S_VC_CC[vc] - S_VC_CF[vc]) % CF_MOD;
  uint32_t pkt = PktCredits (macFrameBytes);
  bool ok = cu + pkt <= S_VC_CL[vc];
  if (!ok) { const_cast<SueCbfcEngine *> (this)->IncCreditExhausted (vc); }   // debug counter
  return ok;
}

void
SueCbfcEngine::OnTx (uint32_t vc, uint32_t macFrameBytes)
{
  if (vc >= NVC) { return; }
  // §5.2.9 rule 1 needs no window gate here: replay frames are never counted
  // through OnTx (the retransmit path skips it structurally, and CanTx blocks
  // new TX while the LLR engine is in REPLAY). Gating OnTx on the window would
  // wrongly skip type-B (LLR-ineligible, CBFC-lossless) frames sent while the
  // RX discard window is open on this very device (full-duplex independence).
  S_VC_CC[vc] = (S_VC_CC[vc] + PktCredits (macFrameBytes)) % CC_MOD;
}

void
SueCbfcEngine::OnCfUpdate (uint32_t vc, uint32_t rVcCf)
{
  if (vc >= NVC) { return; }
  // Receiver sends cumulative R_VC_CF; sender adopts it (S_VC_CU recomputed from CC-CF).
  // Absolute-write is correct for in-order cumulative CF. Out-of-order / stale
  // CF_Update handling is deferred to the on-wire refinement (DESIGN.md §6.3).
  S_VC_CF[vc] = rVcCf % CF_MOD;
}

uint32_t SueCbfcEngine::GetSvcCc (uint32_t vc) const { return vc < NVC ? S_VC_CC[vc] : 0; }
uint32_t SueCbfcEngine::GetSvcCu (uint32_t vc) const
{
  // Same cyclic-difference modulus as CanSend (CF_MOD), see comment there.
  return vc < NVC ? ((S_VC_CC[vc] - S_VC_CF[vc]) % CF_MOD) : 0;
}

void
SueCbfcEngine::OnRx (uint32_t vc, uint32_t macFrameBytes)
{
  if (vc >= NVC) { return; }
  // §5.2.9 rule 2 needs no window gate here: LLR-dropped frames return before
  // OnRx in the device RX path (structural). Gating OnRx on the window would
  // wrongly skip (a) the accepted retransmit that ENDS the discard window —
  // it is new data on the wire and must be counted — and (b) type-B
  // (LLR-ineligible, CBFC-lossless) frames per UEC Table 5-26.
  R_VC_CC[vc] = (R_VC_CC[vc] + RxPktCredits (macFrameBytes)) % CC_MOD;
}

void
SueCbfcEngine::OnDrain (uint32_t vc, uint32_t macFrameBytes)
{
  if (vc >= NVC) { return; }
  R_VC_CF[vc] = (R_VC_CF[vc] + RxPktCredits (macFrameBytes)) % CF_MOD;
}

uint32_t SueCbfcEngine::GetRvcCf (uint32_t vc) const { return vc < NVC ? R_VC_CF[vc] : 0; }

void
SueCbfcEngine::OnCcUpdate (const std::array<uint32_t, 16> &cc)
{
  // §5.2.9 rule 3: CC_Update received inside an LLR window must be discarded
  // (frames re-credited via replay are not a CC-credit leak).
  if (m_inLlrWindow) { return; }
  // §5.2.5.7.4: recover leaked credits (packet drops) via sender's CC snapshot.
  for (uint32_t vc = 0; vc < NVC; ++vc)
    {
      uint32_t lost = (cc[vc] - R_VC_CC[vc]) % CC_MOD;
      // UEC §5.2.4.2 cyclic comparison: only a difference below half the CC
      // range is a real (positive) credit loss. lost >= CC_MOD/2 means
      // R_VC_CC > S_VC_CC — the "LostCredits < 0" severe error of UEC
      // §5.2.5.7.4 (SHOULD report to the management entity), e.g. a CC_Update
      // processed inside an LLR window. Folding the ~2^20 wraparound value
      // into R_VC_CF would poison the count (and the sender's CU); resync only.
      if (lost > 0 && lost < CC_MOD / 2)
        {
          R_VC_CF[vc] = (R_VC_CF[vc] + lost) % CF_MOD;   // free the lost credits
          IncCcLostRecovered (vc);
        }
      else if (lost != 0)
        {
          NS_LOG_WARN ("OnCcUpdate vc=" << vc << ": LostCredits<0 (CC " << cc[vc]
                                        << " < R_VC_CC " << R_VC_CC[vc] << "); resync only");
        }
      R_VC_CC[vc] = cc[vc] % CC_MOD;
    }
}

void SueCbfcEngine::SuspendForLlrWindow (bool on) { m_inLlrWindow = on; }

} // namespace ns3

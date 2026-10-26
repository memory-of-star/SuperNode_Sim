/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE CBFC engine (DESIGN.md §6.3, UEC §5.2). Per-VC credit-based flow control.
 *
 * Sender side: S_VC_CC (consumed), S_VC_CF (freed), S_VC_CL (limit),
 *   S_VC_CU = CC - CF (in-use). CanSend iff CU + pktCredits <= CL.
 * Receiver side: R_VC_CC, R_VC_CF. OnRx bumps R_CC; OnDrain bumps R_CF.
 *   CF_Update carries R_CF to sender; CC_Update recovers leaked credits.
 *
 * S2a: counter logic + CF/CC_Update processing. Cyclic mod (2^20 CC / 2^15 CF)
 * applied; for S2 test-scale values do not wrap. LLR coexist hooks present
 * (inactive until S3).
 */
#ifndef SUE_CBFC_ENGINE_H
#define SUE_CBFC_ENGINE_H

#include "ns3/object.h"
#include "sue-cbfc-counters.h"
#include <cstdint>
#include <array>
#include <vector>

namespace ns3 {

class SueCbfcEngine : public Object
{
public:
  static const uint32_t NVC = 8;
  static const uint32_t CC_MOD = 1u << 20;   // 20-bit CC (CC_Update wire field)
  static const uint32_t CF_MOD = 1u << 15;   // 15-bit CF (CF_Update wire field, UEC)
  // The credit DIFFERENCE (CU = CC - CF) is computed mod CF_MOD (the narrower,
  // wire-CF modulus) so the cyclic counters wrap at the SAME point on both
  // ends. CU only ever holds the in-flight credit (< credit window << 2^15), so
  // mod 2^15 is unambiguous regardless of how many times the cumulative CC/CF
  // wrap during an unbounded transfer. Using CC_MOD (2^20) here let the 15-bit
  // CF wrap first and the sender adopt a wrapped refund -> CU ~ 2^20-δ >>
  // credit limit -> permanent block after ~8.4MB (S5c 400G). CC itself stays
  // mod 2^20 for the CC_Update leak-recovery accounting (its own wire field).

  static TypeId GetTypeId (void);
  SueCbfcEngine ();

  // --- config (direct per DESIGN.md §6.3; LLDP negotiation applies the peer's
  // advertised values via SueNetDevice::OnRemoteLldpTlv, §5.3/§5.2.8.4) ---
  // Dual granularity (UEC §5.2.5.8 / §5.2.8.2): the SENDER charges credits at
  // m_txCellSize; the RECEIVER consumes/frees at its native m_rxCellSize.
  // tx == rx degenerates to the legacy single-cell engine (identical
  // arithmetic). tx < rx (sender snapped to a smaller supported cell, e.g. 32
  // vs the receiver's native 56) leaks ceil(F/tx)-ceil(F/rx) per frame from
  // the sender's CU; the periodic CC_Update reconciliation (OnCcUpdate:
  // LostCredits = S_CC - R_CC folded into R_VC_CF) recovers it — the "count at
  // native cell, reconcile via CC_Update" method of UEC §5.2.5.8. tx <= rx is
  // the HARD conservative direction (§5.2.10.1): CU then overestimates the
  // receiver occupancy, so the sender under-sends but never overruns.
  void SetCreditSize (uint32_t bytes);              // legacy: sets BOTH cells
  void SetTxCellSize (uint32_t bytes);              // sender-side granularity
  void SetRxCellSize (uint32_t bytes);              // receiver-side native cell
  void SetPktOvhd (int32_t bytes);
  void SetVcCreditLimit (uint32_t vc, uint32_t limit);
  uint32_t GetVcCreditLimit (uint32_t vc) const { return vc < NVC ? S_VC_CL[vc] : 0; }
  uint32_t GetSvcCreditLimit (uint32_t vc) const { return vc < NVC ? S_VC_CL[vc] : 0; }
  /** Receiver property: the native RX cell. This is what LLDP advertises (the
   *  receiver's buffer granularity, §5.2.8.2) and what the auto egress buffer
   *  sizes against. == tx under the legacy single-cell config. */
  uint32_t GetCreditSize () const { return m_rxCellSize; }
  uint32_t GetTxCellSize () const { return m_txCellSize; }
  uint32_t GetRxCellSize () const { return m_rxCellSize; }
  /** Supported TX cell sizes, ascending (UEC §5.2.10.1 minimal compliant set
   *  = powers of two 32..2048). Empty (default) = any value adoptable. */
  void SetTxCellChoices (const std::vector<uint32_t> &choices);
  const std::vector<uint32_t> &GetTxCellChoices () const { return m_txChoices; }
  /** §5.2.8.2 sender rule: adopt the receiver's advertised native cell as the
   *  TX granularity, snapped DOWN to the largest supported choice (<= peer).
   *  Empty choice set adopts verbatim (legacy behavior). Writes ONLY
   *  m_txCellSize — the local rx cell is this receiver's own buffer property
   *  and is never modified by adoption. */
  void AdoptTxCellFromPeerRx (uint32_t peerRxCell);
  int32_t GetPktOvhd () const { return m_pktOvhd; }
  void Init ();   // reset counters, ready to send/receive
  /** §5.2.8.4 lossless-VC gate (CBFC-SP-13): only RTS/RTR-complete VCs may use
   *  credits. Init() defaults every VC to active so the direct-config path
   *  (LLDP_NEGOTIATE=0) is unchanged; the LLDP path switches VCs off in
   *  StartLldpExchange and back on as the handshake completes. Note:
   *  SetVcCreditLimit deliberately does NOT touch this bit — the LLDP first
   *  frame adopts remote limits mid-handshake and must not pre-activate VCs. */
  void SetVcLosslessActive (uint32_t vc, bool on);
  bool GetVcLosslessActive (uint32_t vc) const;

  // --- sender ---
  /** Credit units for a MAC-frame of `macFrameBytes` (excl preamble) at the
   *  TX granularity. A packet always consumes >= 1 credit. */
  uint32_t PktCredits (uint32_t macFrameBytes) const;
  /** Same, at the RX (native cell) granularity — the units OnRx/OnDrain count
   *  in. A sub-cell frame still occupies one credit (UEC §5.2.5.8). */
  uint32_t RxPktCredits (uint32_t macFrameBytes) const;
  bool CanSend (uint32_t vc, uint32_t macFrameBytes) const;
  void OnTx (uint32_t vc, uint32_t macFrameBytes);
  /** Receiver's R_VC_CF arrived via CF_Update -> reclaim credits. */
  void OnCfUpdate (uint32_t vc, uint32_t rVcCf);
  /** Sender's S_VC_CC (for CC_Update generation). */
  uint32_t GetSvcCc (uint32_t vc) const;
  /** Sender's adopted S_VC_CF (test/observe; rule-4 window checks). */
  uint32_t GetSvcCf (uint32_t vc) const { return vc < NVC ? S_VC_CF[vc] : 0; }
  uint32_t GetSvcCu (uint32_t vc) const;     // in-use (for test/observe)

  // --- counters (§6.11) ---
  const SueCbfcCounters &GetCounters () const { return m_cnt; }
  void IncCfUpdateTx () { ++m_cnt.cf_update_tx; }
  void IncCfUpdateRx () { ++m_cnt.cf_update_rx; }
  void IncCcUpdateTx () { ++m_cnt.cc_update_tx; }
  void IncCcUpdateRx () { ++m_cnt.cc_update_rx; }
  void IncCreditExhausted (uint32_t vc) { if (vc < NVC) { ++m_cnt.credit_exhausted[vc]; } }
  void IncCcLostRecovered (uint32_t vc) { if (vc < NVC) { ++m_cnt.cc_lost_credits_recovered[vc]; } }

  // --- receiver ---
  void OnRx (uint32_t vc, uint32_t macFrameBytes);
  void OnDrain (uint32_t vc, uint32_t macFrameBytes);
  uint32_t GetRvcCf (uint32_t vc) const;     // for CF_Update generation
  uint32_t GetRvcCc (uint32_t vc) const { return vc < NVC ? R_VC_CC[vc] : 0; }   // test observe
  /** CC_Update from sender: recover leaked credits. */
  void OnCcUpdate (const std::array<uint32_t, 16> &cc);

  // --- LLR coexist (§5.2.9): suspend counter updates during LLR windows ---
  void SuspendForLlrWindow (bool on);
  bool GetInLlrWindow () const { return m_inLlrWindow; }

private:
  uint32_t m_txCellSize{256};              // sender-side credit granularity
  uint32_t m_rxCellSize{256};              // receiver-side native cell
  std::vector<uint32_t> m_txChoices;       // supported TX cells (asc); empty = any
  int32_t m_pktOvhd{0};
  uint32_t S_VC_CC[NVC]{}, S_VC_CF[NVC]{}, S_VC_CL[NVC]{};
  uint32_t R_VC_CC[NVC]{}, R_VC_CF[NVC]{};
  bool m_vcLosslessActive[NVC]{};
  SueCbfcCounters m_cnt;
  bool m_inLlrWindow{false};
};

} // namespace ns3

#endif /* SUE_CBFC_ENGINE_H */

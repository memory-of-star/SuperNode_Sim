/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE link error model (DESIGN.md §10.1). Decides per-frame FCS-bad.
 * Bad frames are *delivered* to RX (not silently dropped at the channel) so
 * the LLR RX can NACK / trigger replay. Deterministic SetBadEvery(N) for
 * reproducible tests; SetRandomErrorRate(p) is the probabilistic mode
 * (PLAT-SP-08, ns-3 global RNG stream — deterministic per run/seed).
 * Control frames (CtlOS / CF_Update / CC_Update / PFC) have their own drop
 * knobs (LLR-SP-22): losing them exercises the UEC tolerance clauses
 * (replay-timer fallback for lost NACK/ACK, init-timeout retry for lost
 * INIT/INIT_ECHO).
 */
#ifndef SUE_ERROR_MODEL_H
#define SUE_ERROR_MODEL_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include <cstdint>
#include <memory>

namespace ns3 {

class Packet;
class RngStream;

class SueErrorModel : public Object
{
public:
  static TypeId GetTypeId (void);
  SueErrorModel ();
  ~SueErrorModel () override;   // complete RngStream for unique_ptr

  /** FCS status (UEC §5.1.2). POISONED: accepted (seq++), no NACK, frame_bad reported. */
  enum FcsStatus { GOOD, POISONED, BAD };

  /** Corrupt every N-th frame (1=none; 2=every 2nd; ...). 0 disables. */
  void SetBadEvery (uint32_t n);
  /** Corrupt only the frame at counter `at`, once (for retransmit tests). */
  void SetCorruptOnce (uint32_t at);
  /** Corrupt the first N frames, then stop (for FLUSH recovery tests). */
  void SetCorruptFirstN (uint32_t n);
  /** Poison the frame at counter `at`, once (POISONED: accepted, no NACK). */
  void SetPoisonOnce (uint32_t at);
  /** Probabilistic per-frame FCS corruption (PLAT-SP-08): each data frame is
   *  BAD with probability p. Uses a dedicated ns-3 RNG stream assigned at
   *  construction (build order) — deterministic across runs and thread counts.
   *  0 disables. Overrides the deterministic knobs when > 0. */
  void SetRandomErrorRate (double p);
  /** Explicit RNG stream index (config-driven builds assign per-link indices
   *  so repeated network rebuilds reproduce the same corruption pattern). */
  void SetRngStreamIdx (uint64_t i);

  /** Drop every N-th CONTROL frame arriving at the device (LLR-SP-22).
   *  1 = drop every control frame; 0 disables. */
  void SetCtlDropEvery (uint32_t n);
  /** TX-side FCS poisoning (COUNTER-007 / §5.1.10): the at-th / every n-th
   *  eligible frame this device TRANSMITS gets its FCS poisoned (the poison
   *  pattern XOR 0xAAAAAAAA is implied; the frame carries SueFrameBadTag so
   *  the peer classifies it POISONED — accepted, no NACK, frame_bad). */
  void SetPoisonTxOnce (uint32_t at);
  void SetPoisonTxEvery (uint32_t n);
  bool CheckPoisonTx ();
  /** Drop only the control frame at counter `at`, once. */
  void SetCtlDropOnce (uint32_t at);
  /** Returns true if this control frame is lost (increments ctl counter). */
  bool CheckCtlDrop ();

  /** Returns FCS status for this frame (increments internal counter). */
  FcsStatus CheckFcs (Ptr<Packet> p);
  /** Legacy bool API (BAD=true). */
  bool IsCorrupt (Ptr<Packet> p);
  void Reset ();

private:
  uint32_t m_badEvery{0};
  uint32_t m_corruptOnce{0};
  uint32_t m_corruptFirstN{0};
  uint32_t m_poisonOnce{0};
  uint32_t m_count{0};
  uint32_t m_ctlDropEvery{0};
  uint32_t m_ctlDropOnce{0};
  uint32_t m_ctlCount{0};
  uint32_t m_poisonTxEvery{0};
  uint32_t m_poisonTxOnce{0};
  uint32_t m_poisonTxCount{0};
  double m_randomRate{0.0};
  std::unique_ptr<RngStream> m_rng;
  uint64_t m_rngStreamIdx{0};
};

} // namespace ns3

#endif /* SUE_ERROR_MODEL_H */

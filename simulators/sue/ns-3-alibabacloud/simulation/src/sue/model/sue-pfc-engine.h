/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE PFC engine (DESIGN.md §6.4). Simplified IEEE 802.1Qbb pause/resume.
 * Receiver monitors buffer occupancy; when above pause threshold, sends pause
 * to the sender. When below resume threshold, sends resume.
 * Sender gates TX on m_paused[vc]. Used for PFC vs CBFC comparison (S4b).
 *
 * Threshold algorithm ported from SwitchMmu: dynamic threshold via
 * pfc_a_shift + headroom + resume_offset (independent, no SwitchMmu dependency).
 */
#ifndef SUE_PFC_ENGINE_H
#define SUE_PFC_ENGINE_H

#include "ns3/object.h"
#include "ns3/callback.h"
#include <cstdint>

namespace ns3 {

class SuePfcEngine : public Object
{
public:
  static const uint32_t NVC = 8;
  static TypeId GetTypeId (void);
  SuePfcEngine ();

  // --- config ---
  void SetBufferSize (uint32_t bytes);
  void SetPauseThreshold (uint32_t bytes);     // per-VC simple mode
  void SetResumeThreshold (uint32_t bytes);
  /** Shared-pool mode (SueMmu §6.4): shared + headroom + resume_offset. */
  void EnableSharedPool (uint32_t sharedPoolBytes, uint32_t headroomBytes, uint32_t resumeOffsetBytes, uint32_t alphaShift);
  void Init ();

  // --- receiver ---
  void OnRx (uint32_t vc, uint32_t bytes);
  void OnDrain (uint32_t vc, uint32_t bytes);
  /** Check thresholds -> fire pause/resume callback if state changed. */
  void CheckThresholds ();

  // --- sender ---
  bool CanSend (uint32_t vc) const;
  void OnPause (uint32_t vc);
  void OnResume (uint32_t vc);

  typedef Callback<void, uint32_t, bool> PauseSink;   // (vc, isPause) -> peer
  void SetPauseSink (PauseSink cb);

  // --- observe ---
  uint32_t GetBufferUsed (uint32_t vc) const;
  bool IsPaused (uint32_t vc) const;

private:
  uint32_t Threshold (uint32_t vc) const;

  uint32_t m_bufferSize{65536};
  uint32_t m_pauseThresh{32768};
  uint32_t m_resumeThresh{16384};
  // shared-pool mode
  bool m_sharedPool{false};
  uint32_t m_sharedPoolSize{0};
  uint32_t m_headroom{0};
  uint32_t m_resumeOffset{0};
  uint32_t m_alphaShift{0};
  uint32_t m_sharedUsed{0};
  uint32_t m_bufUsed[NVC]{};
  uint32_t m_headroomUsed[NVC]{};
  bool m_paused[NVC]{};
  bool m_rxPaused[NVC]{};   // receiver-side state (has sent pause)
  PauseSink m_pauseSink;
};

} // namespace ns3

#endif /* SUE_PFC_ENGINE_H */

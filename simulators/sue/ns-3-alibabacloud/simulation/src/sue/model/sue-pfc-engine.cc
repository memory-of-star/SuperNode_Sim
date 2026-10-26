/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-pfc-engine.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SuePfcEngine");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SuePfcEngine);

TypeId
SuePfcEngine::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SuePfcEngine")
    .SetParent<Object> ()
    .AddConstructor<SuePfcEngine> ()
  ;
  return tid;
}

SuePfcEngine::SuePfcEngine () { Init (); }

void SuePfcEngine::SetBufferSize (uint32_t b) { m_bufferSize = b; }
void SuePfcEngine::SetPauseThreshold (uint32_t b) { m_pauseThresh = b; }
void SuePfcEngine::SetResumeThreshold (uint32_t b) { m_resumeThresh = b; }
void SuePfcEngine::EnableSharedPool (uint32_t shared, uint32_t headroom, uint32_t resumeOff, uint32_t alphaShift)
{
  m_sharedPool = true;
  m_sharedPoolSize = shared;
  m_headroom = headroom;
  m_resumeOffset = resumeOff;
  m_alphaShift = alphaShift;
}
void SuePfcEngine::Init ()
{
  for (uint32_t i = 0; i < NVC; ++i) { m_bufUsed[i] = 0; m_headroomUsed[i] = 0; m_paused[i] = false; m_rxPaused[i] = false; }
  m_sharedUsed = 0;
}
void SuePfcEngine::SetPauseSink (PauseSink cb) { m_pauseSink = cb; }

uint32_t
SuePfcEngine::Threshold (uint32_t vc) const
{
  if (!m_sharedPool) { return m_pauseThresh; }
  // Dynamic shared-pool threshold (SwitchMmu pfc_a_shift style):
  // thresh = (shared_pool_size - shared_used) >> alpha_shift
  uint32_t avail = m_sharedPoolSize > m_sharedUsed ? (m_sharedPoolSize - m_sharedUsed) : 0;
  return (avail >> m_alphaShift) + m_headroomUsed[vc];
}

void SuePfcEngine::OnRx (uint32_t vc, uint32_t bytes)
{
  if (vc >= NVC) { return; }
  m_bufUsed[vc] += bytes;
  m_sharedUsed += bytes;
  if (m_sharedPool && m_bufUsed[vc] > m_headroom) { m_headroomUsed[vc] += bytes; }
}

void
SuePfcEngine::OnDrain (uint32_t vc, uint32_t bytes)
{
  if (vc >= NVC) { return; }
  uint32_t freed = bytes;
  if (m_bufUsed[vc] < freed) { freed = m_bufUsed[vc]; }
  m_bufUsed[vc] -= freed;
  m_sharedUsed = m_sharedUsed >= freed ? m_sharedUsed - freed : 0;
  if (m_sharedPool && m_headroomUsed[vc] >= freed) { m_headroomUsed[vc] -= freed; }
  else if (m_sharedPool) { m_headroomUsed[vc] = 0; }
}

void
SuePfcEngine::CheckThresholds ()
{
  for (uint32_t vc = 0; vc < NVC; ++vc)
    {
      uint32_t th = Threshold (vc);
      // SwitchMmu pause condition: !paused && (hdrm > 0 || shared >= thresh).
      // The m_bufUsed > 0 guard keeps idle VCs from spurious-pausing when an
      // exhausted pool drives the dynamic threshold to 0 (0 >= 0 would fire).
      if (!m_rxPaused[vc] && m_bufUsed[vc] > 0
          && (m_headroomUsed[vc] > 0 || m_bufUsed[vc] >= th))
        {
          m_rxPaused[vc] = true;
          if (!m_pauseSink.IsNull ()) { m_pauseSink (vc, true); }
        }
      else if (m_rxPaused[vc])
        {
          // Hysteresis (SwitchMmu resume_offset): pause at bufUsed >= th, resume
          // only at bufUsed <= th - resume_offset (shared-pool mode; clamped at
          // 0). Without the offset the band degenerates to a single line and
          // boundary jitter re-pauses/resumes on every drain. Simple mode keeps
          // its explicit resume threshold.
          uint32_t resumeLine = m_sharedPool
                                  ? (th > m_resumeOffset ? th - m_resumeOffset : 0)
                                  : m_resumeThresh;
          if (m_bufUsed[vc] <= resumeLine)
            {
              m_rxPaused[vc] = false;
              if (!m_pauseSink.IsNull ()) { m_pauseSink (vc, false); }
            }
        }
    }
}

bool SuePfcEngine::CanSend (uint32_t vc) const { return vc < NVC && !m_paused[vc]; }
void SuePfcEngine::OnPause (uint32_t vc) { if (vc < NVC) { m_paused[vc] = true; } }
void SuePfcEngine::OnResume (uint32_t vc) { if (vc < NVC) { m_paused[vc] = false; } }
uint32_t SuePfcEngine::GetBufferUsed (uint32_t vc) const { return vc < NVC ? m_bufUsed[vc] : 0; }
bool SuePfcEngine::IsPaused (uint32_t vc) const { return vc < NVC && m_paused[vc]; }

} // namespace ns3

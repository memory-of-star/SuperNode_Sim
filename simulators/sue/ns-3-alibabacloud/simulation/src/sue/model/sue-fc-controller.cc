/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-fc-controller.h"
#include "sue-cbfc-engine.h"
#include "sue-pfc-engine.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueFcController");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueFcController);

TypeId
SueFcController::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueFcController")
    .SetParent<Object> ()
    .AddConstructor<SueFcController> ()
  ;
  return tid;
}

SueFcController::SueFcController () {}

void SueFcController::SetCbfc (Ptr<SueCbfcEngine> c) { m_cbfc = c; m_mode = CBFC; }
void SueFcController::SetPfc (Ptr<SuePfcEngine> p) { m_pfc = p; m_mode = PFC; }

bool
SueFcController::CanSend (uint32_t vc, uint32_t bytes) const
{
  if (m_mode == CBFC && m_cbfc) { return m_cbfc->CanSend (vc, bytes); }
  if (m_mode == PFC && m_pfc) { return m_pfc->CanSend (vc); }
  return true;   // OFF
}

void
SueFcController::OnTx (uint32_t vc, uint32_t bytes)
{
  if (m_mode == CBFC && m_cbfc) { m_cbfc->OnTx (vc, bytes); }
}

void
SueFcController::OnRx (uint32_t vc, uint32_t bytes)
{
  if (m_mode == CBFC && m_cbfc) { m_cbfc->OnRx (vc, bytes); }
  else if (m_mode == PFC && m_pfc)
    {
      m_pfc->OnRx (vc, bytes);
      m_pfc->CheckThresholds ();   // match device inline path (may fire pause)
    }
}

uint32_t
SueFcController::GetRvcCf (uint32_t vc) const
{
  if (m_mode == CBFC && m_cbfc) { return m_cbfc->GetRvcCf (vc); }
  return 0;   // PFC / OFF: no credit count
}

void
SueFcController::OnDrain (uint32_t vc, uint32_t bytes)
{
  if (m_mode == CBFC && m_cbfc)
    {
      m_cbfc->OnDrain (vc, bytes);
    }
  else if (m_mode == PFC && m_pfc)
    {
      m_pfc->OnDrain (vc, bytes);
      m_pfc->CheckThresholds ();
    }
}

bool
SueFcController::IsPaused (uint32_t vc) const
{
  if (m_mode == PFC && m_pfc) { return m_pfc->IsPaused (vc); }
  return false;
}

void
SueFcController::SuspendForLlrWindow (bool on)
{
  if (m_mode == CBFC && m_cbfc) { m_cbfc->SuspendForLlrWindow (on); }
}

} // namespace ns3

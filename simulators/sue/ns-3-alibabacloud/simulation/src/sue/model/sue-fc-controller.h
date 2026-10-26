/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE flow-control controller (DESIGN.md §6.5). Facade that selects CBFC
 * or PFC (FC_MODE) and dispatches the common flow-control API. The device
 * calls the controller instead of wiring CBFC/PFC engines directly.
 *
 * LLDP negotiation (§5.3/§5.2.8.4, CBFC-SP-13) runs at the device level
 * (SueNetDevice::StartLldpExchange/OnRemoteLldpTlv); the controller's mode is
 * set directly from config.
 */
#ifndef SUE_FC_CONTROLLER_H
#define SUE_FC_CONTROLLER_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "sue-cbfc-engine.h"
#include "sue-pfc-engine.h"

namespace ns3 {

class SueFcController : public Object
{
public:
  enum Mode { OFF, CBFC, PFC };

  static TypeId GetTypeId (void);
  SueFcController ();

  void SetMode (Mode m) { m_mode = m; }
  Mode GetMode () const { return m_mode; }

  void SetCbfc (Ptr<SueCbfcEngine> c);
  void SetPfc (Ptr<SuePfcEngine> p);
  Ptr<SueCbfcEngine> GetCbfc () const { return m_cbfc; }
  Ptr<SuePfcEngine> GetPfc () const { return m_pfc; }

  // Unified API
  bool CanSend (uint32_t vc, uint32_t bytes) const;
  void OnTx (uint32_t vc, uint32_t bytes);
  void OnRx (uint32_t vc, uint32_t bytes);
  void OnDrain (uint32_t vc, uint32_t bytes);
  /** CBFC receiver credit count (PFC mode -> 0). Used to preserve the device's
   *  CBFC-only CF_Update sink on drain. */
  uint32_t GetRvcCf (uint32_t vc) const;
  bool IsPaused (uint32_t vc) const;
  void SuspendForLlrWindow (bool on);

private:
  Mode m_mode{OFF};
  Ptr<SueCbfcEngine> m_cbfc;
  Ptr<SuePfcEngine> m_pfc;
};

} // namespace ns3

#endif /* SUE_FC_CONTROLLER_H */

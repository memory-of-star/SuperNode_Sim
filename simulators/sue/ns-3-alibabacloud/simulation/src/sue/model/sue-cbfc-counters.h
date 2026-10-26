/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE CBFC counters (DESIGN.md §6.11, UEC §5.2.5.3 + debug).
 */
#ifndef SUE_CBFC_COUNTERS_H
#define SUE_CBFC_COUNTERS_H

#include <cstdint>

namespace ns3 {

struct SueCbfcCounters
{
  uint64_t cf_update_tx{0};     // CF_Update CtlOS sent
  uint64_t cf_update_rx{0};     // CF_Update CtlOS received
  uint64_t cc_update_tx{0};     // CC_Update frames sent
  uint64_t cc_update_rx{0};     // CC_Update frames received
  uint64_t credit_exhausted[8]{};            // debug: VC hit credit limit (CanSend false)
  uint64_t cc_lost_credits_recovered[8]{};   // debug: leaked credits recovered via CC_Update

  void Reset ();
};

} // namespace ns3

#endif /* SUE_CBFC_COUNTERS_H */

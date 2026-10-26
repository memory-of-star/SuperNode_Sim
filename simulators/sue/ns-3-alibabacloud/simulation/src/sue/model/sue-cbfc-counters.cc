/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-cbfc-counters.h"

namespace ns3 {

void
SueCbfcCounters::Reset ()
{
  cf_update_tx = cf_update_rx = cc_update_tx = cc_update_rx = 0;
  for (uint32_t i = 0; i < 8; ++i) { credit_exhausted[i] = cc_lost_credits_recovered[i] = 0; }
}

} // namespace ns3

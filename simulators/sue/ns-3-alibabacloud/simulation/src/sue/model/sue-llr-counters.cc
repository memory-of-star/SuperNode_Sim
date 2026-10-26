/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-llr-counters.h"

namespace ns3 {

void
SueLlrCounters::Reset ()
{
  tx_init_ctl_os = tx_init_echo_ctl_os = tx_ack_ctl_os = tx_nack_ctl_os = 0;
  tx_discard = tx_ok = tx_poisoned = tx_replay = 0;
  rx_init_ctl_os = rx_init_echo_ctl_os = rx_ack_ctl_os = rx_nack_ctl_os = 0;
  rx_ack_nack_seq_error = rx_ok = rx_poisoned = rx_bad = 0;
  rx_expected_seq_good = rx_expected_seq_poisoned = rx_expected_seq_bad = 0;
  rx_missing_seq = rx_duplicate_seq = rx_replay = 0;
}

} // namespace ns3

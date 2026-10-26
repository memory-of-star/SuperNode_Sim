/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE LLR counters (UEC §5.1.11 Table 5-13, 22 items).
 */
#ifndef SUE_LLR_COUNTERS_H
#define SUE_LLR_COUNTERS_H

#include <cstdint>

namespace ns3 {

struct SueLlrCounters
{
  // TX side
  uint64_t tx_init_ctl_os{0};        // LLR_INIT sent
  uint64_t tx_init_echo_ctl_os{0};   // LLR_INIT_ECHO sent
  uint64_t tx_ack_ctl_os{0};         // LLR_ACK sent
  uint64_t tx_nack_ctl_os{0};        // LLR_NACK sent
  uint64_t tx_discard{0};            // FLUSH/INIT discard
  uint64_t tx_ok{0};                 // eligible frames TX with good FCS
  uint64_t tx_poisoned{0};           // eligible frames TX with poisoned FCS
  uint64_t tx_replay{0};             // REPLAY state exits (completed replays, UEC Table 5-13)
  uint64_t tx_replay_frames{0};      // debug: frames retransmitted across all rounds

  // RX side
  uint64_t rx_init_ctl_os{0};        // LLR_INIT received
  uint64_t rx_init_echo_ctl_os{0};   // LLR_INIT_ECHO received
  uint64_t rx_ack_ctl_os{0};         // LLR_ACK received
  uint64_t rx_nack_ctl_os{0};        // LLR_NACK received
  uint64_t rx_ack_nack_seq_error{0}; // ACK/NACK seq validation failure
  uint64_t rx_ok{0};                 // eligible frames RX with good FCS
  uint64_t rx_poisoned{0};           // eligible frames RX with poisoned FCS
  uint64_t rx_bad{0};                // eligible frames RX with bad FCS
  uint64_t rx_expected_seq_good{0};  // expected seq + good FCS
  uint64_t rx_expected_seq_poisoned{0}; // expected seq + poisoned FCS
  uint64_t rx_expected_seq_bad{0};   // expected seq + bad FCS
  uint64_t rx_missing_seq{0};        // gap detected
  uint64_t rx_duplicate_seq{0};      // duplicate seq
  uint64_t rx_replay{0};             // replay detected at receiver

  void Reset ();
};

} // namespace ns3

#endif /* SUE_LLR_COUNTERS_H */

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S3e POISONED + counters: A sends 3 frames; frame 2 (seq=1) is POISONED
 * (not BAD). POISONED frame is accepted (seq++), no NACK, no retransmit.
 * Frame 3 (seq=2) delivered normally. Verifies §5.1.10 + 22 counter values.
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-error-model.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include <iostream>

using namespace ns3;

static uint32_t g_rx = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_rx; }

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)


int
main ()
{
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);
  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps"));

  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> ();
  Ptr<SueLlrEngine> eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  eA->SetReplayTimerMax (MicroSeconds (50));
  eA->Start (); eB->Start ();   // wire engines before Start -> real INIT handshake


  // Poison frame 2 (seq=1): POISONED, not BAD
  Ptr<SueErrorModel> em = CreateObject<SueErrorModel> ();
  em->SetPoisonOnce (2);
  B->SetErrorModel (em);
  B->SetReceiveUpcall (MakeCallback (&RxObs));

  auto build = [] () {
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  for (int i = 0; i < 3; ++i) { A->SwitchSend (0, build ()); }
  Simulator::Run ();

  std::cout << "[S3e] POISONED + counters (3 frames, seq=1 poisoned)\n";
  CHECK (g_rx == 3, "all 3 delivered (poisoned accepted, got " << g_rx << ")");

  // B counters (receiver)
  const SueLlrCounters &bc = eB->GetCounters ();
  CHECK (bc.rx_ok == 2, "rx_ok=2 (seq 0,2 good; got " << bc.rx_ok << ")");
  CHECK (bc.rx_poisoned == 1, "rx_poisoned=1 (seq=1; got " << bc.rx_poisoned << ")");
  CHECK (bc.rx_bad == 0, "rx_bad=0 (no BAD frames)");
  CHECK (bc.rx_expected_seq_good == 2, "rx_expected_seq_good=2");
  CHECK (bc.rx_expected_seq_poisoned == 1, "rx_expected_seq_poisoned=1");
  CHECK (bc.rx_expected_seq_bad == 0, "rx_expected_seq_bad=0");
  CHECK (bc.rx_missing_seq == 0, "rx_missing_seq=0 (no gaps)");
  CHECK (bc.rx_duplicate_seq == 0, "rx_duplicate_seq=0");
  CHECK (bc.tx_ack_ctl_os == 3, "B tx_ack_ctl_os=3 (all 3 ACKed, incl poisoned)");
  CHECK (bc.tx_nack_ctl_os == 0, "B tx_nack_ctl_os=0 (no NACK for poisoned)");
  CHECK (eB->GetNextRxSeq () == 3, "B next_rx_seq=3 (poisoned advanced seq)");

  // A counters (sender)
  const SueLlrCounters &ac = eA->GetCounters ();
  CHECK (ac.tx_ok == 3, "tx_ok=3 (all stored)");
  CHECK (ac.tx_replay == 0, "tx_replay=0 (no retransmit for poisoned)");
  CHECK (ac.tx_nack_ctl_os == 0, "A rx_nack_ctl_os=0 (no NACK received)");
CHECK (ac.rx_ack_ctl_os == 3, "A rx_ack_ctl_os=3 (3 ACKs received)");
  CHECK (ac.tx_discard == 0, "tx_discard=0 (no FLUSH)");

  Simulator::Destroy ();
  std::cout << (g_ok ? "LLR POISONED PASS" : "LLR POISONED FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

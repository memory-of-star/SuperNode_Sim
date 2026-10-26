/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S5 pcs_lost_status_timer (UEC Table 5-10): link-glitch (闪断) impact on LLR.
 *
 * Minimal PCS model (no PCS sublayer): the scenario toggles BOTH link ends'
 * PCS_status FALSE for a glitch duration via SetPcsStatus (Simulator::Schedule);
 * while FALSE the devices drop every incoming frame (undecodable) and each
 * engine's pcs_lost_status_timer runs in ADVANCE/REPLAY only.
 *
 *   - short glitch (< pcs_lost_status_timer_max): PCS returns first, the timer
 *     is cancelled, the dropped frame is recovered by replay — no FLUSH.
 *   - long glitch (>= max): the timer expires -> FLUSH discards the in-flight
 *     replay buffer (even though replay_ct_max was not reached), then re-INIT
 *     re-syncs the receiver so post-glitch frames are delivered.
 *   - idle link + long glitch: FLUSH even with an EMPTY replay buffer — the
 *     distinguishing behavior (replay_ct_max only counts replay rounds).
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
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

static Ptr<Packet> BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

// Glitch the link: both ends' PCS FALSE for `dur` starting at `start`.
static void
ScheduleGlitch (Ptr<SueNetDevice> a, Ptr<SueNetDevice> b, Time start, Time dur)
{
  Simulator::Schedule (start, &SueNetDevice::SetPcsStatus, a, false);
  Simulator::Schedule (start, &SueNetDevice::SetPcsStatus, b, false);
  Simulator::Schedule (start + dur, &SueNetDevice::SetPcsStatus, a, true);
  Simulator::Schedule (start + dur, &SueNetDevice::SetPcsStatus, b, true);
}

// Expired-flag observed mid-glitch (it latches TRUE until PCS recovers).
static bool g_pcsExpiredObs = false;

int
main ()
{
  std::cout << "[S5] pcs_lost_status_timer: link-glitch (闪断) impact on LLR\n";

  // ============ scenario 1: short glitch (< max) recovers via replay ============
  std::cout << "  scenario 1: glitch 30us, pcs_lost_max 100us (no FLUSH)\n";
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->SetPcsLostTimerMax (MicroSeconds (100));
    eB->SetPcsLostTimerMax (MicroSeconds (100));   // both ends, or B FLUSHes at 0
    eA->Start (); eB->Start ();   // real INIT handshake

    B->SetReceiveUpcall (MakeCallback (&RxObs));
    g_rx = 0;
    // frames 1,2 delivered + acked before the glitch
    A->SwitchSend (0, BuildFrame ());
    A->SwitchSend (0, BuildFrame ());
    // glitch [100us, 130us]; frame 3 sent at the glitch start -> dropped once,
    // then recovered by replay after PCS returns (retransmit at ~150us).
    ScheduleGlitch (A, B, MicroSeconds (100), MicroSeconds (30));
    Simulator::Schedule (MicroSeconds (100), &SueNetDevice::SwitchSend, A, 0, BuildFrame ());
    Simulator::Stop (MicroSeconds (300));
    Simulator::Run ();

    CHECK (eA->GetPcsLostTimerExpired () == false, "pcs_lost timer NOT expired");
    CHECK (eA->GetCntTxDiscard () == 0, "no FLUSH / no discard (got " << eA->GetCntTxDiscard () << ")");
    CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "state ADVANCE (recovered)");
    CHECK (g_rx == 3, "all 3 frames delivered (got " << g_rx << ")");
    CHECK (eA->GetCounters ().tx_replay >= 1, "replay recovered the glitch-dropped frame");
    Simulator::Destroy ();
  }

  // ============ scenario 2: long glitch (>= max) -> pcs_lost FLUSH ============
  std::cout << "  scenario 2: glitch 250us, pcs_lost_max 100us (FLUSH + re-INIT)\n";
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetReplayTimerMax (MicroSeconds (50));
    eA->SetPcsLostTimerMax (MicroSeconds (100));
    eA->SetInitTimerMax (MicroSeconds (50));   // INIT retries land post-glitch
    eA->SetInitRetryMax (3);
    eB->SetPcsLostTimerMax (MicroSeconds (100));
    eB->SetInitTimerMax (MicroSeconds (50));   // B re-syncs at the same cadence
    eB->SetInitRetryMax (3);
    eA->Start (); eB->Start ();

    B->SetReceiveUpcall (MakeCallback (&RxObs));
    g_rx = 0;
    // frames 1,2 delivered + acked before the glitch
    A->SwitchSend (0, BuildFrame ());
    A->SwitchSend (0, BuildFrame ());
    // glitch [100us, 350us]; frame 3 (seq 2) is in flight when it starts and is
    // dropped; the pcs_lost timer expires at 200us -> FLUSH discards it
    // (tx_discard=1) and A re-INITs with init_seq = tx_seq = 3; the last INIT
    // lands just after PCS returns -> B re-syncs next_rx_seq to 3.
    ScheduleGlitch (A, B, MicroSeconds (100), MicroSeconds (250));
    Simulator::Schedule (MicroSeconds (100), &SueNetDevice::SwitchSend, A, 0, BuildFrame ());
    // frames 4,5 (seq 3,4) sent after recovery -> delivered
    Simulator::Schedule (MicroSeconds (400), &SueNetDevice::SwitchSend, A, 0, BuildFrame ());
    Simulator::Schedule (MicroSeconds (401), &SueNetDevice::SwitchSend, A, 0, BuildFrame ());
    g_pcsExpiredObs = false;
    Simulator::Schedule (MicroSeconds (300), [eA] () { g_pcsExpiredObs = eA->GetPcsLostTimerExpired (); });
    Simulator::Stop (MicroSeconds (600));
    Simulator::Run ();

    CHECK (g_pcsExpiredObs == true, "pcs_lost timer expired during the glitch");
    CHECK (eA->GetCntTxDiscard () == 1, "1 in-flight frame FLUSHed (got " << eA->GetCntTxDiscard () << ")");
    CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "state ADVANCE (recovered via re-INIT)");
    CHECK (eB->GetNextRxSeq () == 5, "B next_rx_seq=5 (re-synced via INIT, got " << eB->GetNextRxSeq () << ")");
    CHECK (g_rx == 4, "frames 1,2,4,5 delivered; frame 3 discarded by FLUSH (got " << g_rx << ")");
    Simulator::Destroy ();
  }

  // ============ scenario 3: idle link + long glitch -> empty-buffer FLUSH ============
  std::cout << "  scenario 3: idle link, glitch 250us, pcs_lost_max 100us (empty-buffer FLUSH)\n";
  {
    Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
    Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
    n1->AddDevice (A); n2->AddDevice (B);
    Ptr<SueChannel> ch = CreateObject<SueChannel> ();
    A->Attach (ch); B->Attach (ch);
    A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
    A->SetEgressQueue (CreateObject<SueEgressQueue> ());
    B->SetDataRate (DataRate ("100Gbps"));
    Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
    A->SetLlrEngine (eA); B->SetLlrEngine (eB);
    eA->SetPcsLostTimerMax (MicroSeconds (100));
    eA->SetInitTimerMax (MicroSeconds (50));
    eA->SetInitRetryMax (3);
    eB->SetPcsLostTimerMax (MicroSeconds (100));
    eB->SetInitTimerMax (MicroSeconds (50));   // B re-syncs at the same cadence
    eB->SetInitRetryMax (3);
    eA->Start (); eB->Start ();
    g_rx = 0;
    // no traffic at all: the idle link glitches [100us, 350us]
    ScheduleGlitch (A, B, MicroSeconds (100), MicroSeconds (250));
    g_pcsExpiredObs = false;
    Simulator::Schedule (MicroSeconds (300), [eA] () { g_pcsExpiredObs = eA->GetPcsLostTimerExpired (); });
    Simulator::Stop (MicroSeconds (600));
    Simulator::Run ();

    // replay_ct_max (default 5) never fires with no frames, yet pcs_lost does:
    // the timer expiry alone forces FLUSH -> re-INIT even with a 0-frame buffer.
    CHECK (g_pcsExpiredObs == true, "pcs_lost timer expired on the idle link");
    CHECK (eA->GetCntTxDiscard () == 0, "empty buffer: 0 frames discarded (got " << eA->GetCntTxDiscard () << ")");
    CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "state ADVANCE (re-INIT'd after recovery)");
    Simulator::Destroy ();
  }

  std::cout << (g_ok ? "LLR PCS-LOST PASS" : "LLR PCS-LOST FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

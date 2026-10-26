/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * NEG-SP-01/02: UE link negotiation (UEC §5.3, minimal model). Each endpoint
 * exchanges a capability TLV at link start; the LLR engines start only after
 * the peer TLV is applied:
 *   - Options TLV: LLR runs only when BOTH ends support it, else the link
 *     degrades to BEST_EFFORT TX / unfiltered RX.
 *   - CBFC TLV: each end advertises its receiver-NATIVE cell; the sender
 *     adopts the peer's value as its TX granularity ONLY (its own rx cell is
 *     its buffer property and stays). After an asymmetric 256<->64 exchange
 *     each direction runs homogeneous: A.tx=64=B.rx, B.tx=256=A.rx.
 * Run:
 *   cmake --build build -j8 --target scratch_sue-lldp-negotiate
 *   ./build/scratch/ns3.36.1-sue-lldp-negotiate-default
 */
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-channel.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/mac48-address.h"
#include "ns3/node.h"

#include <iostream>

using namespace ns3;

static bool g_ok = true;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    bool c = (cond);                                                           \
    std::cout << "  [" << (c ? "ok" : "FAIL") << "] " << msg << "\n";          \
    g_ok = g_ok && c;                                                          \
  } while (0)


static uint32_t g_rx = 0;
static void
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  ++g_rx;
}

struct Link
{
  Ptr<SueNetDevice> A, B;
  Ptr<SueLlrEngine> eA, eB;
};
static Link
MakeLink (bool bHasLlr)
{
  Link l;
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  l.A = CreateObject<SueNetDevice> ();
  l.B = CreateObject<SueNetDevice> ();
  n1->AddDevice (l.A);
  n2->AddDevice (l.B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  l.A->Attach (ch);
  l.B->Attach (ch);
  l.A->SetDataRate (DataRate ("100Gbps")); l.A->SetIfg (l.A->GetDataRate ().CalculateBytesTxTime (12));   // 12B @100G = 1ns
  l.A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  l.B->SetDataRate (DataRate ("100Gbps"));
  l.eA = CreateObject<SueLlrEngine> ();
  l.A->SetLlrEngine (l.eA);
  if (bHasLlr)
    {
      l.eB = CreateObject<SueLlrEngine> ();
      l.B->SetLlrEngine (l.eB);
    }
  else
    {
      // Legacy preamble-strip path without an engine (the peer still receives
      // BEST_EFFORT ineligible-preamble frames correctly).
      l.B->SetLlrEnabled (true);
    }
  l.B->SetReceiveUpcall (MakeCallback (&RxObs));
  return l;
}

int
main ()
{
  // ---- A: CBFC TLV adoption (asymmetric credit parameters) ----
  {
    std::cout << "[A] CBFC TLV: sender adopts receiver's credit size + VC limits\n";
    Link l = MakeLink (true);
    Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
    ca->SetCreditSize (256);
    for (uint8_t v = 0; v < 8; ++v) { ca->SetVcCreditLimit (v, 512); }
    ca->Init ();
    cb->SetCreditSize (64);   // receiver's own preferred cell size
    for (uint8_t v = 0; v < 8; ++v) { cb->SetVcCreditLimit (v, 8); }
    cb->Init ();
    l.A->SetCbfcEngine (ca);
    l.B->SetCbfcEngine (cb);
    l.A->EnableLldpNegotiation ();
    l.B->EnableLldpNegotiation ();
    l.A->StartLldpExchange ();
    l.B->StartLldpExchange ();
    Simulator::Stop (MicroSeconds (50));
    Simulator::Run ();
    // §5.2.8.2 per-direction adoption: TX adopts the peer's advertised native
    // cell, RX stays local. Each direction is homogeneous after the exchange.
    CHECK (ca->GetTxCellSize () == 64, "A TX adopted B's native cell (64)");
    CHECK (ca->GetRxCellSize () == 256, "A RX native cell untouched (256)");
    CHECK (ca->GetVcCreditLimit (0) == 8, "A adopted B's per-VC limit (8)");
    CHECK (cb->GetTxCellSize () == 256, "B TX adopted A's native cell (256)");
    CHECK (cb->GetRxCellSize () == 64, "B RX native cell untouched (64)");
    CHECK (cb->GetVcCreditLimit (0) == 512, "B adopted A's per-VC limit (512)");
    CHECK ((uint32_t) l.eA->GetTxState () == (uint32_t) SueLlrEngine::TxState::ADVANCE
           && (uint32_t) l.eB->GetTxState () == (uint32_t) SueLlrEngine::TxState::ADVANCE,
           "LLR engines started after negotiation (handshake done)");
    g_rx = 0;
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h;
    h.SetDst (SueMacFromXpuId (2));
    h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    l.A->SwitchSend (0, p);
    Simulator::Stop (MicroSeconds (80));
    Simulator::Run ();
    CHECK (g_rx == 1, "frame delivered over the negotiated link");
    Simulator::Destroy ();
  }

  // ---- B: Options TLV (LLR capability mismatch -> BEST_EFFORT degrade) ----
  {
    std::cout << "[B] Options TLV: peer without LLR -> link degrades to BEST_EFFORT\n";
    Link l = MakeLink (false);   // B has NO LLR engine
    l.A->EnableLldpNegotiation ();
    l.B->EnableLldpNegotiation ();
    l.A->StartLldpExchange ();
    l.B->StartLldpExchange ();   // B speaks LLDP but advertises llrSupported=0
    Simulator::Stop (MicroSeconds (30));   // beyond the 10us fallback
    Simulator::Run ();
    CHECK (l.eA->GetModeRemote () == false, "A TX degraded to BEST_EFFORT (peer lacks LLR)");
    CHECK (l.eA->GetModeLocal () == false, "A RX unfiltered (peer lacks LLR)");
    g_rx = 0;
    Ptr<Packet> p = Create<Packet> (86);
    SueHeader h;
    h.SetDst (SueMacFromXpuId (2));
    h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    l.A->SwitchSend (0, p);
    Simulator::Stop (MicroSeconds (60));
    Simulator::Run ();
    CHECK (g_rx == 1, "frame delivered in the degraded mode");
    Simulator::Destroy ();
  }

  std::cout << (g_ok ? "LLDP NEGOTIATE PASS" : "LLDP NEGOTIATE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

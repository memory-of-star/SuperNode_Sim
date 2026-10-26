/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S2a CBFC engine unit test (counter logic, no messages on wire).
 * A=sender, B=receiver. CreditSize=100, PktOvhd=0, VC0/VC1 limit=5,
 * 100B frame -> 1 credit. Verifies: CanSend/OnTx, exhaustion, CF_Update
 * recovery, CC_Update leak recovery, 8-VC independence.
 */
#include "ns3/sue-cbfc-engine.h"
#include <iostream>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

int
main ()
{
  SueCbfcEngine A, B;                       // A sender, B receiver
  A.SetCreditSize (100);
  A.SetPktOvhd (0);
  A.SetVcCreditLimit (0, 5);
  A.SetVcCreditLimit (1, 5);
  A.Init ();
  B.Init ();

  std::cout << "[1] credit check + consume\n";
  CHECK (A.CanSend (0, 100), "VC0 can send (CU=0)");
  A.OnTx (0, 100);
  CHECK (A.GetSvcCu (0) == 1, "VC0 CU=1 after 1 tx");

  std::cout << "[2] exhaust VC0 (limit 5)\n";
  for (int i = 0; i < 4; ++i) { A.OnTx (0, 100); }   // total 5 consumed
  CHECK (A.GetSvcCu (0) == 5, "VC0 CU=5 (exhausted)");
  CHECK (!A.CanSend (0, 100), "VC0 cannot send (exhausted)");

  std::cout << "[3] 8-VC independence\n";
  CHECK (A.CanSend (1, 100), "VC1 independent (can send)");
  A.OnTx (1, 100);
  CHECK (A.GetSvcCu (1) == 1, "VC1 CU=1");

  std::cout << "[4] CF_Update recovery (receiver drains)\n";
  for (int i = 0; i < 5; ++i) { B.OnRx (0, 100); }   // R_CC0 = 5
  CHECK (B.GetRvcCf (0) == 0, "VC0 R_CF=0 before drain");
  for (int i = 0; i < 5; ++i) { B.OnDrain (0, 100); } // R_CF0 = 5
  CHECK (B.GetRvcCf (0) == 5, "VC0 R_CF=5 after drain");
  A.OnCfUpdate (0, B.GetRvcCf (0));                   // sender reclaims
  CHECK (A.GetSvcCu (0) == 0, "VC0 CU=0 after CF_Update (recovered)");
  CHECK (A.CanSend (0, 100), "VC0 can send again");

  std::cout << "[5] CC_Update leak recovery (dropped packet)\n";
  A.OnTx (0, 100);                  // consumed on sender, but packet dropped (B never OnRx)
  CHECK (A.GetSvcCu (0) == 1, "VC0 CU=1 (leaked credit)");
  std::array<uint32_t, 16> cc{};
  cc[0] = A.GetSvcCc (0);          // sender CC snapshot (6)
  B.OnCcUpdate (cc);               // receiver recovers: lost = 6-5 = 1 -> R_CF += 1
  CHECK (B.GetRvcCf (0) == 6, "VC0 R_CF=6 after CC_Update leak recovery");
  A.OnCfUpdate (0, B.GetRvcCf (0));   // sender reclaims the recovered credit
  CHECK (A.GetSvcCu (0) == 0, "VC0 CU=0 (leak recovered)");

  std::cout << (g_ok ? "CBFC ENGINE-CREDIT PASS" : "CBFC ENGINE-CREDIT FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S5 header byte-level verification (DESIGN §5.6/§5.10): the TestSuite
 * cases in src/sue/test/sue-test.cc cannot run here (test-runner is not
 * built in this fork and test.py is broken on Python 3.14), so this scratch
 * exercises the same assertions through the sanctioned executable path.
 *
 *  (a) SuePfcHeader:   64B on-wire layout (MACDA/SA/EtherType/Opcode/PEV/
 *                      quanta/pad/FCS), CRC covers exactly first 60B.
 *  (b) SueCbfcCcHeader:64B layout (MACDA/SA/EtherType/Opcode/CID/MsgType),
 *                      20-bit mask, MSB-first bit packing, round-trip.
 */
#include "ns3/sue-pfc-header.h"
#include "ns3/sue-cbfc-cc-header.h"
#include "ns3/packet.h"
#include "ns3/mac48-address.h"
#include "ns3/crc32.h"
#include <iostream>
#include <cstring>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

static uint32_t
BeU32 (const uint8_t *p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]); }

static void
TestPfcHeader ()
{
  std::cout << "\n[S5h-a] SuePfcHeader: 64B 802.1Qbb layout\n";
  Mac48Address sa ("02:00:00:00:00:05");
  SuePfcHeader h;
  h.SetMacSa (sa);
  h.SetQuanta (3, 500);          // priority 3 pause, quanta 500
  CHECK (h.GetSerializedSize () == 64, "serialized size == 64");

  Ptr<Packet> p = Create<Packet> (0);
  p->AddHeader (h);
  uint8_t raw[64];
  p->CopyData (raw, 64);
  CHECK (p->GetSize () == 64, "packet size == 64");

  uint8_t saBytes[6];
  sa.CopyTo (saBytes);
  static const uint8_t mda[6] = {0x01,0x80,0xC2,0x00,0x00,0x01};
  CHECK (std::memcmp (raw, mda, 6) == 0, "MACDA == 01-80-C2-00-00-01");
  CHECK (std::memcmp (raw+6, saBytes, 6) == 0, "MACSA == device address");
  CHECK (raw[12]==0x88 && raw[13]==0x08, "EtherType == 0x8808");
  CHECK (raw[14]==0x01 && raw[15]==0x01, "Opcode == 0x0101");
  CHECK (raw[16]==0x00 && raw[17]==0x08, "PEV bit 3 set (big-endian)");
  CHECK (raw[24]==0x01 && raw[25]==0xF4, "quanta[3] == 500 (big-endian, 2B)");
  bool padOk = true;
  for (int i = 34; i < 60; ++i) { if (raw[i] != 0) { padOk = false; } }
  CHECK (padOk, "bytes 34..59 == 0 pad (to 60B Ethernet minimum)");
  CHECK (BeU32 (raw+60) == CRC32Calculate (raw, 60), "FCS == CRC-32 of first 60B");

  // Round-trip through Deserialize.
  SuePfcHeader h2;
  p->RemoveHeader (h2);
  CHECK (h2.GetMacSa () == sa, "round-trip: MACSA preserved");
  CHECK ((h2.GetPev () & (1u<<3)) != 0, "round-trip: PEV bit 3 set");
  CHECK (h2.GetQuanta (3) == 500, "round-trip: quanta[3] == 500");
  CHECK (h2.GetQuanta (0) == 0, "round-trip: unpaused priority quanta == 0");
}

static void
TestCcHeader ()
{
  std::cout << "\n[S5h-b] SueCbfcCcHeader: 64B CC_Update layout + 20-bit packing\n";
  Mac48Address sa ("02:00:00:00:00:05");
  SueCbfcCcHeader h;
  h.SetMacSa (sa);
  h.SetCc (0, 3);
  h.SetCc (1, 0x1FFFFF);         // must be masked to 0xFFFFF (20-bit)
  h.SetCc (7, 0xFFFFF);
  CHECK (h.GetCc (1) == 0xFFFFF, "SetCc(0x1FFFFF) masked to 20-bit 0xFFFFF");
  CHECK (h.GetSerializedSize () == 64, "serialized size == 64");

  Ptr<Packet> p = Create<Packet> (0);
  p->AddHeader (h);
  uint8_t raw[64];
  p->CopyData (raw, 64);
  CHECK (p->GetSize () == 64, "packet size == 64");

  uint8_t saBytes[6];
  sa.CopyTo (saBytes);
  static const uint8_t mda[6] = {0x01,0x80,0xC2,0x00,0x00,0x01};
  CHECK (std::memcmp (raw, mda, 6) == 0, "MACDA == 01-80-C2-00-00-01");
  CHECK (std::memcmp (raw+6, saBytes, 6) == 0, "MACSA == device address");
  CHECK (raw[12]==0x88 && raw[13]==0x08, "EtherType == 0x8808");
  CHECK (raw[14]==0xFF && raw[15]==0xFE, "Opcode == 0xFFFE");
  CHECK (raw[16]==0xFA && raw[17]==0x7A && raw[18]==0xCB, "CID == FA-7A-CB");
  CHECK (raw[19]==0x01, "MsgType == 0x01 (VC[0:15])");
  CHECK (BeU32 (raw+60) == CRC32Calculate (raw, 60), "FCS == CRC-32 of first 60B");

  // Isolated bit-packing check: cc0=3 only (others 0) -> 20-bit value 0b..011,
  // MSB-first: set bits land at field offsets 18,19 -> byte 20+(18>>3)=22,
  // masks 1<<(7-2)=0x20 and 1<<(7-3)=0x10 => bytes 20..22 == 00 00 30.
  {
    SueCbfcCcHeader iso;
    iso.SetCc (0, 3);
    Ptr<Packet> pIso = Create<Packet> (0);
    pIso->AddHeader (iso);
    uint8_t isoRaw[64];
    pIso->CopyData (isoRaw, 64);
    CHECK (isoRaw[20]==0x00 && isoRaw[21]==0x00 && isoRaw[22]==0x30,
           "cc0=3 alone packs to bytes 20..22 == 00 00 30");
    // In the multi-VC frame above, cc1 (all 20 bits set) ORs its first 4 bits
    // into byte 22 too: 0x30 | 0x0F == 0x3F.
    CHECK (raw[20]==0x00 && raw[21]==0x00 && raw[22]==0x3F,
           "multi-VC frame: byte 22 == 0x30 (cc0) | 0x0F (cc1 leading bits) == 0x3F");
  }

  // Round-trip: all 16 counters.
  SueCbfcCcHeader h2;
  p->RemoveHeader (h2);
  bool rtOk = true;
  uint32_t want[16] = {3, 0xFFFFF, 0, 0, 0, 0, 0, 0xFFFFF, 0,0,0,0,0,0,0,0};
  for (uint32_t v = 0; v < 16; ++v) { if (h2.GetCc (v) != want[v]) { rtOk = false; } }
  CHECK (rtOk, "round-trip: all 16 CC values preserved");
}

int
main ()
{
  TestPfcHeader ();
  TestCcHeader ();
  std::cout << "\n" << (g_ok ? "FC FRAME-HEADERS PASS" : "FC FRAME-HEADERS FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

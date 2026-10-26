/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * 稳态抓包场景(本地调试用,不入库): 单链路 A<->B, LLR+CBFC 全开,
 * 无误码注入 -> 无 NACK/FLUSH, 纯稳态: INIT 握手 + 连续数据 + ACK 流 +
 * CF_Update(信用返还) + CC_Update(周期对账)。
 * 节奏取真实预置值(@100G):
 *   CC_Update  102µs   (conf CBFC_CC_MSG_TIMER_US 102, THU 实测 9,790/s)
 *   CF_Update  CF_min 10µs 地板 / CF_max 100µs 周期全量
 *              (conf 125000B / 1250000B @100G, 与 endpoint-ring 同档)
 * 流量: 300 帧 × 86B @ 2µs 间隔(200µs 起步, 先让 INIT 握手收敛)。
 *
 * 抓包: SUE_PCAP=1 SUE_PCAP_DLT=user0 (配 tools/sue-wireshark-plugin.lua)。
 * 预期稳态比例: 数据/ACK 各 300, CF_Update 数十(每 10-20µs), CC_Update ~11
 * —— 与 cc-update-onwire 压力用例(5µs CC 人为加密)相反, CF ≫ CC。
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-cbfc-counters.h"
#include "ns3/sue-header.h"
#include "ns3/sue-config.h"
#include "ns3/sue-shim-header.h"
#include <cstdlib>
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include "sue-scratch-pcap.h"
#include <iostream>

using namespace ns3;

static uint32_t g_rx = 0;
static void RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader) { ++g_rx; }

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

static Ptr<SueNetDevice> g_A;
static uint32_t g_dscp = 0;   // STEADY_DSCP: >0 时帧带 shim(VC=DSCP_TO_VC_MAP[dscp])
static Ptr<Packet>
BuildFrame ()
{
  Ptr<Packet> p = Create<Packet> (86);
  SueHeader h;
  h.SetDst (SueMacFromXpuId (2));
  h.SetSrc (SueMacFromXpuId (1));
  if (g_dscp > 0)
    {
      SueShimHeader sh;
      sh.ttl = 8;
      sh.dscp = g_dscp;
      h.SetEtherType (SUE_ETH_TYPE_1);   // 配 shim 的 EtherType
      h.SetShim (sh);
    }
  else
    {
      h.SetEtherType (SUE_ETH_TYPE_0);
    }
  p->AddHeader (h);
  return p;
}
static void SendOne ()
{
  // §12.51: enqueue on the frame's DERIVED VC — the §12.50 guard asserts
  // qIndex == ComputeVc(h); STEADY_DSCP>0 frames are not VC0.
  Ptr<Packet> p = BuildFrame ();
  SueHeader vh;
  uint32_t vc = 0;
  if (p->PeekHeader (vh))
    {
      if (vh.HasShim ()) { vc = SueConfig::DscpToVc (vh.GetShim ().dscp); }
      else { vc = SueConfig::DefaultVc (); }
    }
  g_A->SwitchSend (vc, p);
}

int
main ()
{
  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);

  // Bypass packet capture: no-op unless SUE_PCAP=1.
  SueScratchPcapAttach (ch, "steady-a-b");

  A->SetDataRate (DataRate ("100Gbps")); A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (DataRate ("100Gbps")); B->SetEgressQueue (CreateObject<SueEgressQueue> ());

  // CBFC: 100B 帧 = 1 信用, 高上限(稳态不触顶); B 侧 drain 10µs
  if (const char *e = std::getenv ("STEADY_DSCP")) { g_dscp = std::atoi (e) & 0x3F; }
  Ptr<SueCbfcEngine> cbfcA = CreateObject<SueCbfcEngine> (), cbfcB = CreateObject<SueCbfcEngine> ();
  cbfcA->SetCreditSize (100);
  cbfcA->SetVcCreditLimit (0, 1024);                       // DEFAULT_VC 路径
  cbfcA->SetVcCreditLimit (g_dscp & 0x07, 1024);           // DSCP 直通映射路径
  cbfcA->Init (); cbfcB->Init ();
  A->SetCbfcEngine (cbfcA); B->SetCbfcEngine (cbfcB);
  B->SetDrainDelay (MicroSeconds (10));

  // 真实预置节奏: CC 102µs; CF_min 10µs / CF_max 100µs(@100G)
  A->StartCcUpdateTimer (MicroSeconds (102));
  B->SetCfUpdateTimers (MicroSeconds (10), MicroSeconds (100));

  // LLR: 双引擎 + 真握手, 无误码 -> 稳态
  Ptr<SueLlrEngine> eA = CreateObject<SueLlrEngine> (), eB = CreateObject<SueLlrEngine> ();
  A->SetLlrEngine (eA); B->SetLlrEngine (eB);
  // STEADY_ACK_SPACING_BYTES: >0 时按配置路径 arm 引擎累积 ACK 周期
  // (LLR_CTLOS_TARGET_SPACING, UEC Table 5-9)+ 设备 CtlOS 400B pacing 门
  // (CTLOS_MIN_SPACING_BYTES, §5.2.11.2)。0 = 引擎默认逐帧即时 ACK(本场景默认)。
  if (const char *e = std::getenv ("STEADY_ACK_SPACING_BYTES"))
    {
      uint32_t bytes = std::atoi (e);
      if (bytes > 0)
        {
          eB->SetAckPeriod (A->GetDataRate ().CalculateBytesTxTime (bytes));
          B->SetCtlOsMinInterval (B->GetDataRate ().CalculateBytesTxTime (400));
          std::cout << "STEADY: ack spacing " << bytes << "B (cumulative ACK, idle repeats)\n";
        }
    }
  eA->Start (); eB->Start ();

  B->SetReceiveUpcall (MakeCallback (&RxObs));

  g_A = A;
  for (uint32_t i = 0; i < 300; ++i)
    {
      Simulator::Schedule (MicroSeconds (200 + 2 * i), &SendOne);
    }
  Simulator::Stop (MicroSeconds (1250));   // 收尾: 最后数据 798µs + drain + CF/CC 余量
  Simulator::Run ();

  std::cout << "[steady] LLR+CBFC steady state (300 frames @2us, CC=102us, CF=10/100us)\n";
  CHECK (g_rx == 300, "all 300 delivered (got " << g_rx << ")");
  CHECK (eA->GetTxState () == SueLlrEngine::TxState::ADVANCE, "LLR state ADVANCE");
  std::cout << "  CF_Update tx=" << cbfcB->GetCounters ().cf_update_tx
            << "  CC_Update tx=" << cbfcA->GetCounters ().cc_update_tx << "\n";
  Simulator::Destroy ();
  std::cout << (g_ok ? "CBFC STEADY-CAPTURE PASS" : "CBFC STEADY-CAPTURE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

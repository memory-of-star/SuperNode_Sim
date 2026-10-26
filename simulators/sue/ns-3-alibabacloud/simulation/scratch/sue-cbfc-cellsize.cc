/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CBFC CreditSize 异粒度四臂对照 (UEC §5.2.5.8 method-2 验证场景)。
 *
 * 背景: 直连两端 MMU cell 不一致(A 原生 128 / 仅支持 2 的幂; SW 原生 56 /
 * 不可配)。协议若取 min(=32) 双向统一, SW 侧 56B cell 记账浪费且可能掉出
 * 线速。方案: 每方向以 RX 端原生 cell 为准 —— A 发往 SW 时 A.tx snap 到
 * 32, SW 按 56 记账; SW 发往 A 时 SW.tx=128。A 侧每 32B 记 1 CC, SW 侧每
 * 56B 记 1 CF, 不足一个 cell 的报文也占满一个信用; 粒度差
 * ceil(F/32)-ceil(F/56) 从 A 的 CU 泄漏, 由周期 CC_Update 对账回收
 * (OnCcUpdate: LostCredits = S_CC - R_CC -> R_VC_CF, §5.2.5.8 method-2)。
 *
 * 四臂 (A→SW 单向持续大帧, 各自独立拓扑+仿真会话):
 *   a  32/32  协议 min() 基线: A.tx=32, SW.rx=32 (无泄漏, SW cell 记账失真)
 *   b  32/56  方案 method-2:   A.tx=32, SW.rx=56 + CC_Update 周期对账
 *   c  32/56  反例臂(关 CC):   同 b 但 CC_Update 关闭 -> 泄漏耗尽窗口后
 *                              硬停 (deliver ~ CL/(1-rx/tx) 帧后 CU 卡死,
 *                              非降速: B 排空后再无返还来源)
 *   d  56/56  理想上界:        A.tx=56, SW.rx=56 (假设 A 也支持 56)
 *
 * 判语:
 *   1. a 与 d 均达线速(>=95%); 两臂吞吐互差 <5% (32B 粒度不损失吞吐);
 *   2. b 恢复到 a 的 90% 以上 (对账把泄漏钉在一个 CC 周期内, CU 呈锯齿);
 *   3. c 塌落到 a 的 75% 以下 (实测硬停: 无对账时泄漏不可自愈);
 *   4. b 的 CC_Update 生效计数 > 0, c 恒为 0;
 *   5. SW 原生 cell 峰值计数: d < a (min() 口径的 cell 数被放大 ~71%)。
 *
 * Env: RATE(100Gbps) FRAME_BYTES(1518) CL(4096) CC_US(10) DRAIN_US(1)
 *      WARM_US(100) STOP_US(3000) N(30000) SAMPLE_US(10)
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

// ---- env (parsed once; shared by all arms) ----
static uint32_t EnvU (const char *k, uint32_t d)
{
  const char *e = std::getenv (k);
  return (e && std::atoi (e) > 0) ? (uint32_t) std::atoi (e) : d;
}

static DataRate g_rate;
static uint32_t g_frameBytes;
static uint32_t g_cl;
static uint32_t g_drainUs;
static uint32_t g_warmUs;
static uint32_t g_stopUs;
static uint32_t g_n;
static uint32_t g_sampleUs;

// ---- per-arm measurement state ----
struct ArmResult
{
  uint64_t rxBytes{0};        // delivered in [warm, stop]
  uint64_t rxAll{0};
  uint32_t cuMax{0};
  uint32_t cuFinal{0};
  uint64_t bufPeakCells{0};   // SW: max(R_CC - R_CF) in native cells
  uint32_t ccTx{0};
  std::vector<uint32_t> cuTrace;
};

static ArmResult *g_res = nullptr;
static Ptr<SueCbfcEngine> g_engA, g_engB;

static void
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  if (!g_res) { return; }
  ++g_res->rxAll;
  if (Simulator::Now () >= MicroSeconds (g_warmUs)) { g_res->rxBytes += g_frameBytes; }
}

static void
Sample ()
{
  if (!g_res || !g_engA || !g_engB) { return; }
  g_res->cuMax = std::max (g_res->cuMax, g_engA->GetSvcCu (0));
  g_res->cuTrace.push_back (g_engA->GetSvcCu (0));
  // receiver-side native-cell occupancy (what the MMU actually backs)
  const uint32_t occ = (g_engB->GetRvcCc (0) - g_engB->GetRvcCf (0)) % SueCbfcEngine::CF_MOD;
  g_res->bufPeakCells = std::max<uint64_t> (g_res->bufPeakCells, occ);
  Simulator::Schedule (MicroSeconds (g_sampleUs), &Sample);
}

// One arm: fresh topology + simulator session. ccUs == 0 disables CC_Update.
static ArmResult
RunArm (const char *name, uint32_t aTxCell, uint32_t bRxCell, uint32_t ccUs)
{
  ArmResult res;
  g_res = &res;

  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> (); A->Attach (ch); B->Attach (ch);

  A->SetDataRate (g_rate); A->SetIfg (g_rate.CalculateBytesTxTime (12));
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (g_rate); B->SetEgressQueue (CreateObject<SueEgressQueue> ());

  // A (pow2-only device): TX snaps to aTxCell toward the switch; native rx 128.
  // B (switch role): rx native bRxCell (fixed); TX toward A at A's native 128.
  Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
  ca->SetTxCellSize (aTxCell); ca->SetRxCellSize (128);
  cb->SetTxCellSize (128);    cb->SetRxCellSize (bRxCell);
  ca->SetVcCreditLimit (0, g_cl);
  ca->Init (); cb->Init ();
  A->SetCbfcEngine (ca); B->SetCbfcEngine (cb);
  g_engA = ca; g_engB = cb;

  B->SetDrainDelay (MicroSeconds (g_drainUs));
  B->SetCfUpdateTimers (MicroSeconds (2), MicroSeconds (20));
  if (ccUs > 0) { A->StartCcUpdateTimer (MicroSeconds (ccUs)); }

  B->SetReceiveUpcall (MakeCallback (&RxObs));

  const uint32_t payload = g_frameBytes - 18;   // SueHeader (MAC+VLAN+etype)
  auto build = [payload] () {
    Ptr<Packet> p = Create<Packet> (payload);
    SueHeader h;
    h.SetDst (SueMacFromXpuId (2));
    h.SetSrc (SueMacFromXpuId (1));
    h.SetEtherType (SUE_ETH_TYPE_0);
    p->AddHeader (h);
    return p;
  };
  // Pre-enqueue everything; the egress credit gate paces the wire (frames
  // queue while CU + pktCredits > CL), so the offered load is always line-rate
  // and the achieved rate is set purely by the credit dynamics under test.
  for (uint32_t i = 0; i < g_n; ++i) { A->SwitchSend (0, build ()); }

  Simulator::Schedule (MicroSeconds (g_sampleUs), &Sample);
  Simulator::Stop (MicroSeconds (g_stopUs));
  Simulator::Run ();
  res.cuFinal = ca->GetSvcCu (0);
  res.ccTx = ca->GetCounters ().cc_update_tx;

  const double sec = (g_stopUs - g_warmUs) * 1e-6;
  const double gbps = (double) res.rxBytes * 8.0 / sec / 1e9;
  std::cout << "[arm " << name << "] A.tx=" << aTxCell << "B SW.rx=" << bRxCell
            << "B CC=" << (ccUs ? std::to_string (ccUs) + "us" : std::string ("off"))
            << std::fixed << std::setprecision (2)
            << "  |  " << gbps << " Gbps (" << gbps / (g_rate.GetBitRate () / 1e9) * 100.0 << "%)"
            << "  CU max " << res.cuMax << "/" << g_cl << " final " << res.cuFinal
            << "  SW cell peak " << res.bufPeakCells << " (" << res.bufPeakCells * (uint64_t) bRxCell / 1024 << "KiB)"
            << "  CC_tx " << res.ccTx
            << "  rx " << res.rxAll << "/" << g_n << "\n";
  (void) g_engA; (void) g_engB;
  g_engA = nullptr; g_engB = nullptr; g_res = nullptr;
  Simulator::Destroy ();
  return res;
}

int
main ()
{
  std::cout << std::unitbuf;
  const char *rateStr = "100Gbps";
  if (const char *e = std::getenv ("RATE")) { rateStr = e; }
  g_rate = DataRate (rateStr);
  g_frameBytes = EnvU ("FRAME_BYTES", 1518);
  g_cl = EnvU ("CL", 4096);
  g_drainUs = EnvU ("DRAIN_US", 1);
  g_warmUs = EnvU ("WARM_US", 100);
  g_stopUs = EnvU ("STOP_US", 3000);
  g_n = EnvU ("N", 30000);
  g_sampleUs = EnvU ("SAMPLE_US", 10);
  const uint32_t ccUs = EnvU ("CC_US", 10);

  std::cout << "CBFC cell-size A/B: " << rateStr << ", frame " << g_frameBytes
            << "B, window " << g_cl << " credits, drain " << g_drainUs << "us,"
               " measure [" << g_warmUs << "," << g_stopUs << "]us\n\n";

  const ArmResult a = RunArm ("a min() ", 32, 32, 0);            // CC irrelevant: no leak
  const ArmResult b = RunArm ("b mthd-2", 32, 56, ccUs);
  const ArmResult c = RunArm ("c no-CC ", 32, 56, 0);
  const ArmResult d = RunArm ("d ideal ", 56, 56, 0);

  auto gbps = [] (const ArmResult &r) {
    return (double) r.rxBytes * 8.0 / ((g_stopUs - g_warmUs) * 1e-6) / 1e9;
  };
  const double line = g_rate.GetBitRate () / 1e9;
  const double ga = gbps (a), gb = gbps (b), gc = gbps (c), gd = gbps (d);

  std::cout << "\n[verdicts]\n";
  CHECK (ga >= 0.95 * line, "arm a (min 32-32) at line rate: " << ga << " >= 95% x " << line);
  CHECK (gd >= 0.95 * line, "arm d (ideal 56-56) at line rate: " << gd << " >= 95% x " << line);
  CHECK (std::abs (ga - gd) <= 0.05 * line, "a == d within 5% (cell size alone costs nothing)");
  CHECK (gb >= 0.90 * ga, "arm b (32/56 + CC) recovers >= 90% of baseline: " << gb << " vs " << ga);
  CHECK (gc <= 0.75 * ga, "arm c (32/56 no CC) collapses <= 75% of baseline: " << gc << " vs " << ga);
  CHECK (gb > gc, "method-2 strictly beats the no-reconcile control");
  CHECK (b.ccTx > 0, "arm b CC_Update active (" << b.ccTx << " frames)");
  CHECK (c.ccTx == 0, "arm c CC_Update disabled");
  CHECK (d.bufPeakCells < a.bufPeakCells,
         "SW native-cell accounting tighter than min()-cell (d " << d.bufPeakCells
         << " < a " << a.bufPeakCells << " cells)");

  std::cout << (g_ok ? "CBFC CELLSIZE PASS" : "CBFC CELLSIZE FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

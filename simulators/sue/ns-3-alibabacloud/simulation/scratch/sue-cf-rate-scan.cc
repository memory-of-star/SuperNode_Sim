/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S5g: CF_Update CtlOS 速率扫描(THU 芯片测量的仿真同构)。
 *
 * 2 节点直连 CBFC,按 env 配置三键 + 打流/不打流,实测 CF_Update 消息速率
 * 并与模型公式比对(自检式 PASS/FAIL)。CF_Update 是 8B CtlOS 消息,每条
 * 打包 2 个 (VC,CF) 条目(§5.6):threshold 触发只脏收流 VC(1 条/触发),
 * CF_max 周期全量报全部 8 个 VC(ceil(8/2)=4 条/触发)。
 *
 *   threshold 主导: rate ≈ line / (8 × threshold)(每触发 1 条)
 *   地板封顶:       rate ≤ line / (8 × CF_MIN)(每触发 1 条)
 *   timer 兜底:     rate ≈ 4 × line / (8 × CF_MAX)(每周期 4 条)
 *   硬件门顶:       rate ≤ line / (8 × 400)(CtlOS 2% 帽的 400B 门)
 *   打流时 threshold/CF_MIN/门取 min(最严限制);CF_max 是独立事件流
 *
 * env 旋钮:
 *   CF_RATE             链路速率(默认 100Gbps)
 *   CF_THRESHOLD_BYTES  数据量触发阈值(默认 2048=芯片默认档;0=关闭;1=逐帧触发)
 *   CF_MIN_BYTES        CF_min 地板(线字节;0=无地板)
 *   CF_MAX_BYTES        CF_max 周期全量(线字节;0=关闭)
 *   CF_GBITS            打流量(默认 1=1Gbit 线速;0=不打流)
 *   CF_FRAME_BYTES      打流帧长(默认 4096;阈值按数据字节计,小帧组帧开销
 *                       (前导+IFG)会按比例拉低数据字节率、拖慢阈值档速率——
 *                       THU 打流为大帧≈100% 效率,故默认大帧。测 CF_min 地板
 *                       档时须用小帧(如 100B):帧到达节奏须密于地板,否则
 *                       发送跟随帧率而非地板)
 *   CF_CREDITS          发送端信用窗口(默认 4096;须 ≥ threshold/creditSize,
 *                       否则发送端在接收端攒够阈值前停摆——芯片同款约束)
 *   CF_TOL_PCT          与公式的容差百分比(默认 25)
 *
 * 裸跑 = 芯片默认档复现(threshold 2048B 线速打流 → 6.10M/s@100G,THU 6.02M)。
 */
#include "ns3/sue-channel.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-header.h"
#include "ns3/sue-ctl-header.h"
#include "ns3/sue-ctl-tag.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mac48-address.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <cstring>

using namespace ns3;

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

// CF_Update 线速观测:channel SueRx trace 筛 CF_UPDATE tag
static uint64_t g_cfCount = 0;
static bool g_cfFirstSet = false;
static Time g_cfFirst, g_cfLast;
static void
CfObs (Ptr<const Packet> p, Ptr<NetDevice>, Ptr<NetDevice>, Time, Time)
{
  SueCtlTag tag;
  if (!(p->PeekPacketTag (tag) && tag.GetType () == SueCtlHeader::CF_UPDATE)) { return; }
  if (!g_cfFirstSet) { g_cfFirstSet = true; g_cfFirst = Simulator::Now (); }
  g_cfLast = Simulator::Now ();
  ++g_cfCount;
}

static uint64_t g_rx = 0;
static bool g_rxFirstSet = false;
static Time g_firstRx, g_lastRx;
static uint64_t g_cfAtFirstRx = 0, g_cfAtLastRx = 0;
static void
RxObs (Ptr<SueNetDevice>, Ptr<Packet>, SueHeader)
{
  // 打流窗口 = 首/末数据帧到达时刻;CF 计数锚定窗口端点,把打流结束后
  // 空闲期继续发的 CF_max 周期全量排除在速率窗口之外。
  if (!g_rxFirstSet) { g_rxFirstSet = true; g_firstRx = Simulator::Now (); g_cfAtFirstRx = g_cfCount; }
  g_lastRx = Simulator::Now ();
  g_cfAtLastRx = g_cfCount;
  ++g_rx;
}

static Ptr<Packet>
BuildFrame (uint32_t frameBytes)
{
  // frameBytes = SueHeader(14B) + payload;SueHeader 14B -> payload 至少 86B。
  Ptr<Packet> p = Create<Packet> (frameBytes - 14);
  SueHeader h; h.SetDst (SueMacFromXpuId (2)); h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  return p;
}

int
main ()
{
  std::string rateStr = "100Gbps";
  if (const char *e = std::getenv ("CF_RATE")) { rateStr = e; }
  uint64_t threshold = 2048;
  if (const char *e = std::getenv ("CF_THRESHOLD_BYTES")) { threshold = std::strtoull (e, nullptr, 0); }
  uint64_t cfMin = 0;
  if (const char *e = std::getenv ("CF_MIN_BYTES")) { cfMin = std::strtoull (e, nullptr, 0); }
  uint64_t cfMax = 0;
  if (const char *e = std::getenv ("CF_MAX_BYTES")) { cfMax = std::strtoull (e, nullptr, 0); }
  uint32_t gbits = 1;
  if (const char *e = std::getenv ("CF_GBITS")) { gbits = std::atoi (e); }
  uint32_t frameBytes = 4096;
  if (const char *e = std::getenv ("CF_FRAME_BYTES")) { frameBytes = std::atoi (e); }
  NS_ABORT_MSG_IF (frameBytes < 100 || frameBytes > 9000, "CF_FRAME_BYTES out of [100, 9000]");
  uint32_t credits = 4096;
  if (const char *e = std::getenv ("CF_CREDITS")) { credits = std::atoi (e); }
  double tolPct = 25.0;
  if (const char *e = std::getenv ("CF_TOL_PCT")) { tolPct = std::atof (e); }

  const DataRate rate (rateStr);
  const double bps = rate.GetBitRate ();

  std::cout << std::unitbuf;
  std::cout << "CF rate scan: rate=" << rateStr
            << " threshold=" << threshold << "B cf_min=" << cfMin
            << "B cf_max=" << cfMax << "B traffic=" << gbits << "Gbit\n";

  Ptr<Node> n1 = CreateObject<Node> (), n2 = CreateObject<Node> ();
  Ptr<SueNetDevice> A = CreateObject<SueNetDevice> (), B = CreateObject<SueNetDevice> ();
  n1->AddDevice (A); n2->AddDevice (B);
  Ptr<SueChannel> ch = CreateObject<SueChannel> ();
  A->Attach (ch); B->Attach (ch);
  A->SetDataRate (rate);
  A->SetIfg (A->GetDataRate ().CalculateBytesTxTime (12));
  A->SetEgressQueue (CreateObject<SueEgressQueue> ());
  B->SetDataRate (rate);
  B->SetEgressQueue (CreateObject<SueEgressQueue> ());
  // CtlOS 2% 带宽帽的 400B 门(配置路径恒设;逐帧触发档靠它封顶)。
  A->SetCtlOsTargetSpacing (400);
  B->SetCtlOsTargetSpacing (400);

  Ptr<SueCbfcEngine> ca = CreateObject<SueCbfcEngine> (), cb = CreateObject<SueCbfcEngine> ();
  ca->SetCreditSize (100);
  ca->SetVcCreditLimit (0, credits);   // 窗口 ≥ 阈值/100B(死锁约束)
  ca->Init ();
  cb->SetCreditSize (100);
  cb->Init ();
  A->SetCbfcEngine (ca);
  B->SetCbfcEngine (cb);
  // 无 drainDelay:排空即退款(阈值计数即时)。
  B->SetCfUpdateThresholdBytes (threshold);
  B->SetCfUpdateTimers (cfMin > 0 ? B->GetDataRate ().CalculateBytesTxTime (cfMin) : Seconds (0),
                        cfMax > 0 ? B->GetDataRate ().CalculateBytesTxTime (cfMax) : Seconds (0));

  ch->TraceConnectWithoutContext ("TxRxSue", MakeCallback (&CfObs));
  B->SetReceiveUpcall (MakeCallback (&RxObs));
  g_cfCount = 0; g_cfFirstSet = false; g_rx = 0;
  g_rxFirstSet = false; g_cfAtFirstRx = 0; g_cfAtLastRx = 0;

  if (gbits > 0)
    {
      // frameBytes 帧注入 gbits Gbit(egress 队列线速排空)。
      const uint64_t nFrames = (uint64_t) gbits * 1000000000ull / 8 / frameBytes;
      for (uint64_t i = 0; i < nFrames; ++i) { A->SwitchSend (0, BuildFrame (frameBytes)); }
      const double simSec = (double) nFrames * frameBytes * 8 / bps;
      Simulator::Stop (Seconds (simSec + 0.001));
    }
  else
    {
      // 无打流:只有 CF_max 周期全量(或 0=无 CF)。
      Simulator::Stop (MilliSeconds (1));
    }
  Simulator::Run ();
  Simulator::Destroy ();

  double measRate = 0;
  if (gbits > 0 && g_rxFirstSet && g_lastRx > g_firstRx)
    {
      // 打流:速率锚定数据接收窗口(排除打流后空闲期 CF_max 尾);
      // CF 计数取窗口端点锚定值(含窗口内周期全量,与真机计数同口径)。
      measRate = (double) (g_cfAtLastRx - g_cfAtFirstRx)
                 / (g_lastRx.GetSeconds () - g_firstRx.GetSeconds ());
    }
  else if (g_cfCount >= 2 && g_cfLast > g_cfFirst)
    { measRate = (g_cfCount - 1) / (g_cfLast.GetSeconds () - g_cfFirst.GetSeconds ()); }
  std::cout << "  cf_update messages=" << g_cfCount << "  rate=" << measRate << "/s"
            << "  rx=" << g_rx << "\n";

  // 模型比对:消息速率 = 批量 × 触发速率。threshold 触发逐 VC 脏(1 条),
  // 受 CF_min 间距与 400B 门封顶;CF_max 周期全量报全部 8 个 VC(4 条/次),
  // 是独立事件流(真机合流时 CF_max 会吞掉挂起的 CF_min 一次射击,扫描配方
  // 不组合这两键,取 max 仅作近似)。
  const double batchFull = std::ceil (SueCbfcEngine::NVC / 2.0); // 全量批 = 4 条
  const double capGate = bps / 8.0 / 400.0;                     // 400B 门顶
  const double capMin = cfMin ? bps / 8.0 / cfMin : 1e18;       // CF_min 间距顶
  const double fullRate = cfMax ? bps / 8.0 / cfMax * batchFull : 0.0; // CF_max 全量
  const double thrRate = threshold ? bps / 8.0 / threshold : 0.0;

  double expect;
  if (gbits > 0)
    {
      expect = threshold ? std::min (thrRate, std::min (capMin, capGate))
                         : (cfMin ? std::min (capMin, capGate) : 0.0);
      if (cfMax > 0) { expect = std::max (expect, fullRate); }
    }
  else
    {
      expect = fullRate;   // 无打流:只有 CF_max 周期全量
    }

  const double lo = expect * (1 - tolPct / 100.0);
  const double hi = expect * (1 + tolPct / 100.0);
  CHECK (measRate >= lo && measRate <= hi,
         "CF rate within " << (int) tolPct << "% of model (got " << measRate
         << "/s vs expect " << expect << "/s [" << lo << ", " << hi << "])");
  CHECK (measRate <= capGate * 1.05 || gbits == 0 || measRate == 0,
         "CF rate under the 400B gate ceiling (" << capGate << "/s)");
  if (cfMax > 0) { CHECK (measRate >= fullRate * 0.8 || measRate == 0, "CF rate above the CF_max floor (" << fullRate << "/s)"); }

  std::cout << (g_ok ? "CF RATE-SCAN PASS" : "CF RATE-SCAN FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

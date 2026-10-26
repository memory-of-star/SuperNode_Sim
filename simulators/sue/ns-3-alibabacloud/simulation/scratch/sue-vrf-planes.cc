/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * VRF 多平面拓扑 · IO Die 编址版(DESIGN §12.41,§12.54 需求变更):
 * 8 GPU × 32×200G 口、单交换机 256×200G。
 *
 * 需求模型(v2):每 GPU 的 32 个口由 4 个 IO Die 提供(Die 占连续 8 口,
 * 口序 k = Die*8 + stripe,stripe∈[0,8));Die ID 编址进 XPU-ID——目的地址
 * 自含 Die 信息,不再需要靠入口端口位置消歧,因此隔离域从 32 收缩到 8:
 *
 *   XPU-ID    x = gpu*4 + die            (0..31,gpu∈[0,8),die∈[0,4))
 *   端口      host i 的口 k = die*8 + stripe(k 即设备序;全局口序 = gpu*32+k)
 *   入口映射  PORT_TO_VRF_MAP:p % 8 + 1 —— 每个 Die 内同 stripe 的口同平面
 *   平面路由  VRF v(stripe v-1):x=(j,dd) -> 全局口 j*32 + dd*8 + (v-1)
 *             (出/入同一 stripe:帧在哪个平面进入,就在哪个平面出去)
 *
 * 8 个平面 = 每平面自 (8 GPU × 4 Die) 各取同 stripe 的 1 口,共 32 口;
 * 与旧 32 平面模型等价提供 all-to-all 织构饱和,但编址语义贴合实物
 * (IO Die 划分 + Die 可寻址)。split 变体(§12.45)相应变为:按 stripe
 * 拆成 8 台 32 口交换机(node 8+m,端口序 = i*4+die),VRF 隔离保证与单机
 * 转发严格等价;MTP 分区单位是节点(16 LP)。
 *
 * 流量:all-to-all(56 有向对)。每 (src port, dst gpu, dst die) 一条流;
 * 每 (src,dst-gpu) 对总帧数保持 32 流 × 每-流帧数(与旧口径同负载);
 * 帧/流数 = pairFrames / 128(pairFrames 强制 128 对齐)。入口 stripe 决定
 * 平面、出口 stripe 相同 → 逐平面 / 逐端口计数均可整数精确断言。
 *
 * 断言(自检式 PASS/FAIL):
 *   1. 互通:每 GPU 经每个源 XPU-ID(含 Die 位)收到恰好 per-stream 帧;
 *      折叠到 (dst-gpu, src-gpu) == pairFrames;delivered == sent,零丢失;
 *   2. 平面隔离:每 VRF(v1..8)入口计数 == 8GPU×4Die×28流×N';
 *      单机模式 VRF0 == 0;lookup miss == 0(split:各平面机 VRF0 计数);
 *   3. 端口对称:每端口收 28 × N'(7 源 GPU × 4 源 Die);
 *   4. LLR 全栈:replay == discard == nack == 0;
 *   5. 性能:聚合 Gbps ≥ 95% × 载荷份额天花板(PFC 模式沿用同 CBFC 标尺)。
 *
 * env:与旧版一致(VRF_PL_PAIR_GBITS / FRAME_BYTES / LLR / FC /
 * PFC_DRAIN_NS / THREADS / SPLIT);SPLIT 现在 = 8 台 32 口平面交换机。
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-config.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-routing.h"
#include "ns3/sue-header.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/nstime.h"
#include "ns3/data-rate.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "sue-scratch-util.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>

using namespace ns3;

static const uint32_t NGPU = 8;     // 8 卡机
static const uint32_t NDIE = 4;     // 每 GPU 4 个 IO Die
static const uint32_t PDIE = 8;     // 每 Die 8 口
static const uint32_t NVRF = 8;     // 隔离平面数(= 每 Die 的 stripe 数)
static const uint32_t NPORT = NDIE * PDIE;        // 每 GPU 32 口
static const uint32_t NXPU = NGPU * NDIE;         // XPU-ID 地址空间(含 Die 位)
static const uint32_t SWNODE = NGPU;              // 单机模式交换机节点 id(host 0..7,switch 8)

static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while (0)

// ---- globals for the verify phase ----
static std::vector<Ptr<SUESwitchNode>> g_sws;   // 单机 = 1 台;split = 8 台平面交换机(SWNODE..SWNODE+7)
static uint32_t g_frameBytes = 2048;
static uint32_t g_pairGbits = 1;
static std::string g_fcMode = "CBFC";
static uint32_t g_pfcDrainNs = 0;
static uint32_t g_threads = 1;
static bool g_split = false;
static uint64_t g_perStream = 0;      // 每 (src port, dst gpu, dst die) 流帧数 N'
static uint64_t g_pairFrames = 0;     // 每 (src,dst)-GPU 有向对总帧数(= 128 × N')

// ---- rx attribution ----
static std::map<Ptr<SueNetDevice>, std::pair<uint32_t, uint32_t>> g_devGpuPort;
static uint64_t g_rxByDstGpuSrcXpu[NGPU][NXPU];  // [dst gpu][src xpu(gpu*4+die)]
static uint64_t g_rxByPortSrc[NGPU][NPORT][NXPU]; // [dst gpu][port][src xpu] —— 精确到端口的强断言
static uint64_t g_rxByGpuPort[NGPU][NPORT];      // [dst gpu][port k=die*8+stripe]
static std::atomic<uint64_t> g_rxBytes {0};
static std::atomic<bool> g_firstSet {false};
static std::atomic<int64_t> g_firstRxNs {0}, g_lastRxNs {0};
static uint64_t g_pauseSamples = 0;

static void
RxObs (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h)
{
  auto it = g_devGpuPort.find (dev);
  if (it == g_devGpuPort.end ()) { return; }
  const uint32_t gpu = it->second.first, port = it->second.second;
  const uint32_t sx = SueXpuFromMac (h.GetSrc ()) % NXPU;
  ++g_rxByDstGpuSrcXpu[gpu][sx];
  ++g_rxByPortSrc[gpu][port][sx];
  ++g_rxByGpuPort[gpu][port];
  g_rxBytes += p->GetSize ();
  const int64_t nowNs = Simulator::Now ().GetNanoSeconds ();
  bool expectFalse = false;
  if (g_firstSet.compare_exchange_strong (expectFalse, true)) { g_firstRxNs = nowNs; }
  int64_t prev = g_lastRxNs.load ();
  while (nowNs > prev && !g_lastRxNs.compare_exchange_weak (prev, nowNs)) {}
}

// ---- per-(src port, dst gpu, dst die) stream: self-chaining send ----
struct Stream
{
  Ptr<SueNetDevice> dev;
  uint32_t srcXpu;
  uint32_t dstXpu;
  uint64_t remaining;
  Time interval;
};
static std::atomic<uint64_t> g_streamsLeft {0};
static void VerifyAndStop (void);
static void
SendOne (Stream *s)
{
  // FC 感知注入(同旧版):CBFC 窗口满 / PFC pause 时不排队,100ns 重试,
  // 让 CtlOS/pause 自由流动。两种模式同一注入纪律。
  Ptr<SueCbfcEngine> cbfc = s->dev->GetCbfcEngine ();
  Ptr<SuePfcEngine> pfc = s->dev->GetPfcEngine ();
  if ((cbfc && !cbfc->CanSend (0, g_frameBytes)) || (pfc && !pfc->CanSend (0)))
    {
      Simulator::Schedule (NanoSeconds (100), &SendOne, s);
      return;
    }
  Ptr<Packet> p = Create<Packet> (g_frameBytes - 14);
  SueHeader h;
  h.SetDst (SueMacFromXpuId (s->dstXpu));
  h.SetSrc (SueMacFromXpuId (s->srcXpu));
  h.SetEtherType (SUE_ETH_TYPE_0);
  p->AddHeader (h);
  s->dev->SwitchSend (0, p);
  if (--s->remaining > 0)
    {
      Simulator::Schedule (s->interval, &SendOne, s);
      return;
    }
  delete s;
  if (g_streamsLeft.fetch_sub (1) == 1)
    {
      Simulator::Schedule (MilliSeconds (5), &VerifyAndStop);
    }
}

static void
VerifyAndStop (void)
{
  // 计数基准:N' = 每-(源口, 目的GPU, 目的Die)帧数。推导(全部整数精确):
  //   每端口发出 = 7 目的GPU × 4 目的Die × N'
  //   每 GPU 收  = 32 源口 × 4 目的Die × ... 见各断言
  const uint64_t N = g_perStream;

  // 1. 互通:折叠 (dst-gpu, src-gpu) == pairFrames;强断言:每 (dst 口, src-xpu)
  //    恰收 N'(几何:出/入同 stripe,每条流全部帧落在唯一出口)
  uint64_t sent = 56ull * g_pairFrames, delivered = 0;
  bool everyPair = true, everyCell = true;
  for (uint32_t j = 0; j < NGPU; ++j)
    {
      for (uint32_t i = 0; i < NGPU; ++i)
        {
          if (i == j) { continue; }
          uint64_t folded = 0;
          for (uint32_t dd = 0; dd < NDIE; ++dd)
            {
              folded += g_rxByDstGpuSrcXpu[j][i * NDIE + dd];
            }
          if (folded != g_pairFrames) { everyPair = false; }
          delivered += folded;
        }
      for (uint32_t k = 0; k < NPORT; ++k)
        {
          for (uint32_t sx = 0; sx < NXPU; ++sx)
            {
              const uint32_t expect = (sx / NDIE == j) ? 0 : N;   // 本 GPU 自身非流量源
              if (g_rxByPortSrc[j][k][sx] != expect) { everyCell = false; }
            }
        }
    }
  CHECK (everyCell, "every (dst-port, src-xpu-id-with-die) cell received exactly " << N << " frames");
  CHECK (everyPair, "each GPU received " << g_pairFrames << " frames from every other GPU (folded over Die addressing)");
  CHECK (delivered == sent, "delivered == sent (" << delivered << "/" << sent << ", zero loss)");

  // 2. 平面隔离:平面 v 入口计数 = 8 GPU × 4 Die(源口)× 7 对端 × 4 目的Die × N'
  //    单机 = VRF v 段计数;split = 第 v 台平面交换机的 VRF0 计数(§12.45 口径)
  const uint64_t expectPlane = (uint64_t) NGPU * NDIE * (NGPU - 1) * NDIE * N;
  bool planesOk = true;
  uint64_t missSum = 0;
  for (uint32_t v = 0; v < NVRF; ++v)
    {
      const uint64_t cnt = g_sws[g_split ? v : 0]->GetCntVrfRx (g_split ? 0 : v + 1);
      if (cnt != expectPlane)
        {
          planesOk = false;
          if (v < 2) { std::cout << "    plane " << (v + 1) << " forwarded " << cnt << " (expect " << expectPlane << ")\n"; }
        }
      missSum += g_sws[g_split ? v : 0]->GetCntLookupMiss ();
    }
  CHECK (planesOk, "every plane forwarded exactly 32 sources x 28 streams x " << N << " frames (per-plane isolation)");
  if (!g_split)
    {
      CHECK (g_sws[0]->GetCntVrfRx (0) == 0, "VRF0 forwarded 0 (default plane unused)");
    }
  CHECK (missSum == 0, "lookup miss == 0 (all plane tables complete)");

  // 3. 端口对称:每端口收 7 源GPU × 4 源Die × N'(同 stripe 出入)
  bool portsOk = true;
  for (uint32_t j = 0; j < NGPU; ++j)
    {
      for (uint32_t k = 0; k < NPORT; ++k)
        {
          if (g_rxByGpuPort[j][k] != (uint64_t) (NGPU - 1) * NDIE * N) { portsOk = false; }
        }
    }
  CHECK (portsOk, "every GPU port received exactly 28 x " << N << " (matching stripes)");

  // 4. LLR 全栈:零 replay/discard/nack
  uint64_t replay = 0, discard = 0, nack = 0;
  for (uint32_t i = 0; i < NGPU; ++i)
    {
      const uint32_t node = SueGetHostNodeId (i);
      for (uint32_t k = 0; k < NPORT; ++k)
        {
          Ptr<SueLlrEngine> llr = SueGetDevice (node, k)->GetLlrEngine ();
          if (!llr) { continue; }
          replay += llr->GetCounters ().tx_replay;
          discard += llr->GetCounters ().tx_discard;
          nack += llr->GetCounters ().tx_nack_ctl_os;
        }
    }
  CHECK (replay == 0 && discard == 0 && nack == 0,
         "LLR clean: replay=" << replay << " discard=" << discard << " nack=" << nack);

  // 5. 性能:聚合 Gbps vs 载荷份额天花板(数据帧 wire = 帧+14+8+12,返回
  //    链路每帧 1 ACK + 1 CF 各 29B 线时,PFC 无 CF 同标尺对比)
  const double ceiling = (double) NGPU * NPORT * 200.0
                         * g_frameBytes / (double) (g_frameBytes + 14 + 8 + 12 + 2 * (9 + 8 + 12));
  double agg = 0;
  if (g_firstSet && g_lastRxNs > g_firstRxNs)
    {
      agg = (double) g_rxBytes * 8.0 / ((g_lastRxNs - g_firstRxNs) * 1e-9) / 1.0e9;
    }
  {
    uint64_t pend = 0, pendMax = 0, rx = 0, tx = 0;
    for (auto &sw : g_sws)
      {
        pend += sw->GetCntPendPkts ();
        pendMax += sw->GetCntPendMax ();
        rx += sw->GetCntRxPkts ();
        tx += sw->GetCntTxPkts ();
      }
    std::cout << "  switch pend=" << pend << " pendMax=" << pendMax << " rx=" << rx << " tx=" << tx << "\n";
  }
  std::cout << "  aggregate=" << agg << " Gbps (ceiling " << ceiling << " Gbps, "
            << (ceiling > 0 ? 100.0 * agg / ceiling : 0.0) << "%)\n";
  CHECK (agg >= 0.95 * ceiling, "all-to-all aggregate >= 95% of the payload-share ceiling ("
         << agg << " vs " << 0.95 * ceiling << " Gbps)");
  for (uint32_t j = 0; j < NGPU; ++j)
    {
      uint64_t gb = 0;
      for (uint32_t sd = 0; sd < NXPU; ++sd) { gb += g_rxByDstGpuSrcXpu[j][sd]; }
      std::cout << "    gpu" << j << " rx=" << gb * g_frameBytes * 8.0 / 1.0e9 << " Gbit\n";
    }
  if (g_fcMode == "PFC")
    {
      std::cout << "  pfc: pause-samples=" << g_pauseSamples << " drain=" << g_pfcDrainNs << "ns\n";
    }
  // MTP 等价判据:BHASH = 设备计数器 + 交付矩阵(folded 到 (dst-gpu, src-gpu))
  {
    std::ostringstream o;
    SueScratchAppendDeviceCounters (o, true, true);
    for (uint32_t j = 0; j < NGPU; ++j)
      {
        for (uint32_t i = 0; i < NGPU; ++i)
          {
            uint64_t folded = 0;
            for (uint32_t sd = 0; sd < NDIE; ++sd) { folded += g_rxByDstGpuSrcXpu[j][i * NDIE + sd]; }
            o << folded << ",";
          }
      }
    std::cout << "VRF-PLANES BHASH " << o.str () << "\n";
  }
  std::cout << (g_ok ? "VRF-PLANES PASS" : "VRF-PLANES FAIL") << std::endl;
  Simulator::Stop ();
}

int
main ()
{
  if (const char *e = std::getenv ("VRF_PL_PAIR_GBITS")) { g_pairGbits = std::atoi (e); }
  if (const char *e = std::getenv ("VRF_PL_FRAME_BYTES")) { g_frameBytes = std::atoi (e); }
  uint32_t llrEna = 1;
  if (const char *e = std::getenv ("VRF_PL_LLR")) { llrEna = std::atoi (e); }
  if (const char *e = std::getenv ("VRF_PL_FC")) { g_fcMode = e; }
  if (const char *e = std::getenv ("VRF_PL_PFC_DRAIN_NS")) { g_pfcDrainNs = std::atoi (e); }
  if (const char *e = std::getenv ("VRF_PL_THREADS")) { g_threads = std::atoi (e); }
  if (const char *e = std::getenv ("VRF_PL_SPLIT")) { g_split = std::atoi (e) == 1; }
  NS_ABORT_MSG_IF (g_frameBytes < 100 || g_frameBytes > 9000, "VRF_PL_FRAME_BYTES out of [100, 9000]");
  NS_ABORT_MSG_IF (g_fcMode != "CBFC" && g_fcMode != "PFC", "VRF_PL_FC must be CBFC or PFC");

  const DataRate rate ("200Gbps");
  std::cout << std::unitbuf;
  std::cout << "VRF-PLANES gpus=" << NGPU << " io-dies/gpu=" << NDIE << " ports/die=" << PDIE
            << " xrfs=" << NVRF << " (die-in-xpuid addressing)"
            << " pair=" << g_pairGbits << "Gbit frame=" << g_frameBytes << "B rate=200Gbps"
            << " fc=" << g_fcMode
            << (g_fcMode == "PFC" ? " drain=" + std::to_string (g_pfcDrainNs) + "ns" : "")
            << " threads=" << g_threads
            << (g_threads > 1 ? " (NS3_MTP auto-partition)" : " (DefaultSimulatorImpl)")
            << " topo=" << (g_split ? "8x32-port-plane-switches" : "1x256-port-switch") << "\n";

  // ---- topo:host 0..7;host 设备序(链路出现序)= k = die*8 + stripe 升序。
  //      split:node 8+m = 平面 m 交换机(32 口,其端口序 = i*4 + die)。 ----
  const std::string topo = "/tmp/sue-vrf-planes-topo.txt";
  {
    std::ofstream t (topo);
    t << "type=SUE\n";
    if (g_split)
      {
        t << "nodes " << NGPU + NVRF << "\n";
        for (uint32_t m = 0; m < NVRF; ++m) { t << "switch " << SWNODE + m << "\n"; }
        for (uint32_t i = 0; i < NGPU; ++i)
          {
            for (uint32_t d = 0; d < NDIE; ++d)
              {
                for (uint32_t m = 0; m < NVRF; ++m)
                  {
                    t << "link " << i << " " << SWNODE + m << " 200Gbps " << llrEna << " " << g_fcMode << " 0\n";
                  }
              }
          }
      }
    else
      {
        t << "nodes 9\n"
          << "switch 8\n";
        for (uint32_t i = 0; i < NGPU; ++i)
          {
            for (uint32_t k = 0; k < NPORT; ++k)
              {
                t << "link " << i << " 8 200Gbps " << llrEna << " " << g_fcMode << " 0\n";
              }
          }
      }
  }

  // ---- conf:全栈 LLR + PORT_TO_VRF_MAP(每 Die 内 stripe→平面)----
  const std::string conf = "/tmp/sue-vrf-planes.conf";
  {
    std::ofstream c (conf);
    c << "ENABLE_SCALEUP 1\n"
      << "SUE_MTP_THREADS " << g_threads << "\n"
      << "SUE_ETH_TYPE_0 0xFCF1\n"
      << "SUE_ETH_TYPE_0_HAS_SHIM 0\n"
      << "SUE_ETH_TYPE_1 0xFCF2\n"
      << "SUE_ETH_TYPE_1_HAS_SHIM 1\n"
      << "SUE_IFG_BYTES 12\n"
      << "SUE_LINK_PROPAGATION_NS 10\n"
      << "SUE_SWITCH_FWD_BUDGET_NS 300\n"
      << "LLR_MODE_LOCAL 1\n"
      << "LLR_MODE_REMOTE 1\n"
      << "LLR_INIT_BEHAVIOR BLOCK\n"
      << "LLR_FLUSH_BEHAVIOR BLOCK\n"
      << "LLR_RE_INIT_ON_DISCARD 1\n"
      << "LLR_OUTSTANDING_SEQ_MAX 65536\n"
      << "LLR_OUTSTANDING_DATA_MAX 1048576\n"
      << "LLR_REPLAY_BUF_MAX 1048576\n"
      << "LLR_REPLAY_TIMER_MAX_NS 20915\n"
      << "LLR_REPLAY_CT_MAX 5\n"
      << "LLR_INIT_TIMER_MAX_NS 20915\n"
      << "LLR_INIT_RETRY_MAX 3\n"
      << "LLR_DATA_AGE_TIMER_MAX_NS 50000\n"
      << "LLR_CTLOS_TARGET_SPACING 2048\n";
    if (g_fcMode == "CBFC")
      {
        c << "FC_MODE CBFC\n"
          << "CBFC_CREDIT_SIZE 256\n"
          << "CBFC_VC_CREDIT_LIMIT 0:512,1:512,2:512,3:512,4:512,5:512,6:512,7:512\n"
          << "CBFC_PKT_OVHD 0\n"
          << "CBFC_CC_MSG_TIMER_US 102\n"
          << "CBFC_CF_THRESHOLD_BYTES 2075\n"
          << "CBFC_CF_MIN_TIMER_BYTES 0\n"
          << "CBFC_CF_MAX_TIMER_BYTES 1250000\n"
          << "CBFC_EGRESS_BUF_BYTES 0\n";
      }
    else
      {
        c << "FC_MODE PFC\n"
          << "PFC_DRAIN_DELAY_NS " << g_pfcDrainNs << "\n"
          << "PFC_DYNAMIC_THRESHOLD 1\n"
          << "PFC_BUFFER_SIZE 65536\n"
          << "PFC_SHARED_POOL_SIZE 32768\n"
          << "PFC_HEADROOM_SIZE 8192\n"
          << "PFC_RESUME_OFFSET 4096\n"
          << "PFC_ALPHA_SHIFT 0\n"
          << "PFC_PAUSE_QUANTA 65535\n";
      }
    if (!g_split)
      {
        c << "PORT_TO_VRF_MAP ";
        const uint32_t swPorts = NGPU * NPORT;
        for (uint32_t q = 0; q < swPorts; ++q)
          {
            if (q) { c << ","; }
            c << q << ":" << (q % NVRF + 1);   // 全局口序 p 映射平面 (p%8)+1
          }
        c << "\n";
      }
    // split 变体留空走 VRF0,平面交换机各自显式 VRF0 表(见下)。
  }

  SueReadConf (conf);
  UintegerValue mtpTv;
  GlobalValue::GetValueByName ("SUE_MTP_THREADS", mtpTv);
  const uint32_t mtpThreads = mtpTv.Get ();
  NS_ABORT_MSG_IF (mtpThreads != g_threads, "SUE_MTP_THREADS conf/env mismatch");
  if (mtpThreads > 1)
    {
      MtpInterface::Enable (mtpThreads);
      Simulator::SetImplementation (CreateObject<MultithreadedSimulatorImpl> ());
    }

  const auto tWall0 = std::chrono::steady_clock::now ();
  SueEntryDispatch (topo, conf);

  // ---- 覆盖路由(自动建路会建跨平面 ECMP 大组,必须覆盖) ----
  if (g_split)
    {
      // §12.45 v2:平面交换机 8+m,VRF0 表 32 条精确路由
      // x=(j,dd) -> 本机端口 i*4+dd(i=j)。设备序即按此布局生成。
      for (uint32_t m = 0; m < NVRF; ++m)
        {
          Ptr<SUESwitchNode> sw = DynamicCast<SUESwitchNode> (SueGetDevice (SWNODE + m, 0)->GetNode ());
          g_sws.push_back (sw);
          Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
          for (uint32_t x = 0; x < NXPU; ++x)
            {
              rt->AddPrefix (x, 10, {x});       // 本机端口 idx == x(见拓扑发射序)
            }
          sw->SetRoutingTable (0, rt);
        }
    }
  else
    {
      g_sws.push_back (DynamicCast<SUESwitchNode> (SueGetDevice (SWNODE, 0)->GetNode ()));
      g_sws[0]->SetRoutingTable (0, CreateObject<SueRoutingTable> ());   // VRF0:无路由 = miss
      for (uint32_t v = 0; v < NVRF; ++v)
        {
          Ptr<SueRoutingTable> rt = CreateObject<SueRoutingTable> ();
          for (uint32_t x = 0; x < NXPU; ++x)
            {
              const uint32_t gpu = x / NDIE, die = x % NDIE;
              rt->AddPrefix (x, 10, {gpu * NPORT + die * PDIE + v});   // x -> 口 (gpu, die, stripe=v)
            }
          g_sws[0]->SetRoutingTable (v + 1, rt);
        }
    }

  // ---- rx attribution + per-host upcall override ----
  for (uint32_t i = 0; i < NGPU; ++i)
    {
      const uint32_t node = SueGetHostNodeId (i);
      for (uint32_t k = 0; k < NPORT; ++k)
        {
          Ptr<SueNetDevice> dev = SueGetDevice (node, k);
          g_devGpuPort[dev] = {i, k};
          dev->SetReceiveUpcall (MakeCallback (&RxObs));
        }
    }

  // ---- all-to-all streams:每 (src 口, dst GPU, dst Die) 一条流 ----
  const uint64_t raw = (uint64_t) g_pairGbits * 1000000000ull / 8 / g_frameBytes;
  g_pairFrames = (raw / (NPORT * NDIE)) * (NPORT * NDIE);   // 128 对齐:N' 整数
  g_perStream = g_pairFrames / (NPORT * NDIE);
  // 注入节拍:每口并发 7目的GPU × 4目的Die = 28 条流轮流占用线速;返回链路
  // 每数据帧要回 2 条 CtlOS(ACK+CF 各 29B 线时),计入 wireBytes(两种 FC
  // 模式同 offered load)。
  const uint64_t wireBytes = g_frameBytes + 14 + 8 + 12 + 2 * (9 + 8 + 12);
  const Time interval = rate.CalculateBytesTxTime (wireBytes) * ((NGPU - 1) * NDIE);
  std::cout << "  pairFrames=" << g_pairFrames << " perStream=" << g_perStream
            << " interval=" << interval.GetNanoSeconds () << "ns wireBytes=" << wireBytes << "\n";
  for (uint32_t i = 0; i < NGPU; ++i)
    {
      const uint32_t node = SueGetHostNodeId (i);
      for (uint32_t k = 0; k < NPORT; ++k)
        {
          const uint32_t sdie = k / PDIE;
          const uint32_t srcXpu = i * NDIE + sdie;
          for (uint32_t j = 0; j < NGPU; ++j)
            {
              if (j == i) { continue; }
              for (uint32_t dd = 0; dd < NDIE; ++dd)
                {
                  Stream *s = new Stream;
                  s->dev = SueGetDevice (node, k);
                  s->srcXpu = srcXpu;
                  s->dstXpu = j * NDIE + dd;
                  s->remaining = g_perStream;
                  s->interval = interval;
                  ++g_streamsLeft;
                  const uint64_t dither = (uint64_t) ((i * NPORT + k) * NGPU * NDIE + j * NDIE + dd);
                  Simulator::ScheduleWithContext (node, TimeStep (1) + NanoSeconds (dither),
                                                  &SendOne, s);
                }
            }
        }
    }

  // ---- rate diagnostics(每 200µs;PFC 加普查 paused 端口) ----
  for (uint32_t t = 200; t <= 5000; t += 200)
    {
      Simulator::ScheduleWithContext (SWNODE, MicroSeconds (t), [] () {
        uint32_t paused = 0;
        if (g_fcMode == "PFC")
          {
            for (uint32_t i = 0; i < NGPU; ++i)
              {
                const uint32_t node = SueGetHostNodeId (i);
                for (uint32_t k = 0; k < NPORT; ++k)
                  {
                    Ptr<SuePfcEngine> a = SueGetDevice (node, k)->GetPfcEngine ();
                    // 单机:交换机口 (i,k) = 全局口 i*NPORT+k;split:平面 k%8 的口 i*4+k/8
                    Ptr<SuePfcEngine> b = SueGetDevice (g_split ? SWNODE + k % NVRF : SWNODE,
                                                        g_split ? i * NDIE + k / PDIE : i * NPORT + k)->GetPfcEngine ();
                    if (a && a->IsPaused (0)) { ++paused; }
                    if (b && b->IsPaused (0)) { ++paused; }
                  }
              }
            g_pauseSamples += paused;
          }
        uint64_t swRx = 0, swTx = 0, pend = 0, pendMax = 0;
        for (auto &sw : g_sws)
          {
            swRx += sw->GetCntRxPkts (); swTx += sw->GetCntTxPkts ();
            pend += sw->GetCntPendPkts (); pendMax += sw->GetCntPendMax ();
          }
        std::cout << "    t=" << Simulator::Now ().GetMicroSeconds ()
                  << "us swRx=" << swRx << " swTx=" << swTx
                  << " pend=" << pend << " pendMax=" << pendMax;
        if (g_fcMode == "PFC") { std::cout << " pausedPorts=" << paused; }
        std::cout << "\n";
      });
    }
  Simulator::Stop (MilliSeconds (12));
  Simulator::Run ();
  const double wallMs = std::chrono::duration<double, std::milli> (
    std::chrono::steady_clock::now () - tWall0).count ();
  std::cout << "VRF-PLANES WALL_MS " << wallMs << "\n";
  Simulator::Destroy ();
  return g_ok ? 0 : 1;
}

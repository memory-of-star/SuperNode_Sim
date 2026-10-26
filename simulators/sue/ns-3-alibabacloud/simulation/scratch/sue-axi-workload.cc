/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-only:nil; -*- */
/*
 * AXI-over-SUE workload: transaction-level GoBackN over 4-VC fabric.
 *
 * Models the scale-up fabric carrying AXI-like DMA traffic:
 *   AW(写地址)+W(写数据) → pcp 1 → VC1
 *   AR(读地址)           → pcp 2 → VC2
 *   B(写响应)            → pcp 3 → VC3
 *   R(读数据)            → pcp 4 → VC4
 * Transaction semantics (no RH header, DESIGN §1.2 预留):
 *   Read:  master sends AR → slave sends K R-data frames; completion on all K;
 *   Write: master sends AW + K W-data → slave sends B on all received; completion on B.
 * Rollback (GoBackN): on timeout, master re-issues entire transaction
 *   (AR again / AW+all W again); attempt counter tracks retry depth.
 * Purpose: measure performance when LLR is OFF (each lost frame → rollback);
 *   with LLR ON the fabric is lossless → rollback ≈ 0, providing the A/B control.
 *
 * Env knobs:
 *   AXI_RATE          link rate (default 200Gbps)
 *   AXI_TXN_BYTES     transaction size (default 65536 = 64KB)
 *   AXI_TXNS          total transactions to issue (default 5000)
 *   AXI_OUTSTANDING   max concurrent transactions (default 32)
 *   AXI_CTRL_BYTES    control frame payload (AW/AR/B, default 128)
 *   AXI_DATA_BYTES    data frame payload (W/R, default 2048)
 *   AXI_LLR           LLR enable (default 1; 0 = no-LLR lossy link)
 *   AXI_LOSS_EVERY    0=none; N=SetBadEvery(N) on both switch ingress ports
 *   AXI_TIMEOUT_NS    per-transaction timeout; 0 = auto (4×K×wire + RTT)
 *   AXI_MAX_RETRY     max rollback attempts per txn (default 8)
 *   AXI_THREADS       MTP thread count (default 1)
 *   AXI_RATE          link rate (default 200Gbps)
 *   AXI_VID           VLAN ID for isolation (default 100)
 *
 * Assertions (self-check PASS/FAIL):
 *   1. All txns completed or abort-after-retries (completed + aborted == issued);
 *   2. LLR=1 arm: retries == 0 (lossless fabric, zero rollback despite injection);
 *   3. LLR=0 arm: retries > 0 when loss > 0 (rollback actually occurs);
 *   4. Per-VC frame counts match expected (computed from txn/retry counts);
 *   5. Switch counters consistent.
 */
#include "ns3/sue-entry.h"
#include "ns3/sue-error-model.h"
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
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include "sue-scratch-util.h"
#include "sue-scratch-pcap.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdlib>

using namespace ns3;

// ---- env defaults ----
static uint32_t g_txnBytes     = 32768;
static uint32_t g_txnCount     = 500;
static uint32_t g_outstanding  = 16;
static uint32_t g_ctrlBytes    = 128;
static uint32_t g_dataBytes    = 512;
static uint32_t g_llr          = 1;
static uint32_t g_lossEvery    = 0;
static uint32_t g_timeoutNs    = 0;
static uint32_t g_maxRetry     = 16;
static uint32_t g_threads      = 1;
static std::string g_rateStr   = "200Gbps";
static uint32_t g_vid          = 100;
static uint32_t g_propNs       = 100;   // link propagation delay (ns)
static bool g_ok = true;
#define CHECK(c, m) do { bool _c=(c); std::cout<<"  ["<<(_c?"ok":"FAIL")<<"] "<<m<<"\n"; g_ok=g_ok&&_c; } while(0)

// ---- PCP channel mapping (AXI → VC) ----
static const uint8_t PCP_AW_W = 1;   // AW + W frames → VC1
static const uint8_t PCP_AR   = 2;   // AR frames → VC2
static const uint8_t PCP_B    = 3;   // B frames → VC3
static const uint8_t PCP_R    = 4;   // R frames → VC4

// ---- metadata layout (12B in payload) ----
static const uint32_t META_LEN = 12;
// [0..1] magic 0xA55A, [2..5] txnId u32, [6..7] seq u16, [8] ch, [9..10] kTotal, [11] attempt
enum ChType : uint8_t { CH_AW = 1, CH_W = 2, CH_AR = 3, CH_B = 4, CH_R = 5 };

static void WriteMeta (Ptr<Packet> p, uint32_t txnId, uint16_t seq, uint8_t ch,
                       uint16_t kTotal, uint8_t attempt)
{
  uint8_t buf[META_LEN];
  buf[0] = 0xA5; buf[1] = 0x5A;
  std::memcpy(buf+2, &txnId, 4);
  std::memcpy(buf+6, &seq,   2);
  buf[8] = ch;
  std::memcpy(buf+9, &kTotal, 2);
  buf[11] = attempt;
  p->AddAtEnd (Create<Packet> (buf, META_LEN));   // 会增加在载荷尾部? 需要在 AddHeader 前
  // 实际：在 Packet 构建时先加 meta 再加 header; 但 SueHeader 已 addHeader.
  // 改用 prepend: 先 AddHeader(SueHeader) 后再把 metadata 写进 payload 前 12B
  // 更简洁：构建 payload 时在首 12B 写 metadata，剩余是零填充。
  // 修正见下方。
}

// ---- 改为：构建带 metadata 的 payload 包 ----
static Ptr<Packet> MakeFrame (uint32_t payloadSize, uint32_t txnId, uint16_t seq,
                              uint8_t ch, uint16_t kTotal, uint8_t attempt,
                              uint8_t pcp, uint16_t vid)
{
  // 64B Ethernet minimum frame: SueHeader(14B) + VLAN(4B) + payload ≥ 64B
  // → minimum payload = 64 - 18 = 46B (with 12B metadata, 34B zero padding)
  const uint32_t MIN_PAYLOAD = 64 - 18; // 46B
  if (payloadSize < META_LEN + MIN_PAYLOAD) { payloadSize = META_LEN + MIN_PAYLOAD; }
  // payload: [META_LEN bytes] + [payloadSize - META_LEN bytes zero padding]
  NS_ASSERT (payloadSize > META_LEN);
  uint8_t *buf = new uint8_t[payloadSize] ();
  std::memcpy(buf+2, &txnId, 4);
  std::memcpy(buf+6, &seq,   2);
  buf[0] = 0xA5; buf[1] = 0x5A;
  buf[8] = ch;
  std::memcpy(buf+9, &kTotal, 2);
  buf[11] = attempt;
  Ptr<Packet> p = Create<Packet> (buf, payloadSize);
  delete[] buf;
  SueHeader h;
  h.SetDst (SueMacFromXpuId (1));   // slave XPU=1
  h.SetSrc (SueMacFromXpuId (0));   // master XPU=0
  h.SetEtherType (SUE_ETH_TYPE_0);  // 0xFCF1, no shim (use VLAN for VC)
  h.SetVlan (true, pcp, 0, vid);
  p->AddHeader (h);
  return p;
}

static void ReadMeta (const uint8_t *buf, uint32_t &txnId, uint16_t &seq,
                      uint8_t &ch, uint16_t &kTotal, uint8_t &attempt)
{
  std::memcpy(&txnId, buf+2, 4);
  std::memcpy(&seq,   buf+6, 2);
  ch = buf[8];
  std::memcpy(&kTotal, buf+9, 2);
  attempt = buf[11];
}

// ---- node topology ----
static const uint32_t NMASTER = 0;   // host0 = master (XPU 0)
static const uint32_t NSLAVE  = 1;   // host1 = slave  (XPU 1)
static const uint32_t NSWITCH = 2;   // switch node

// ---- transaction state (master-side) ----
struct Txn {
  uint32_t id;
  uint32_t kTotal;      // data frames per txn
  uint32_t attempts;    // retry depth
  uint64_t issueTime;
  uint64_t deadline;
  bool isWrite;
  // read: collect R frame seqs
  uint32_t rReceived;
  std::vector<bool> rSeqGot;
  // write: wait for B
  bool bReceived;
  bool completed;
  bool aborted;
  uint64_t doneTime;
};
static std::map<uint32_t, Txn> g_txns;
static uint32_t g_issued    = 0;
static uint32_t g_completed = 0;
static uint32_t g_retries   = 0;
static uint32_t g_aborted   = 0;
static uint64_t g_totalLatencyNs = 0;

// ---- per-channel frame counters (master TX/RX + switch) ----
// ch: 0=AW,1=W,2=AR,3=B,4=R  (internal; ChType 1..5 → index chType-1)
static uint64_t g_chTx[5] = {};   // master TX per channel
static std::atomic<uint64_t> g_chRx[5] = {};   // master RX per channel (B/R, concurrent)
static uint64_t g_rollbacksByType[2] = {};       // [0]=read, [1]=write

// ---- scheduling helpers ----
static void MasterSendAR (uint32_t txnId, uint32_t kTotal, uint8_t attempt);
static void MasterSendAW (uint32_t txnId, uint32_t kTotal, uint8_t attempt);
static void MasterSendW  (uint32_t txnId, uint16_t seq, uint32_t kTotal, uint8_t attempt);
static void IssueTxn (uint32_t txnId);
static void CheckTimeouts (void);
static void MasterRx (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h);
static void SlaveRx  (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h);

// ---- globals ----
static Ptr<SUESwitchNode> g_sw;
static DataRate g_rate;
static Time g_timeout;
static uint32_t g_dataPayload;   // data frame payload size (g_dataBytes - 14)
static uint32_t g_ctrlPayload;
static uint32_t g_nextTxnId = 0;
static uint32_t g_inFlight  = 0;
static uint32_t g_targetTxns;     // = g_txnCount
static std::atomic<uint64_t> g_slaveTx[5] = {};  // slave TX per channel (concurrent)

// ---- implementation ----

static void MasterSendAR (uint32_t txnId, uint32_t kTotal, uint8_t attempt)
{
  Ptr<SueNetDevice> dev = SueGetDevice (NMASTER, 0);
  Ptr<Packet> p = MakeFrame (g_ctrlPayload, txnId, 0, CH_AR, kTotal, attempt,
                             PCP_AR, g_vid);
  dev->SwitchSend (PCP_AR, p);     // qIndex = VC
  ++g_chTx[CH_AR - 1];
}

static void MasterSendAW (uint32_t txnId, uint32_t kTotal, uint8_t attempt)
{
  Ptr<SueNetDevice> dev = SueGetDevice (NMASTER, 0);
  Ptr<Packet> p = MakeFrame (g_ctrlPayload, txnId, 0, CH_AW, kTotal, attempt,
                             PCP_AW_W, g_vid);
  dev->SwitchSend (PCP_AW_W, p);   // qIndex = VC (PCP→VC identity map)
  ++g_chTx[CH_AW - 1];
}

static void MasterSendW (uint32_t txnId, uint16_t seq, uint32_t kTotal, uint8_t attempt)
{
  Ptr<SueNetDevice> dev = SueGetDevice (NMASTER, 0);
  Ptr<Packet> p = MakeFrame (g_dataPayload, txnId, seq, CH_W, kTotal, attempt,
                             PCP_AW_W, g_vid);
  dev->SwitchSend (PCP_AW_W, p);   // qIndex = VC
  ++g_chTx[CH_W - 1];
}

// slave sends R frames (self-chained on slave node LP)
static void SlaveSendR (uint32_t txnId, uint16_t seq, uint32_t kTotal, uint8_t attempt)
{
  Ptr<SueNetDevice> dev = SueGetDevice (NSLAVE, 0);
  // R frame goes back to master: swap src/dst in MakeFrame needs adjustment;
  // build manually: dst=XPU0, src=XPU1
  uint8_t *buf = new uint8_t[g_dataPayload] ();
  std::memcpy(buf+2, &txnId, 4);
  std::memcpy(buf+6, &seq,   2);
  buf[0] = 0xA5; buf[1] = 0x5A;
  buf[8] = CH_R;
  uint16_t k = kTotal; std::memcpy(buf+9, &k, 2);
  buf[11] = attempt;
  Ptr<Packet> p = Create<Packet> (buf, g_dataPayload);
  delete[] buf;
  SueHeader h;
  h.SetDst (SueMacFromXpuId (0));
  h.SetSrc (SueMacFromXpuId (1));
  h.SetEtherType (SUE_ETH_TYPE_0);
  h.SetVlan (true, PCP_R, 0, g_vid);
  p->AddHeader (h);
  dev->SwitchSend (PCP_R, p);      // qIndex = VC
  ++g_slaveTx[CH_R - 1];
}

// slave sends B frame
static void SlaveSendB (uint32_t txnId, uint16_t seq, uint32_t kTotal, uint8_t attempt)
{
  uint8_t *buf = new uint8_t[g_ctrlPayload] ();
  std::memcpy(buf+2, &txnId, 4);
  std::memcpy(buf+6, &seq,   2);
  buf[0] = 0xA5; buf[1] = 0x5A;
  buf[8] = CH_B;
  uint16_t k = kTotal; std::memcpy(buf+9, &k, 2);
  buf[11] = attempt;
  Ptr<Packet> p = Create<Packet> (buf, g_ctrlPayload);
  delete[] buf;
  SueHeader h;
  h.SetDst (SueMacFromXpuId (0));
  h.SetSrc (SueMacFromXpuId (1));
  h.SetEtherType (SUE_ETH_TYPE_0);
  h.SetVlan (true, PCP_B, 0, g_vid);
  p->AddHeader (h);
  SueGetDevice (NSLAVE, 0)->SwitchSend (PCP_B, p);   // qIndex = VC
  ++g_slaveTx[CH_B - 1];
}

// Issue a new transaction (runs on master LP)
static void IssueTxn (uint32_t txnId)
{
  Txn t;
  t.id = txnId;
  t.kTotal = (g_txnBytes + g_dataPayload - 1) / g_dataPayload;
  t.attempts = 0;
  t.issueTime = Simulator::Now ().GetNanoSeconds ();
  t.isWrite = (txnId % 2 == 0);   // alternate read/write
  t.rReceived = 0;
  t.rSeqGot.resize (t.kTotal, false);
  t.bReceived = false;
  t.completed = false;
  t.aborted   = false;
  t.doneTime  = 0;
  uint64_t wireNs = g_rate.CalculateBytesTxTime (g_dataBytes + 14 + 8 + 12).GetNanoSeconds ();
  uint64_t timeout = g_timeoutNs;
  // 200×K×wire + 4×RTT(数据+ACK+CF 退款往返)+ 固定余量;RTT 项随
  // AXI_PROP_NS 缩放,长 RTT 下不误超时(§12.48)。
  if (timeout == 0) { timeout = 500ULL * t.kTotal * wireNs + 16ULL * g_propNs + 500000; }
  t.deadline = t.issueTime + timeout;
  g_txns[txnId] = t;
  ++g_issued;

  if (t.isWrite)
    {
      MasterSendAW (txnId, t.kTotal, 0);
      // schedule K W data frames back-to-back (same pattern as rollback re-issue)
      const uint64_t wireNs = g_rate.CalculateBytesTxTime (g_dataBytes + 14 + 8 + 12).GetNanoSeconds ();
      for (uint16_t i = 0; i < t.kTotal; ++i)
        {
          Simulator::ScheduleWithContext (NMASTER, NanoSeconds (wireNs * i),
                                          &MasterSendW, txnId, i, t.kTotal, (uint8_t)0);
        }
    }
  else { MasterSendAR (txnId, t.kTotal, 0); }
}

// Master RX callback (host0 node LP)
static void MasterRx (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h)
{
  uint32_t pktSize = p->GetSize ();
  if (pktSize < 14 + META_LEN) return;
  p->RemoveHeader (h);
  uint8_t *buf = new uint8_t[pktSize - 14];
  p->CopyData (buf, pktSize - 14);
  uint32_t txnId; uint16_t seq, kTotal; uint8_t ch, attempt;
  ReadMeta (buf, txnId, seq, ch, kTotal, attempt);
  delete[] buf;

  auto it = g_txns.find (txnId);
  if (it == g_txns.end ()) return;
  Txn &t = it->second;
  if (t.completed || t.aborted) return;
  if (attempt != t.attempts) return;   // stale response from prior attempt

  if (ch == CH_B && t.isWrite)
    {
      // Write complete
      t.completed = true;
      t.doneTime = Simulator::Now ().GetNanoSeconds ();
      g_totalLatencyNs += (t.doneTime - t.issueTime);
      ++g_completed;
      --g_inFlight;
      ++g_chRx[CH_B - 1];   // atomic fetch_add
      // schedule next issue if more to go
      if (g_nextTxnId < g_targetTxns)
        {
          Simulator::ScheduleWithContext (NMASTER, NanoSeconds (1),
                                          &IssueTxn, g_nextTxnId++);
        }
    }
  else if (ch == CH_R && !t.isWrite)
    {
      // Read data frame
      if (seq < t.kTotal && !t.rSeqGot[seq])
        {
          t.rSeqGot[seq] = true;
          t.rReceived++;
          ++g_chRx[CH_R - 1];
        }
      if (t.rReceived == t.kTotal)
        {
          // Read complete
          t.completed = true;
          t.doneTime = Simulator::Now ().GetNanoSeconds ();
          g_totalLatencyNs += (t.doneTime - t.issueTime);
          ++g_completed;
          --g_inFlight;
          if (g_nextTxnId < g_targetTxns)
            {
              Simulator::ScheduleWithContext (NMASTER, NanoSeconds (1),
                                              &IssueTxn, g_nextTxnId++);
            }
        }
    }
}

// Slave RX callback (host1 node LP)
static void SlaveRx (Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h)
{
  uint32_t pktSize = p->GetSize ();
  if (pktSize < 14 + META_LEN) return;
  p->RemoveHeader (h);
  uint8_t *buf = new uint8_t[pktSize - 14];
  p->CopyData (buf, pktSize - 14);
  uint32_t txnId; uint16_t seq, kTotal; uint8_t ch, attempt;
  ReadMeta (buf, txnId, seq, ch, kTotal, attempt);
  delete[] buf;

  if (ch == CH_AR)
    {
      // Read request: send K R frames back (self-chained on slave LP)
      for (uint16_t i = 0; i < kTotal; ++i)
        {
          Simulator::Schedule (g_rate.CalculateBytesTxTime (g_dataBytes + 14 + 8 + 12) * i,
                               &SlaveSendR, txnId, i, kTotal, attempt);
        }
    }
  else if (ch == CH_AW)
    {
      // Write request (slave resets state on new attempt)
      // Nothing to send yet; just record; W frames will follow.
    }
  else if (ch == CH_W)
    {
      // Write data frame; track per-txn received count;
      // on last W (seq == kTotal-1) send B (implying full receive)
      if (seq == kTotal - 1)
        {
          SlaveSendB (txnId, 0, kTotal, attempt);
        }
    }
}

// Timeout scanner (runs on master LP, periodic)
static void CheckTimeouts (void)
{
  uint64_t now = Simulator::Now ().GetNanoSeconds ();
  for (auto &pr : g_txns)
    {
      Txn &t = pr.second;
      if (t.completed || t.aborted) continue;
      if (now < t.deadline) continue;
      // rollback (GoBackN)
      ++g_retries;
      ++g_rollbacksByType[t.isWrite ? 1 : 0];
      t.attempts++;
      if (t.attempts > g_maxRetry)
        {
          t.aborted = true;
          ++g_aborted;
          --g_inFlight;
          if (g_nextTxnId < g_targetTxns)
            {
              Simulator::ScheduleWithContext (NMASTER, NanoSeconds (1),
                                              &IssueTxn, g_nextTxnId++);
            }
          continue;
        }
      // reset state and re-issue
      uint64_t wireNs = g_rate.CalculateBytesTxTime (g_dataBytes + 14 + 8 + 12).GetNanoSeconds ();
      // 重试用短超时(丢帧快速恢复):K×wire×10 + 4×RTT + 50µs 余量。
      // IssueTxn 的长超时(200×K×wire)覆盖 CBFC 背压;重试场景是丢帧,
      // 帧已在队列,快速重发即可(§12.48)。
      uint64_t timeout = g_timeoutNs ? g_timeoutNs : 500ULL * t.kTotal * wireNs + 16ULL * g_propNs + 500000;
      t.deadline = now + timeout;
      t.issueTime = now;
      if (!t.isWrite)
        {
          // re-issue read
          t.rReceived = 0;
          std::fill (t.rSeqGot.begin (), t.rSeqGot.end (), false);
          MasterSendAR (t.id, t.kTotal, t.attempts);
        }
      else
        {
          // re-issue write
          t.bReceived = false;
          MasterSendAW (t.id, t.kTotal, t.attempts);
          // schedule K W frames back-to-back
          for (uint16_t i = 0; i < t.kTotal; ++i)
            {
              Simulator::ScheduleWithContext (NMASTER,
                                              NanoSeconds (wireNs * i),
                                              &MasterSendW, t.id, i,
                                              t.kTotal, t.attempts);
            }
        }
    }
}

int
main ()
{
  // ---- parse env ----
  if (const char *e = std::getenv ("AXI_RATE"))           { g_rateStr = e; }
  if (const char *e = std::getenv ("AXI_TXN_BYTES"))      { g_txnBytes = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_TXNS"))           { g_txnCount = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_OUTSTANDING"))    { g_outstanding = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_CTRL_BYTES"))     { g_ctrlBytes = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_DATA_BYTES"))     { g_dataBytes = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_LLR"))            { g_llr = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_LOSS_EVERY"))     { g_lossEvery = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_TIMEOUT_NS"))     { g_timeoutNs = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_MAX_RETRY"))      { g_maxRetry = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_THREADS"))        { g_threads = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_VID"))            { g_vid = std::atoi (e); }
  if (const char *e = std::getenv ("AXI_PROP_NS"))       { g_propNs = std::atoi (e); }
  static uint32_t fwdNs = 300;
  if (const char *e = std::getenv ("AXI_FWD_NS"))        { fwdNs = std::atoi (e); }
  // UEC §5.1.4 replay_timer 建议 10×LLR RTT(NACK 丢失兜底);0 = auto 推导。
  // 固定 50µs 在长 RTT 下把"重放中又坏帧"的恢复拉到每轮 50µs(prop=700ns
  // 实测 14.6→38.4Gbps,§12.53)。
  static uint32_t replayNs = 0;
  if (const char *e = std::getenv ("AXI_REPLAY_TIMER_NS")) { replayNs = std::atoi (e); }
  static std::string fcMode = "CBFC";
  if (const char *e = std::getenv ("AXI_FC"))            { fcMode = e; }
  if (g_ctrlBytes <= META_LEN) { g_ctrlBytes = META_LEN + 64; }
  if (g_dataBytes <= META_LEN) { g_dataBytes = META_LEN + 256; }
  g_ctrlPayload = g_ctrlBytes;   // payload before SueHeader
  g_dataPayload = g_dataBytes;
  g_rate        = DataRate (g_rateStr);
  g_targetTxns  = g_txnCount;
  const uint32_t k = (g_txnBytes + g_dataPayload - 1) / g_dataPayload;

  std::cout << std::unitbuf;
  std::cout << "AXI-WORKLOAD txn=" << g_txnBytes << "B txns=" << g_txnCount
            << " out=" << g_outstanding << " K=" << k << " rate=" << g_rateStr
            << " loss=1/" << g_lossEvery << " llr=" << g_llr
            << " threads=" << g_threads << "\n";

  // ---- topo: host0 -- switch2 -- host1 ----
  const std::string topo = "/tmp/sue-axi-topo.txt";
  {
    std::ofstream t (topo);
    t << "type=SUE\n"
      << "nodes 3\n"
      << "switch 2\n"
      << "link 0 2 " << g_rateStr << " " << g_llr << " " << fcMode << " 0\n"
      << "link 1 2 " << g_rateStr << " " << g_llr << " " << fcMode << " 0\n";
  }

  // ---- conf: FCFC2 no-shim + VLAN PCP → 4 VCs + CBFC per-VC ----
  const std::string conf = "/tmp/sue-axi.conf";
  {
    std::ofstream c (conf);
    c << "ENABLE_SCALEUP 1\n"
      << "SUE_MTP_THREADS " << g_threads << "\n"
      << "SUE_ETH_TYPE_0 0xFCF1\n"
      << "SUE_ETH_TYPE_0_HAS_SHIM 0\n"
      << "SUE_ETH_TYPE_1 0xFCF2\n"
      << "SUE_ETH_TYPE_1_HAS_SHIM 1\n"
      << "SUE_IFG_BYTES 12\n"
      << "SUE_LINK_PROPAGATION_NS " << g_propNs << "\n"
      << "SUE_SWITCH_FWD_BUDGET_NS " << fwdNs << "\n"
      << "LLR_MODE_LOCAL " << g_llr << "\n"
      << "LLR_MODE_REMOTE " << g_llr << "\n"
      << "LLR_INIT_BEHAVIOR BLOCK\n"
      << "LLR_FLUSH_BEHAVIOR BLOCK\n"
      << "LLR_RE_INIT_ON_DISCARD 1\n"
      << "LLR_OUTSTANDING_SEQ_MAX 65536\n"
      << "LLR_OUTSTANDING_DATA_MAX 1048576\n"
      << "LLR_REPLAY_BUF_MAX 1048576\n"
      << "LLR_REPLAY_TIMER_MAX_NS " << replayNs << "\n"
      << "LLR_REPLAY_CT_MAX 5\n"
      << "LLR_INIT_TIMER_MAX_NS 50000\n"
      // 数据年龄上限 4s(同 thpt):只有真断链才触发 FLUSH。默认 50µs 在长 RTT
      // 下会把 replay 风暴中滞留的帧误判为过期 -> FLUSH 丢帧 -> 事务回滚风暴。
      << "LLR_DATA_AGE_TIMER_MAX_NS 4000000000\n"
      << "LLR_CTLOS_TARGET_SPACING 2048\n"
      << "FC_MODE CBFC\n"
      << "CBFC_CREDIT_SIZE 256\n"
      << "CBFC_VC_CREDIT_LIMIT 0:4096,1:4096,2:4096,3:4096,4:4096\n"
      << "CBFC_CC_MSG_TIMER_US 102\n"
      << "CBFC_CF_THRESHOLD_BYTES 2075\n"
      // CF_min=0:阈值量子(2075B)触发即发,退款最频繁(实测 10µs 节拍
      // 无差别,v3 验证);瓶颈在 CtlOS 时隙被 ACK 流占满(§12.48)。
      << "CBFC_CF_MIN_TIMER_BYTES 0\n"
      << "CBFC_CF_MAX_TIMER_BYTES 1250000\n"
      << "PCP_TO_VC_MAP 1:1,2:2,3:3,4:4\n";   // AW+W→VC1, AR→VC2, B→VC3, R→VC4
  }

  // ---- MTP enable ----
  SueReadConf (conf);
  UintegerValue mtpTv;
  GlobalValue::GetValueByName ("SUE_MTP_THREADS", mtpTv);
  const uint32_t mtpThreads = mtpTv.Get ();
  if (mtpThreads > 1)
    {
      MtpInterface::Enable (mtpThreads);
      Simulator::SetImplementation (CreateObject<MultithreadedSimulatorImpl> ());
    }

  const auto tWall0 = std::chrono::steady_clock::now ();
  SueEntryDispatch (topo, conf, nullptr);

  g_sw = DynamicCast<SUESwitchNode> (SueGetDevice (NSWITCH, 0)->GetNode ());

  // ---- LLR=0 legacy wiring (all devices) ----
  if (!g_llr)
    {
      for (uint32_t n = 0; n < 3; ++n)
        {
          for (uint32_t d = 0; d < SueGetDevice (n, 0)->GetNode ()->GetNDevices (); ++d)
            {
              Ptr<SueNetDevice> dev = SueGetDevice (n, d);
              if (dev) { dev->SetLlrEnabled (true); }
            }
        }
    }

  // ---- loss injection ----
  if (g_lossEvery > 0)
    {
      Ptr<SueErrorModel> em0 = CreateObject<SueErrorModel> ();
      em0->SetBadEvery (g_lossEvery);
      SueGetDevice (NSWITCH, 0)->SetErrorModel (em0);   // host0→switch ingress
      Ptr<SueErrorModel> em1 = CreateObject<SueErrorModel> ();
      em1->SetBadEvery (g_lossEvery);
      SueGetDevice (NSWITCH, 1)->SetErrorModel (em1);   // host1→switch ingress
    }

  // ---- register RX upcalls ----
  SueGetDevice (NMASTER, 0)->SetReceiveUpcall (MakeCallback (&MasterRx));
  SueGetDevice (NSLAVE,  0)->SetReceiveUpcall (MakeCallback (&SlaveRx));

  // ---- bypass pcap (SUE_PCAP=1 activates; default off) ----
  // Attach to the two links: host0↔switch and host1↔switch.
  // Each generates a .pcap + .txt pair for Wireshark / text review.
  // FLIT metadata (txnId/seq/ch/attempt) is visible in .txt's
  // payload hex dump (first 12B after SueHeader); with
  // sue-wireshark-plugin.lua on DLT_EN10MB, the SueHeader + VLAN
  // PCP are parsed directly.
  // SueScratchPcapAttach expects Ptr<SueChannel>; GetChannel() returns
  // Ptr<Channel> base — DynamicCast to SueChannel (the underlying type
  // created by SueEntryDispatch per DESIGN §6.13).
  Ptr<SueChannel> ch0 = SueGetDevice (NMASTER, 0)->GetSueChannel ();
  Ptr<SueChannel> ch1 = SueGetDevice (NSLAVE,  0)->GetSueChannel ();
  SueScratchPcapAttach (ch0, "axi-master-switch");
  SueScratchPcapAttach (ch1, "axi-slave-switch");

  // ---- issue initial batch of transactions ----
  for (uint32_t i = 0; i < g_outstanding && i < g_txnCount; ++i)
    {
      Simulator::ScheduleWithContext (NMASTER, NanoSeconds (1), &IssueTxn, g_nextTxnId++);
      ++g_inFlight;
    }

  // ---- timeout checker periodic on master LP ----
  for (uint64_t t = 100000; t <= 800000000; t += 100000)   // every 100µs for 800ms (cover Stop=1s)
    {
      Simulator::ScheduleWithContext (NMASTER, NanoSeconds (t), &CheckTimeouts);
    }

  Simulator::Stop (MilliSeconds (1000));
  Simulator::Run ();

  // ---- verify ----
  const uint64_t now = Simulator::Now ().GetNanoSeconds ();
  const double wallMs = std::chrono::duration<double, std::milli> (
      std::chrono::steady_clock::now () - tWall0).count ();

  // expected per-channel frame counts (from issued txns and retries)
  // tracked in g_chTx/slaveTx; per-VC on switch = sum of frames in that VC
  // VC1=AW+W: g_chTx[0] master TX AW + g_chTx[1] master TX W
  // VC2=AR:   g_chTx[2] master TX AR
  // VC3=B:    g_slaveTx[3] slave TX B
  // VC4=R:    g_slaveTx[4] slave TX R
  // on the switch, total frames per VC = sum of both directions for that VC channel
  // master TX AW/W/AR/B/R via device(0,0); slave TX via device(1,0)
  // switch ingress (2,0) receives from host0: AW, W, AR (master TX)
  // switch ingress (2,1) receives from host1: B, R   (slave TX)

  std::cout << "\n=== AXI WORKLOAD RESULT ===\n";
  std::cout << "issued=" << g_issued << " completed=" << g_completed
            << " retries=" << g_retries << " aborted=" << g_aborted << "\n";
  std::cout << "rollbacks read=" << g_rollbacksByType[0]
            << " write=" << g_rollbacksByType[1] << "\n";

  if (g_completed > 0)
    {
      const double avgLat = (double) g_totalLatencyNs / g_completed / 1e6; // ms
      std::cout << "avg latency=" << avgLat << "ms\n";
    }

  // ---- bandwidth utilization (based on actual completion time) ----
  // Find actual completion time: from first issue to last completion
  uint64_t firstIssueNs = UINT64_MAX, lastDoneNs = 0;
  for (auto &pr : g_txns) {
    if (pr.second.issueTime < firstIssueNs) firstIssueNs = pr.second.issueTime;
    if (pr.second.doneTime > lastDoneNs && pr.second.completed) lastDoneNs = pr.second.doneTime;
  }
  const double actualDurationS = (lastDoneNs > firstIssueNs) ? (lastDoneNs - firstIssueNs) * 1e-9 : 1e-9;
  const uint64_t usefulBytes = g_completed * g_txnBytes;
  const double usefulBw = usefulBytes * 8.0 / actualDurationS / 1e9; // Gbps
  const double lineBw = 200.0; // Gbps
  const double utilization = usefulBw / lineBw * 100.0;
  // total bytes on wire = all frames (useful + retry overhead)
  const uint64_t totalTxBytes = (g_chTx[0] + g_chTx[1]) * g_dataBytes + g_chTx[2] * g_ctrlBytes;
  const double totalBw = totalTxBytes * 8.0 / actualDurationS / 1e9; // Gbps
  const double linkUtil = totalBw / lineBw * 100.0;
  // overhead ratio: extra bytes due to retries
  const uint64_t usefulTxBytes = g_completed * (g_dataBytes * ((g_txnBytes + g_dataPayload - 1) / g_dataPayload) + g_ctrlBytes); // ideal
  const double overheadRatio = (usefulTxBytes > 0) ? (double)totalTxBytes / usefulTxBytes : 1.0;

  std::cout << "bandwidth: useful=" << usefulBytes << "B total_tx=" << totalTxBytes << "B actual_time=" << actualDurationS*1e6 << "us useful_bw=" << usefulBw << "Gbps link_util=" << linkUtil << "%\n";
  std::cout << "timing: first_issue=" << firstIssueNs << " last_done=" << lastDoneNs << " overhead=" << overheadRatio << "x\n";

  // per-channel frame counters
  std::cout << "master TX: AW=" << g_chTx[0] << " W=" << g_chTx[1]
            << " AR=" << g_chTx[2] << "\n";
  std::cout << "master RX: B=" << g_chRx[CH_B-1].load ()
            << " R=" << g_chRx[CH_R-1].load () << "\n";
  std::cout << "slave  TX: B=" << g_slaveTx[CH_B-1].load ()
            << " R=" << g_slaveTx[CH_R-1].load () << "\n";

  // switch per-VC
  for (uint32_t vc = 0; vc < 8; ++vc)
    {
      uint64_t v = g_sw->GetCntVcRx (vc);
      if (v > 0) { std::cout << "  switch VC" << vc << " rx=" << v << "\n"; }
    }
  std::cout << "switch pend=" << g_sw->GetCntPendPkts ()
            << " pendMax=" << g_sw->GetCntPendMax ()
            << " rx=" << g_sw->GetCntRxPkts ()
            << " tx=" << g_sw->GetCntTxPkts () << "\n";

  // ---- device LLR/CBFC counters (diagnostic) ----
  for (uint32_t n = 0; n < 3; ++n)
    {
      for (uint32_t d = 0; d < SueGetDevice (n, 0)->GetNode ()->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = SueGetDevice (n, d);
          if (!dev) { continue; }
          std::cout << "dev[" << n << "][" << d << "] ";
          if (dev->GetLlrEngine ())
            {
              const SueLlrCounters &c = dev->GetLlrEngine ()->GetCounters ();
              std::cout << "llr{tx_ok=" << c.tx_ok << " rx_ok=" << c.rx_ok
                        << " replay=" << c.tx_replay << " discard=" << c.tx_discard
                        << " nack_tx=" << c.tx_nack_ctl_os << " nack_rx=" << c.rx_nack_ctl_os
                        << " rx_bad=" << c.rx_bad << " rx_missing=" << c.rx_missing_seq
                        << " rx_dup=" << c.rx_duplicate_seq << "} ";
            }
          if (dev->GetCbfcEngine ())
            {
              const SueCbfcCounters &c = dev->GetCbfcEngine ()->GetCounters ();
              uint64_t exh = 0;
              for (int v = 0; v < 8; ++v) { exh += c.credit_exhausted[v]; }
              std::cout << "cbfc{cf_tx=" << c.cf_update_tx << " cf_rx=" << c.cf_update_rx
                        << " cc_tx=" << c.cc_update_tx << " cc_rx=" << c.cc_update_rx
                        << " exh=" << exh << "}";
            }
          std::cout << "\n";
        }
    }

  // ---- assertions ----
  CHECK (g_completed + g_aborted == g_issued,
         "all txns accounted for (issued=" << g_issued
         << " completed=" << g_completed << " aborted=" << g_aborted << ")");

  if (g_llr && g_lossEvery > 0)
    {
      CHECK (g_retries == 0,
             "LLR=1: zero rollback despite loss=1/" << g_lossEvery
             << " (retries=" << g_retries << ")");
    }
  else if (!g_llr && g_lossEvery > 0)
    {
      // Only assert retries>0 when enough frames to guarantee ≥1 loss
      // (500 txn × 17 frames = 8500; loss=100 → ≥1; loss=10000 → <1 possible)
      if (g_lossEvery <= 1000)
        {
          CHECK (g_retries > 0,
                 "LLR=0: rollback actually occurred (retries=" << g_retries << ")");
        }
      CHECK (g_aborted == 0,
             "LLR=0: no txn permanently aborted (maxRetry=" << g_maxRetry << ")");
    }

  // ---- CBFC credit audit (§12.49): absolute counters, both ends ----
  {
    Ptr<SueCbfcEngine> mcb = SueGetDevice (0, 0)->GetCbfcEngine ();
    Ptr<SueCbfcEngine> scb = SueGetDevice (2, 0)->GetCbfcEngine ();
    if (mcb && scb)
      {
        for (uint32_t v = 0; v < 4; ++v)
          {
            std::cout << "AUDIT vc" << v << ": master CC=" << mcb->GetSvcCc (v)
                      << " CF=" << mcb->GetSvcCf (v) << " CU=" << mcb->GetSvcCu (v)
                      << " | switch RCC=" << scb->GetRvcCc (v)
                      << " RCF=" << scb->GetRvcCf (v) << "\n";
            // §12.50 credit audit as a HARD assertion: charge == bill ==
            // refund on the same VC; CU bounded (no mod-2^15 flip).
            if (mcb->GetSvcCc (v) > 0)
              {
                CHECK (mcb->GetSvcCc (v) == scb->GetRvcCc (v),
                       "credit audit vc" << v << ": master charge == switch bill ("
                       << mcb->GetSvcCc (v) << " vs " << scb->GetRvcCc (v) << ")");
                CHECK (mcb->GetSvcCf (v) == scb->GetRvcCf (v),
                       "credit audit vc" << v << ": master adopted CF == switch refunded CF");
                CHECK (mcb->GetSvcCu (v) <= 4096,
                       "credit audit vc" << v << ": CU bounded (got " << mcb->GetSvcCu (v) << ")");
              }
          }
      }
  }
  std::cout << "AXI-WORKLOAD WALL_MS " << wallMs << "\n";
  std::cout << (g_ok ? "AXI-WORKLOAD PASS" : "AXI-WORKLOAD FAIL") << std::endl;

  Simulator::Destroy ();
  return g_ok ? 0 : 1;
}

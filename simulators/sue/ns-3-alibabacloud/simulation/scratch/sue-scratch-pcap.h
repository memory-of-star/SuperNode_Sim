/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE scratch 旁路抓包工具(默认关闭的调试能力,不改变任何仿真行为)。
 *
 * 用法:scratch 驱动在拓扑建好后对每条要抓的链路调一次
 *   SueScratchPcapAttach (ch1, "h1-sw");      // ch1 为 Ptr<SueChannel>
 * 开关未打开时该调用是 no-op(零开销、零行为差异)。运行时打开:
 *   SUE_PCAP=1 [SUE_PCAP_DIR=.] [SUE_PCAP_PREFIX=sue] [SUE_PCAP_DLT=en10mb] ./ns3-...-scratch
 * 每个 channel 生成两个文件:
 *   <prefix>-<tag>.pcap  标准 pcap,帧字节为 Packet::CopyData 的线字节;
 *   <prefix>-<tag>.txt   人类可读摘要(时间/收发设备/长度/AFH 或控制帧解析/
 *                        前 32B hex)。
 *
 * DLT 选择(SUE_PCAP_DLT,默认 en10mb):
 * - en10mb:DLT 1。纯 AFH 链路(如 afh-forward)在 Wireshark 里可直接看
 *   eth/VLAN 层,装 tools/sue-wireshark-plugin.lua 后 shim 也解析;但 CtlOS
 *   (9B 无 MAC 头)与带 LLR 前导的帧会被标准以太解析判为 malformed;
 * - user0:DLT 147(DLT_USER0)。Wireshark 把整帧交给 sue 插件按 SUE 链路
 *   线格式解析(前导/CtlOS/AFH/CC/PFC 全部支持)。抓 LLR/CBFC 交互过程
 *   (如 cc-update-onwire)应使用本模式。未装插件时显示为原始字节。
 *
 * 捕获点:SueChannel 的 "TxRxSue" trace source——每帧上线(任一方向)回调
 * 一次,含 (packet, src dev, dst dev, txTime, rxDelta)。注意标准
 * PointToPointHelper::EnablePcap 抓不到 SUE 流量:SUE 数据面绕过基类
 * 收发路径(SueChannel 静态分派 &SueNetDevice::Receive),必须走本工具。
 *
 * .txt 字段级解析(解析用与接收侧相同的 Header::Deserialize 代码):
 * - 数据帧:可选 8B LLR 前导(B0=0xFB/B7=0xD5 判别,eligible+seq)+ AFH;
 * - CtlOS(经 SueCtlHeader):LLR_ACK/NACK(seq)、LLR_INIT/INIT_ECHO
 *   (init_seq+init_data)、CF_UPDATE(两条 VC+CF_count);
 * - 真以太控制帧(整帧即头):CC_UPDATE(逐 VC CC 计数)、PFC(PEV+
 *   逐 VC quanta);LLDP 仅打印类型名。
 * - MTP 多线程下文件写入有全局锁保护,但跨 LP 的写序不确定——做协议
 *   排障建议单线程(ENDPOINT_THREADS=1)运行。
 */
#ifndef SUE_SCRATCH_PCAP_H
#define SUE_SCRATCH_PCAP_H

#include "ns3/sue-channel.h"
#include "ns3/sue-ctl-header.h"
#include "ns3/sue-ctl-tag.h"
#include "ns3/sue-header.h"
#include "ns3/sue-llr-preamble-header.h"
#include "ns3/sue-llr-ctlos-header.h"
#include "ns3/sue-cbfc-header.h"
#include "ns3/sue-cbfc-cc-header.h"
#include "ns3/sue-pfc-header.h"
#include "ns3/sue-config.h"
#include "ns3/pcap-file-wrapper.h"
#include "ns3/trace-helper.h"
#include "ns3/mac48-address.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace ns3 {

/** Master switch: on iff SUE_PCAP ∈ {1, on, true} (case-insensitive). Default off. */
inline bool
SueScratchPcapEnabled ()
{
  const char *e = getenv ("SUE_PCAP");
  if (!e) { return false; }
  std::string s (e);
  for (char &c : s) { c = (char) tolower ((unsigned char) c); }
  return s == "1" || s == "on" || s == "true";
}

/** Per-channel capture sink: one pcap + one txt per attached SueChannel. */
class SueScratchPcapCapture
{
  static uint32_t
  PcapDlt ()
  {
    const char *d = getenv ("SUE_PCAP_DLT");
    std::string s (d && *d ? d : "");
    for (char &c : s) { c = (char) tolower ((unsigned char) c); }
    return s == "user0" ? 147 /* DLT_USER0, SUE 链路线格式, 配 wireshark 插件 */
                        : PcapHelper::DLT_EN10MB;
  }

public:
  SueScratchPcapCapture (Ptr<SueChannel> ch, const std::string &tag,
                         const std::string &dir, const std::string &prefix)
    : m_ch (ch)
  {
    const char *d = getenv ("SUE_PCAP_DIR");
    const char *p = getenv ("SUE_PCAP_PREFIX");
    std::string outDir = (d && *d) ? d : dir;
    std::string pfx = (p && *p) ? p : prefix;
    std::string base = outDir + "/" + pfx + "-" + tag;

    m_pcap = CreateObject<PcapFileWrapper> ();
    m_pcap->Open (base + ".pcap", std::ios::out | std::ios::binary);
    m_pcap->Init (PcapDlt ());
    m_txt.open (base + ".txt", std::ios::out);
    m_txt << "# SUE scratch capture  channel=" << tag
          << "  dlt=" << (PcapDlt () == 147 ? "USER0(147)" : "EN10MB")
          << "  (bytes as put on the wire; ctl frames carry sim-metadata tags, see .cc docs)\n";

    ch->TraceConnectWithoutContext ("TxRxSue",
                                    MakeCallback (&SueScratchPcapCapture::OnFrame, this));
    std::cerr << "[sue-pcap] capturing link '" << tag << "' -> " << base << ".pcap/.txt\n";
  }

private:
  static std::mutex &
  Mutex ()
  {
    static std::mutex m;
    return m;
  }

  static std::string
  DevName (Ptr<NetDevice> d)
  {
    std::ostringstream o;
    if (d && d->GetNode ()) { o << "n" << d->GetNode ()->GetId () << "/d" << d->GetIfIndex (); }
    else { o << "node?"; }
    o << "(" << (d ? d->GetAddress () : Mac48Address ()) << ")";
    return o.str ();
  }

  static const char *
  CtlName (SueCtlHeader::CtlType t)
  {
    switch (t)
      {
      case SueCtlHeader::LLR_ACK: return "LLR_ACK";
      case SueCtlHeader::LLR_NACK: return "LLR_NACK";
      case SueCtlHeader::LLR_INIT: return "LLR_INIT";
      case SueCtlHeader::LLR_INIT_ECHO: return "LLR_INIT_ECHO";
      case SueCtlHeader::CF_UPDATE: return "CF_UPDATE";
      case SueCtlHeader::CC_UPDATE: return "CC_UPDATE";
      case SueCtlHeader::PFC_PAUSE: return "PFC_PAUSE";
      case SueCtlHeader::PFC_RESUME: return "PFC_RESUME";
      case SueCtlHeader::LLDP: return "LLDP";
      default: return "CTL?";
      }
  }

  /** AFH view of a data-frame packet (no preamble): " afh[...] dst_xpu/src_xpu". */
  void
  AppendAfhView (Ptr<const Packet> p)
  {
    SueHeader h;
    if (p->PeekHeader (h)
        && (h.GetEtherType () == SueConfig::EthType0 ()
            || h.GetEtherType () == SueConfig::EthType1 ()))
      {
        m_txt << " afh[";
        h.Print (m_txt);
        m_txt << "] dst_xpu=" << h.GetXpuId ()
              << " src_xpu=" << SueXpuFromMac (h.GetSrc ());
      }
  }

  /** Field-level view of a control frame. Parses with the same Header
   *  Deserialize code the receiver uses (copy + RemoveHeader chain). */
  void
  AppendCtlView (Ptr<const Packet> p, SueCtlHeader::CtlType t)
  {
    switch (t)
      {
      case SueCtlHeader::LLR_ACK:
      case SueCtlHeader::LLR_NACK:
      case SueCtlHeader::LLR_INIT:
      case SueCtlHeader::LLR_INIT_ECHO:
        {
          Ptr<Packet> q = p->Copy ();
          SueCtlHeader ctl; q->RemoveHeader (ctl);
          SueLlrCtlOsHeader h; q->RemoveHeader (h);
          if (t == SueCtlHeader::LLR_INIT || t == SueCtlHeader::LLR_INIT_ECHO)
            {
              m_txt << " init_seq=" << h.GetSeq ()
                    << " init_data=0x" << std::hex << h.GetInitData () << std::dec;
            }
          else
            {
              m_txt << " seq=" << h.GetSeq ();
            }
          break;
        }
      case SueCtlHeader::CF_UPDATE:
        {
          Ptr<Packet> q = p->Copy ();
          SueCtlHeader ctl; q->RemoveHeader (ctl);
          SueCbfcCtlOsHeader h; q->RemoveHeader (h);
          m_txt << " vc" << h.GetVc (0) << "_cf=" << h.GetCount (0);
          if (h.GetVc (1) != 0 || h.GetCount (1) != 0)
            {
              m_txt << " vc" << h.GetVc (1) << "_cf=" << h.GetCount (1);
            }
          break;
        }
      case SueCtlHeader::CC_UPDATE:
        {
          Ptr<Packet> q = p->Copy ();
          SueCbfcCcHeader h; q->RemoveHeader (h);
          m_txt << " cid=0x" << std::hex << h.GetCid () << std::dec << " cc:";
          bool any = false;
          for (uint32_t v = 0; v < SueCbfcCcHeader::NVC; ++v)
            {
              if (h.GetCc (v) != 0) { m_txt << (any ? "," : "") << "vc" << v << "=" << h.GetCc (v); any = true; }
            }
          if (!any) { m_txt << "all-zero"; }
          break;
        }
      case SueCtlHeader::PFC_PAUSE:
      case SueCtlHeader::PFC_RESUME:
        {
          Ptr<Packet> q = p->Copy ();
          SuePfcHeader h; q->RemoveHeader (h);
          m_txt << " pev=0x" << std::hex << h.GetPev () << std::dec << " quanta:";
          bool any = false;
          for (uint32_t v = 0; v < SuePfcHeader::NVC; ++v)
            {
              if (h.GetQuanta (v) != 0) { m_txt << (any ? "," : "") << "vc" << v << "=" << h.GetQuanta (v); any = true; }
            }
          if (!any) { m_txt << "all-zero"; }
          break;
        }
      default:
        break;   // LLDP: type name only
      }
  }

  void
  OnFrame (Ptr<const Packet> p, Ptr<NetDevice> src, Ptr<NetDevice> dst,
           Time txTime, Time rxDelta)
  {
    std::lock_guard<std::mutex> lock (Mutex ());
    std::vector<uint8_t> buf (p->GetSize ());
    p->CopyData (buf.data (), buf.size ());
    m_pcap->Write (Simulator::Now (), buf.data (), (uint32_t) buf.size ());

    m_txt << "#" << m_idx << " t=" << Simulator::Now ().GetNanoSeconds () << "ns"
          << " len=" << buf.size ()
          << " " << DevName (src) << " -> " << DevName (dst)
          << " txTime=" << txTime.GetNanoSeconds () << "ns"
          << " rxDelta=" << rxDelta.GetNanoSeconds () << "ns";

    SueCtlTag ctlTag;
    if (p->PeekPacketTag (ctlTag))
      {
        m_txt << " ctl(" << CtlName (ctlTag.GetType ()) << ")";
        AppendCtlView (p, ctlTag.GetType ());
      }
    else if (buf.size () >= 8 && buf[0] == 0xFB && buf[7] == 0xD5)
      {
        // LLR data frame: strip the 8B preamble, then the AFH view.
        Ptr<Packet> q = p->Copy ();
        SueLlrPreambleHeader pre;
        q->RemoveHeader (pre);
        m_txt << " llr[" << (pre.IsEligible () ? "eligible" : "ineligible")
              << ",seq=" << pre.GetSeq () << "]";
        AppendAfhView (q);
      }
    else if (buf.size () >= 14)
      {
        AppendAfhView (p);
      }

    m_txt << "\n    hex";
    for (size_t i = 0; i < buf.size () && i < 32; ++i)
      {
        m_txt << (i % 16 == 0 ? " " : " ")
              << std::hex << std::setw (2) << std::setfill ('0') << (unsigned) buf[i] << std::dec;
      }
    if (buf.size () > 32) { m_txt << " ..."; }
    m_txt << "\n";
    m_txt.flush ();
    ++m_idx;
  }

  Ptr<SueChannel> m_ch;
  Ptr<PcapFileWrapper> m_pcap;
  std::ofstream m_txt;
  uint32_t m_idx{0};
};

/** Attach a capture to `ch`. No-op unless SUE_PCAP is on (default off). */
inline void
SueScratchPcapAttach (Ptr<SueChannel> ch, const std::string &tag)
{
  if (!SueScratchPcapEnabled ()) { return; }
  static std::vector<std::unique_ptr<SueScratchPcapCapture>> registry;
  registry.push_back (std::make_unique<SueScratchPcapCapture> (ch, tag, ".", "sue"));
}

} // namespace ns3

#endif /* SUE_SCRATCH_PCAP_H */

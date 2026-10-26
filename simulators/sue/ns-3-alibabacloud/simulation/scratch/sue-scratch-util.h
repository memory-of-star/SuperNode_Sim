/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Shared scratch-test utilities (review-fix E2 dedup): the aggregate-hash
 * core used by the MTP equivalence checks. Both sue-mtp-equivalence and
 * sue-endpoint-ring previously carried byte-identical copies of this loop;
 * it lives here so the equivalence criterion stays a single source of truth.
 */
#ifndef SUE_SCRATCH_UTIL_H
#define SUE_SCRATCH_UTIL_H

#include "ns3/sue-entry.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-llr-counters.h"
#include "ns3/sue-cbfc-counters.h"
#include "ns3/sue-cbfc-engine.h"

#include <sstream>

namespace ns3 {

/** Append every node's device LLR+CBFC counter dump ("n{d}l{...}c{...};" per
 *  device) to `o` — the shared aggregate-hash core of the MTP equivalence
 *  checks. `maskAckNack` replaces the order-sensitive ACK/NACK tallies with
 *  "x" (the behavioral-hash masking, §12.28: those counts may legitimately
 *  shift by one across Default vs MTP under the same-timestamp tie-break). */
inline void
SueScratchAppendDeviceCounters (std::ostringstream &o, bool maskAckNack,
                                bool maskCfUpdate = false)
{
  // maskCfUpdate: CF_Update 是 CtlOS 节奏类消息(与 ACK/NACK 同类)——其发送
  // 计数对 CF_min 边界处的 drain 事件交错敏感(100G 下 CF_min=10us,线程数
  // 不同时 1ns 级边界翻转会移相 4~5 次),而接收侧处理幂等、数据路径不受
  // 影响。端点环的 MTP 等价判据聚焦数据路径/状态机(l{} 全同 + delivered
  // 全同),cf_update 计数按同设计原则掩码(§10.5)。
  for (uint32_t n = 0; n < SueGetNNodes (); ++n)
    {
      Ptr<Node> node = SueGetDevice (n, 0)->GetNode ();
      for (uint32_t d = 0; d < node->GetNDevices (); ++d)
        {
          Ptr<SueNetDevice> dev = DynamicCast<SueNetDevice> (node->GetDevice (d));
          if (!dev) { continue; }
          o << "n" << n << "d" << d;
          if (dev->GetLlrEngine ())
            {
              const SueLlrCounters &c = dev->GetLlrEngine ()->GetCounters ();
              auto M = [maskAckNack](uint64_t v) -> std::string
                { return maskAckNack ? std::string ("x") : std::to_string (v); };
              o << "l{"
                << c.tx_init_ctl_os << "," << c.tx_init_echo_ctl_os << ","
                << M (c.tx_ack_ctl_os) << "," << M (c.tx_nack_ctl_os) << "," << c.tx_discard << ","
                << c.tx_ok << "," << c.tx_poisoned << "," << c.tx_replay << ","
                << c.rx_init_ctl_os << "," << c.rx_init_echo_ctl_os << ","
                << M (c.rx_ack_ctl_os) << "," << M (c.rx_nack_ctl_os) << ","
                << M (c.rx_ack_nack_seq_error) << ","
                << c.rx_ok << "," << c.rx_poisoned << "," << c.rx_bad << ","
                << c.rx_expected_seq_good << "," << c.rx_expected_seq_poisoned << ","
                << c.rx_expected_seq_bad << "," << c.rx_missing_seq << ","
                << c.rx_duplicate_seq << "," << c.rx_replay << "}";
            }
          if (dev->GetCbfcEngine ())
            {
              const SueCbfcCounters &c = dev->GetCbfcEngine ()->GetCounters ();
              auto MC = [maskCfUpdate](uint64_t v) -> std::string
                { return maskCfUpdate ? std::string ("x") : std::to_string (v); };
              o << "c{" << MC (c.cf_update_tx) << "," << MC (c.cf_update_rx) << ","
                << c.cc_update_tx << "," << c.cc_update_rx;
              for (int v = 0; v < SueCbfcEngine::NVC; ++v) { o << "," << c.credit_exhausted[v]; }
              for (int v = 0; v < SueCbfcEngine::NVC; ++v) { o << "," << c.cc_lost_credits_recovered[v]; }
              o << "}";
            }
          o << ";";
        }
    }
}

} // namespace ns3

#endif /* SUE_SCRATCH_UTIL_H */

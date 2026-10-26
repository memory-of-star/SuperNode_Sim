/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE configuration (DESIGN.md §9). GlobalValue registration + SimAI.conf-
 * style parser + per-device applier.
 *
 * The values are module-wide (one config file per run), matching how the
 * scale-out baseline's ReadConf/SetConfig consume a single SimAI.conf. All
 * defaults mirror the DESIGN §9 table; they take effect only when
 * SueReadConf/ApplySueConfig are invoked (device/engine code-level defaults
 * are untouched otherwise -> zero regression for existing tests/scratches).
 */
#include "sue-config.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/object.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/integer.h"
#include "ns3/string.h"
#include "ns3/double.h"
#include "ns3/config.h"
#include "ns3/nstime.h"
#include "ns3/log.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <array>

NS_LOG_COMPONENT_DEFINE ("SueConfig");

namespace ns3 {

// ---- GlobalValue registration (DESIGN §9). Defaults = the §9 table. ----
static GlobalValue g_enableScaleup ("ENABLE_SCALEUP", "Enable the SUE scale-up topology at the entry dispatch.",
                                    UintegerValue (0), MakeUintegerChecker<uint32_t> ());
// External scale-up workload gate (DESIGN §6.12): 0 (default) = SueLiteEndpoint
// runs its built-in self-simulated traffic generator; 1 = future astra-sim
// workload driving via the reserved sim_send/sim_recv/sim_schedule API (not yet
// wired — the endpoint logs a warning and falls back to the built-in generator).
static GlobalValue g_enableScaleupWorkload ("ENABLE_SCALEUP_WORKLOAD",
                                            "Enable external scale-up workload driving (future astra-sim integration, §6.12). 0 = SueLiteEndpoint's built-in generator.",
                                            UintegerValue (0), MakeUintegerChecker<uint32_t> ());
// Two SYMMETRIC SUE protocol EtherTypes. Neither value implies shim presence:
// per-etype *HAS_SHIM decides how frames of that EtherType are parsed (the
// chip model: an EtherType "configured together with a shim header" parses as
// shim-bearing, otherwise as shim-less; DESIGN §5.2/§9). Defaults below keep
// the historical behavior (0xFCF1 no-shim, 0xFCF2 shim) as the default config.
static GlobalValue g_eth0 ("SUE_ETH_TYPE_0", "SUE protocol EtherType #0 (symmetric identifier).",
                           UintegerValue (0xFCF1), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_eth0HasShim ("SUE_ETH_TYPE_0_HAS_SHIM", "Parse frames of SUE_ETH_TYPE_0 as carrying a shim header.",
                                  UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_eth1 ("SUE_ETH_TYPE_1", "SUE protocol EtherType #1 (symmetric identifier).",
                           UintegerValue (0xFCF2), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_eth1HasShim ("SUE_ETH_TYPE_1_HAS_SHIM", "Parse frames of SUE_ETH_TYPE_1 as carrying a shim header.",
                                  UintegerValue (1), MakeUintegerChecker<uint32_t> ());
// UEC §5.1.4 two-phase LLR enablement (replaces the single LLR_ENABLE):
// llr_mode_local gates the RX path, llr_mode_remote gates the TX path.
static GlobalValue g_llrModeLocal ("LLR_MODE_LOCAL", "llr_mode_local: ON = RX LLR active (may receive LLR_INIT, seq-filters eligible frames). 0=OFF 1=ON.",
                                   UintegerValue (1), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_llrModeRemote ("LLR_MODE_REMOTE", "llr_mode_remote: ON = TX LLR active (may send LLR_INIT, assigns seq + replay). 0=OFF 1=ON.",
                                    UintegerValue (1), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_llrInitBehavior ("LLR_INIT_BEHAVIOR", "TX handling of new LLR-desired frames in the INIT state: DISCARD|BLOCK|BEST_EFFORT.",
                                      StringValue ("BLOCK"), MakeStringChecker ());
static GlobalValue g_llrFlushBehavior ("LLR_FLUSH_BEHAVIOR", "TX handling of new LLR-desired frames in the FLUSH state: DISCARD|BLOCK|BEST_EFFORT.",
                                       StringValue ("BLOCK"), MakeStringChecker ());
static GlobalValue g_llrReInitOnDiscard ("LLR_RE_INIT_ON_DISCARD", "re_init_on_discard: TRUE = FLUSH auto-enters INIT; FALSE = stay in FLUSH for management.",
                                         UintegerValue (1), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_replayTimerMax ("LLR_REPLAY_TIMER_MAX_NS", "LLR replay timer period (ns); 0 = auto-derive 10x LLR RTT (DESIGN §12.27).",
                                     UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_replayCtMax ("LLR_REPLAY_CT_MAX", "LLR replay count before FLUSH.",
                                  UintegerValue (5), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_dataAgeTimerMax ("LLR_DATA_AGE_TIMER_MAX_NS", "LLR data-age timer max (ns).",
                                      UintegerValue (50000), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_initTimerMax ("LLR_INIT_TIMER_MAX_NS", "LLR INIT handshake retry timeout (ns); 0 = auto-derive (DESIGN §12.27).",
                                   UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_initRetryMax ("LLR_INIT_RETRY_MAX", "LLR INIT handshake retry count.",
                                   UintegerValue (3), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_outstandingSeqMax ("LLR_OUTSTANDING_SEQ_MAX", "LLR outstanding-seq limit.",
                                        UintegerValue (65536), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_outstandingDataMax ("LLR_OUTSTANDING_DATA_MAX", "LLR outstanding-data limit (B).",
                                         UintegerValue (1048576), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_replayBufMax ("LLR_REPLAY_BUF_MAX", "LLR replay-buffer capacity (frames): max LLR-eligible frames held for retransmission (UEC L303: buffer is implementation-dependent, sized by sublayer delays + intended link length; must cover in-flight data AND data sent during a PCS glitch).",
                                   UintegerValue (1u << 20), MakeUintegerChecker<uint32_t> ());
// UEC §5.2.11.2: CtlOS must be limited to < 2% of link bandwidth, enforced by
// a hard minimum distance of 400 B between ANY two CtlOS (any type). This is
// the device pacing gate, and it is INDEPENDENT of the LLR ACK period
// (LLR_CTLOS_TARGET_SPACING, Table 5-9): ACKs are generated at that cadence
// and ride ON TOP of this wire gate. Conflating the two (gate == ackPeriod)
// lets a continuous idle-repeat ACK stream occupy every CtlOS slot on a
// control-only link and starve the lowest-priority CF_UPDATE (DESIGN §12.26).
static const uint32_t CTLOS_MIN_SPACING_BYTES = 400;
// §12.25/§12.27 auto-timer derivation: canonical data frame (14 hdr + 86 payload
// + 8 trailer, matching the default payload) + IFG, to bound the LLR RTT
// (2*wire + 2*prop + ackPeriod) when the replay/init timer keys are 0 (= auto).
static const uint32_t LLR_AUTO_TIMER_FRAME_BYTES = 108;
static const uint32_t LLR_AUTO_TIMER_IFG_BYTES = 12;
static GlobalValue g_ctlosTargetSpacing ("LLR_CTLOS_TARGET_SPACING", "LLR ACK/NACK target spacing (wire bytes, UEC Table 5-9, default 2048). Drives only the engine's periodic cumulative-ACK period; the device CtlOS wire gate is the fixed 400B floor (CTLOS_MIN_SPACING_BYTES, §5.2.11.2).",
                                         UintegerValue (2048), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pcsLostTimerMax ("LLR_PCS_LOST_STATUS_TIMER_MAX_NS", "pcs_lost_status_timer_max (ns): how long PCS_status may be FALSE in ADVANCE/REPLAY before the timer expires and forces FLUSH (UEC Table 5-10). 0 = immediate expiry (UEC '0 = immediate expiration').",
                                      UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_cfThresholdBytes ("CBFC_CF_THRESHOLD_BYTES",
                                           "Data-volume CF_Update trigger (SDK credit_threshold 对等物): 累计释放信用字节 ≥ 该值即上报一次。0=关闭(逐帧/定时器路径)。字节键、与端口速率无关:CF 计数 ≈ 流量字节/阈值。",
                                           UintegerValue (0), MakeUintegerChecker<uint64_t> ());
static GlobalValue g_ccMacDa ("CBFC_CC_MACDA", "CC_Update MACDA (UEC §5.2.6.2 建议可配; default = slow-protocol multicast).",
                                 StringValue ("01:80:c2:00:00:01"), MakeStringChecker ());
static GlobalValue g_ccCid ("CBFC_CC_CID", "CC_Update CID, 24-bit (UEC §5.2.6.2 建议可配; default = FA-7A-CB).",
                            UintegerValue (0xFA7ACB), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_creditSize ("CBFC_CREDIT_SIZE", "CBFC credit size (B).",
                                 UintegerValue (256), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_txCellChoices ("CBFC_TX_CELL_CHOICES",
                                    "Supported TX cell sizes as CSV (UEC §5.2.10.1 minimal set = powers of "
                                    "two). Empty = any. LLDP adoption snaps the peer's advertised native RX "
                                    "cell down to the largest supported value <= it (§5.2.8.2), e.g. a "
                                    "pow2-only sender facing a 56B-cell receiver transmits at 32B; the "
                                    "granularity leak is recovered by CC_Update (§5.2.5.8).",
                                    StringValue (""), MakeStringChecker ());
static GlobalValue g_vcCreditLimit ("CBFC_VC_CREDIT_LIMIT", "Per-VC credit limit as 'vc:limit,...'.",
                                    StringValue ("0:512,1:512,2:512,3:512,4:512,5:512,6:512,7:512"),
                                    MakeStringChecker ());
static GlobalValue g_cbfcEgressBufBytes ("CBFC_EGRESS_BUF_BYTES",
                                         "CBFC per-VC switch egress buffer depth limit (B). "
                                         "0 = auto = creditLimit x creditSize per VC (physical default, §12.30). "
                                         ">0 = explicit per-VC limit on every CBFC switch port. The egress gate makes "
                                         "the switch hold frames (and withhold the ingress credit refund) when the "
                                         "egress VC queue is full, so a 2:1 incast genuinely throttles the senders.",
                                         UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_dscpToVcMap ("DSCP_TO_VC_MAP",
                                  "Shim DSCP -> VC mapping table as 'dscp:vc,...' (DESIGN §7; default = direct map "
                                  "dscp&0x07 for all 64 values, i.e. 0:0,...,63:7). Partial overrides are applied on "
                                  "top of the direct default: a dscp not listed keeps dscp&0x07. End-to-end: host "
                                  "send/receive and the switch decode the same table, so a packet's VC is identical "
                                  "at every hop.",
                                  StringValue ("0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7,"
                                               "8:0,9:1,10:2,11:3,12:4,13:5,14:6,15:7,"
                                               "16:0,17:1,18:2,19:3,20:4,21:5,22:6,23:7,"
                                               "24:0,25:1,26:2,27:3,28:4,29:5,30:6,31:7,"
                                               "32:0,33:1,34:2,35:3,36:4,37:5,38:6,39:7,"
                                               "40:0,41:1,42:2,43:3,44:4,45:5,46:6,47:7,"
                                               "48:0,49:1,50:2,51:3,52:4,53:5,54:6,55:7,"
                                               "56:0,57:1,58:2,59:3,60:4,61:5,62:6,63:7"),
                                  MakeStringChecker ());
static GlobalValue g_pcpToVcMap ("PCP_TO_VC_MAP", "VLAN PCP -> VC mapping table as 'pcp:vc,...' (DESIGN §7; default = pcp "
                                    "identity). Keys 8..15 address (dei<<3|pcp) for per-DEI mapping; "
                                    "with an 8-entry map both DEI rows are identical. "
                                 "identity 0:0,...,7:7). Partial overrides on top of the identity default.",
                                 StringValue ("0:0,1:1,2:2,3:3,4:4,5:5,6:6,7:7"), MakeStringChecker ());
static GlobalValue g_defaultVc ("DEFAULT_VC", "VC for frames with neither shim nor VLAN (DESIGN §7; default 0).",
                                UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_dscpToVrfMap ("DSCP_TO_VRF_MAP",
                                   "Shim DSCP -> VRF mapping table as 'dscp:vrf,...' (DESIGN §12.40; default empty = "
                                   "all VRF0). Precedence DSCP > VLAN > port; a dscp not listed means the next "
                                   "source is tried.",
                                   StringValue (""), MakeStringChecker ());
static GlobalValue g_vlanToVrfMap ("VLAN_TO_VRF_MAP",
                                   "VLAN vid -> VRF mapping table as 'vid:vrf,...' (DESIGN §12.40; default empty = "
                                   "all VRF0). Precedence DSCP > VLAN > port; a vid not listed means the next "
                                   "source is tried.",
                                   StringValue (""), MakeStringChecker ());
static GlobalValue g_portToVrfMap ("PORT_TO_VRF_MAP",
                                   "Ingress port -> VRF mapping table as 'portIdx:vrf,...' (DESIGN §12.40; default "
                                   "empty = all VRF0). portIdx is the switch AddPort order (0..N-1), per switch. "
                                   "Lowest precedence source: only used when neither the shim DSCP nor the VLAN "
                                   "vid is mapped.",
                                   StringValue (""), MakeStringChecker ());
static GlobalValue g_pktOvhd ("CBFC_PKT_OVHD", "CBFC packet overhead (B, may be negative).",
                              IntegerValue (0), MakeIntegerChecker<int32_t> ());
static GlobalValue g_ccMsgTimer ("CBFC_CC_MSG_TIMER_US", "CC_Update generation period (us; 0 = off).",
                                 UintegerValue (100), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_cfMinTimer ("CBFC_CF_MIN_TIMER_BYTES", "CF_Update min-timer threshold (wire B, -> time at the link rate; 0 = legacy per-frame emission).",
                                 UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_cfMaxTimer ("CBFC_CF_MAX_TIMER_BYTES", "CF_Update max-timer period (wire B, -> time at the link rate; periodic full update; 0 = off).",
                                 UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_ifgBytes ("SUE_IFG_BYTES", "Inter-frame gap in wire bytes (12 = 802.3 standard).",
                               UintegerValue (12), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_linkPropNs ("SUE_LINK_PROPAGATION_NS",
                                 "One-way propagation delay on every SUE link (ns; copper/optical transit, "
                                 "control frames share it). 0 = zero-delay links (baseline). Nonzero also gives "
                                 "MTP a per-LP lookahead equal to this transit (§10.5).",
                                 UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_swFwdBudgetNs ("SUE_SWITCH_FWD_BUDGET_NS",
                                    "Per-hop switch forwarding budget (ns): forwarding latency + egress wire "
                                    "time <= budget. 0 = forward immediately (no switch processing latency). "
                                    "Egress-queue congestion waiting is on top of this budget.",
                                    UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcPauseQuanta ("PFC_PAUSE_QUANTA", "PFC pause quanta (802.1Qbb).",
                                     UintegerValue (0xFFFF), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcDrainDelayNs ("PFC_DRAIN_DELAY_NS",
                                      "PFC receiver drain delay (ns): without it (0, default) OnRx is followed "
                                      "by an immediate OnDrain, the buffer never accumulates and pause never "
                                      "triggers on the config path (DESIGN §6.4 modeling constraint).",
                                      UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcDynamicThreshold ("PFC_DYNAMIC_THRESHOLD", "Enable shared-pool dynamic thresholds.",
                                          UintegerValue (1), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcBufferSize ("PFC_BUFFER_SIZE", "PFC receiver buffer size (B).",
                                    UintegerValue (65536), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcSharedPool ("PFC_SHARED_POOL_SIZE", "PFC shared-pool size (B).",
                                    UintegerValue (32768), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcHeadroom ("PFC_HEADROOM_SIZE", "PFC shared-pool headroom (B).",
                                  UintegerValue (8192), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcResumeOffset ("PFC_RESUME_OFFSET", "PFC shared-pool resume offset (B).",
                                      UintegerValue (4096), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcAlphaShift ("PFC_ALPHA_SHIFT", "PFC shared-pool alpha shift.",
                                    UintegerValue (0), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcPauseThresh ("PFC_PAUSE_THRESHOLD", "PFC per-VC pause threshold (simple mode, B).",
                                     UintegerValue (32768), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_pfcResumeThresh ("PFC_RESUME_THRESHOLD", "PFC per-VC resume threshold (simple mode, B).",
                                      UintegerValue (16384), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_fcMode ("FC_MODE", "Flow-control mode: OFF | CBFC | PFC.",
                             StringValue ("CBFC"), MakeStringChecker ());
static GlobalValue g_linkErrorRate ("LINK_ERROR_RATE_PER_LINK",
                                    "Per-link error rate. Written from the topo link's err_rate by SetupSueNetwork "
                                    "(recorded value; the module itself uses deterministic corruptor sets, §10.1).",
                                    DoubleValue (1e-9), MakeDoubleChecker<double> ());
static GlobalValue g_mtpThreads ("SUE_MTP_THREADS",
                                 "Multithreaded simulator thread count. 1 (default) = single-threaded, no NS3_MTP. "
                                 ">1 = the front-end (e.g. scratch/sue-s8-mtp.cc) enables the single-process "
                                 "multithreaded simulator with auto-partition (§10.5); the sue module itself stays "
                                 "mtp-free and only context-routes its node timers.",
                                 UintegerValue (1), MakeUintegerChecker<uint32_t> ());
static GlobalValue g_lldpNegotiate ("LLDP_NEGOTIATE",
                                    "UE link negotiation (UEC §5.3, NEG-SP-01/02 + CBFC-SP-13): 1 = exchange "
                                    "capability TLVs at link start and start the LLR engines only after the "
                                    "peer TLV is applied (LLR iff both ends support it; CBFC sender adopts "
                                    "the receiver's advertised credit size / per-VC limits once). Also runs "
                                    "the §5.2.8.4 RTS/RTR lossless-VC initialization handshake (multi-round "
                                    "minimal model): a VC may not use credits until local S_VC_RTS and the "
                                    "peer's R_VC_RTR agree. 0 (default) = direct config, no exchange.",
                                    UintegerValue (0), MakeUintegerChecker<uint32_t> ());

namespace {

enum GKind { G_UINT, G_INT, G_STRING, G_DOUBLE };
struct GKey { const char *name; GKind kind; };

// Parser dispatch table: must stay in sync with the GlobalValues above.
const GKey g_keys[] = {
  {"ENABLE_SCALEUP", G_UINT}, {"ENABLE_SCALEUP_WORKLOAD", G_UINT},
  {"SUE_ETH_TYPE_0", G_UINT}, {"SUE_ETH_TYPE_0_HAS_SHIM", G_UINT},
  {"SUE_ETH_TYPE_1", G_UINT}, {"SUE_ETH_TYPE_1_HAS_SHIM", G_UINT},
  {"LLR_MODE_LOCAL", G_UINT}, {"LLR_MODE_REMOTE", G_UINT}, {"LLR_INIT_BEHAVIOR", G_STRING},
  {"LLR_FLUSH_BEHAVIOR", G_STRING}, {"LLR_RE_INIT_ON_DISCARD", G_UINT},
  {"LLR_REPLAY_TIMER_MAX_NS", G_UINT}, {"LLR_REPLAY_CT_MAX", G_UINT},
  {"LLR_DATA_AGE_TIMER_MAX_NS", G_UINT}, {"LLR_INIT_TIMER_MAX_NS", G_UINT}, {"LLR_INIT_RETRY_MAX", G_UINT},
  {"LLR_OUTSTANDING_SEQ_MAX", G_UINT}, {"LLR_OUTSTANDING_DATA_MAX", G_UINT},
  {"LLR_REPLAY_BUF_MAX", G_UINT},
  {"LLR_CTLOS_TARGET_SPACING", G_UINT}, {"LLR_PCS_LOST_STATUS_TIMER_MAX_NS", G_UINT},
  {"CBFC_CREDIT_SIZE", G_UINT}, {"CBFC_CC_MACDA", G_STRING}, {"CBFC_CC_CID", G_UINT},
  {"CBFC_VC_CREDIT_LIMIT", G_STRING}, {"CBFC_PKT_OVHD", G_INT},
  {"CBFC_TX_CELL_CHOICES", G_STRING},
  {"CBFC_EGRESS_BUF_BYTES", G_UINT},
  {"DSCP_TO_VC_MAP", G_STRING}, {"PCP_TO_VC_MAP", G_STRING}, {"DEFAULT_VC", G_UINT},
  {"DSCP_TO_VRF_MAP", G_STRING}, {"VLAN_TO_VRF_MAP", G_STRING}, {"PORT_TO_VRF_MAP", G_STRING},
  {"CBFC_CC_MSG_TIMER_US", G_UINT}, {"CBFC_CF_MIN_TIMER_BYTES", G_UINT}, {"CBFC_CF_MAX_TIMER_BYTES", G_UINT},
  {"CBFC_CF_THRESHOLD_BYTES", G_UINT},
  {"SUE_IFG_BYTES", G_UINT},
  {"SUE_LINK_PROPAGATION_NS", G_UINT}, {"SUE_SWITCH_FWD_BUDGET_NS", G_UINT},
  {"PFC_PAUSE_QUANTA", G_UINT}, {"PFC_DRAIN_DELAY_NS", G_UINT}, {"PFC_DYNAMIC_THRESHOLD", G_UINT},
  {"PFC_BUFFER_SIZE", G_UINT}, {"PFC_SHARED_POOL_SIZE", G_UINT}, {"PFC_HEADROOM_SIZE", G_UINT},
  {"PFC_RESUME_OFFSET", G_UINT}, {"PFC_ALPHA_SHIFT", G_UINT},
  {"PFC_PAUSE_THRESHOLD", G_UINT}, {"PFC_RESUME_THRESHOLD", G_UINT},
  {"FC_MODE", G_STRING}, {"LINK_ERROR_RATE_PER_LINK", G_DOUBLE}, {"SUE_MTP_THREADS", G_UINT},
  {"LLDP_NEGOTIATE", G_UINT},
};

/** Set one GlobalValue from a string token. Returns false for unknown keys
 *  (skipped silently, mirroring the baseline ReadConf fallthrough). */
bool
SueSetGlobal (const std::string &key, const std::string &val)
{
  for (const GKey &k : g_keys)
    {
      if (key == k.name)
        {
          // SetGlobalFailSafe returns false when no GlobalValue with that name
          // is registered (e.g. a key listed in g_keys[] whose registration
          // was removed/renamed): fail loudly instead of silently no-op'ing.
          bool ok = false;
          switch (k.kind)
            {
            case G_UINT:
              ok = Config::SetGlobalFailSafe (key, UintegerValue ((uint32_t) std::strtoul (val.c_str (), nullptr, 0)));
              break;
            case G_INT:
              ok = Config::SetGlobalFailSafe (key, IntegerValue ((int32_t) std::strtol (val.c_str (), nullptr, 0)));
              break;
            case G_STRING:
              ok = Config::SetGlobalFailSafe (key, StringValue (val));
              break;
            case G_DOUBLE:
              ok = Config::SetGlobalFailSafe (key, DoubleValue (std::strtod (val.c_str (), nullptr)));
              break;
            }
          if (!ok)
            {
              NS_LOG_WARN ("SueSetGlobal: Config::SetGlobal failed for key '" << key
                           << "' (listed in g_keys but no GlobalValue registered)");
            }
          return true;
        }
    }
  return false;
}

// EtherType / *HAS_SHIM first-use cache (DESIGN §9): EthType0/1/HasShim() read
// the GlobalValues once and memoize, instead of a per-frame GetValue lookup on
// the 512-card hot path. SueReadConf invalidates so a re-read takes effect
// before traffic starts.
struct EtherTypeCache
{
  std::optional<uint16_t> eth0, eth1;
  std::optional<bool> eth0Shim, eth1Shim;
};
EtherTypeCache g_etypeCache;

// VC-map first-use cache (DESIGN §7). The arrays are PRE-SEEDED with the
// direct default (dscp&0x07 / pcp identity) and parsed entries override them,
// so a PARTIAL map is an incremental tweak on the direct baseline (a dscp not
// listed keeps dscp&0x07) and DEFAULT_VC stays strictly the no-shim/no-vlan
// fallback. Mirrors the EtherTypeCache pattern (same MTP safety contract:
// invalidate only from main()/config load, never mid-traffic from a node LP).
struct VcMapCache
{
  std::optional<std::array<uint8_t, 64>> dscp;
  std::optional<std::array<uint8_t, 16>> pcp;   // [0..7]=DEI0 row, [8..15]=DEI1 row
  std::optional<uint8_t> defaultVc;
};
VcMapCache g_vcMapCache;

/** VRF map first-use caches (§12.40): per-source "key -> vrf" tables, default
 *  all VRF0. A mapped value of 0 is indistinguishable from "unmapped" (both
 *  mean VRF0), so the tables only need the default seed of zeros. */
// v3: cover 576-port switches as well as the original 256-port fabric.
// This is a configuration-map bound, not a device or routing-key limit.
constexpr uint32_t kPortVrfEntries = 1024;
struct VrfMapCache
{
  std::optional<std::array<uint32_t, 64>> dscp;    // shim DSCP 0..63 -> vrf
  std::optional<std::array<uint32_t, 4096>> vlan;  // vid 0..4095 -> vrf
  std::optional<std::array<uint32_t, kPortVrfEntries>> port;
};
VrfMapCache g_vrfMapCache;

} // anonymous namespace

void
SueConfig::InvalidateCache ()
{
  g_etypeCache = EtherTypeCache {};
  g_vcMapCache = VcMapCache {};
  g_vrfMapCache = VrfMapCache {};
}

uint16_t
SueConfig::EthType0 ()
{
  if (!g_etypeCache.eth0)
    {
      UintegerValue v;
      g_eth0.GetValue (v);
      g_etypeCache.eth0 = (uint16_t) (v.Get () & 0xFFFF);
    }
  return *g_etypeCache.eth0;
}

uint16_t
SueConfig::EthType1 ()
{
  if (!g_etypeCache.eth1)
    {
      UintegerValue v;
      g_eth1.GetValue (v);
      g_etypeCache.eth1 = (uint16_t) (v.Get () & 0xFFFF);
    }
  return *g_etypeCache.eth1;
}

/** Whether frames of `etherType` are parsed as shim-bearing, per the per-etype
 *  *HAS_SHIM config (the chip model, DESIGN §5.2/§9). False for any value that
 *  is not one of the configured SUE EtherTypes. */
bool
SueConfig::HasShim (uint16_t etherType)
{
  // Compare against the (cached) configured EtherTypes first; only then read
  // the per-etype HAS_SHIM flag (also cached).
  if (etherType == EthType0 ())
    {
      if (!g_etypeCache.eth0Shim)
        {
          UintegerValue v;
          g_eth0HasShim.GetValue (v);
          g_etypeCache.eth0Shim = v.Get () != 0;
        }
      return *g_etypeCache.eth0Shim;
    }
  if (etherType == EthType1 ())
    {
      if (!g_etypeCache.eth1Shim)
        {
          UintegerValue v;
          g_eth1HasShim.GetValue (v);
          g_etypeCache.eth1Shim = v.Get () != 0;
        }
      return *g_etypeCache.eth1Shim;
    }
  return false;
}

/** One-way link propagation delay (ns) set on every SUE channel at build time
 *  (SetupSueNetwork). Not cached: read once per link at setup, not hot-path. */
Time
SueConfig::LinkPropagation ()
{
  UintegerValue v;
  g_linkPropNs.GetValue (v);
  return NanoSeconds (v.Get ());
}

/** Switch per-hop forwarding budget (ns): forwarding latency + egress wire
 *  time <= budget; congestion queueing on top. 0 = forward immediately. */
Time
SueConfig::SwitchForwardingBudget ()
{
  UintegerValue v;
  g_swFwdBudgetNs.GetValue (v);
  return NanoSeconds (v.Get ());
}

/** CBFC per-VC switch egress buffer depth limit (B); 0 = auto
 *  (creditLimit x creditSize per VC). §12.30. */
uint64_t
SueConfig::CbfcEgressBufBytes ()
{
  UintegerValue v;
  g_cbfcEgressBufBytes.GetValue (v);
  return v.Get ();
}

// "k:v,..." -> apply entries to out[] (maxKey = largest valid key). Bad key
// (out of range) / malformed item: warn+skip. Value > 7: warn+clamp to 7
// (8-VC invariant, DESIGN §7). Keys not listed leave out[] as the caller
// seeded it (direct default) -> partial-override semantics (§7).
static void
ParseVcMap (const std::string &s, uint32_t maxKey, uint8_t *out)
{
  std::istringstream iss (s);
  std::string item;
  while (std::getline (iss, item, ','))
    {
      const std::string::size_type c = item.find (':');
      if (c == std::string::npos)
        {
          NS_LOG_WARN ("ParseVcMap: malformed item '" << item << "' skipped");
          continue;
        }
      const uint32_t k = (uint32_t) std::strtoul (item.substr (0, c).c_str (), nullptr, 0);
      const uint32_t v = (uint32_t) std::strtoul (item.substr (c + 1).c_str (), nullptr, 0);
      if (k > maxKey)
        {
          NS_LOG_WARN ("ParseVcMap: key " << k << " out of range (0.." << maxKey << ") skipped");
          continue;
        }
      if (v > 7)
        {
          NS_LOG_WARN ("ParseVcMap: vc " << v << " > 7 clamped to 7 (8-VC invariant)");
        }
      out[k] = (v > 7) ? (uint8_t) 7 : (uint8_t) v;
    }
}

uint8_t
SueConfig::DscpToVc (uint8_t dscp)
{
  if (!g_vcMapCache.dscp)
    {
      std::array<uint8_t, 64> m;
      for (uint32_t i = 0; i < m.size (); ++i) { m[i] = (uint8_t) (i & 0x07); } // direct default
      StringValue sv;
      g_dscpToVcMap.GetValue (sv);
      ParseVcMap (sv.Get (), 63, m.data ());
      g_vcMapCache.dscp = m;
    }
  return (*g_vcMapCache.dscp)[dscp & 0x3F];
}

uint8_t
SueConfig::PcpToVc (uint8_t pcp)
{
  return PcpToVc (pcp, false);
}

uint8_t
SueConfig::PcpToVc (uint8_t pcp, bool dei)
{
  if (!g_vcMapCache.pcp)
    {
      std::array<uint8_t, 16> m;
      for (uint32_t i = 0; i < m.size (); ++i) { m[i] = (uint8_t) (i & 0x07); } // identity default
      StringValue sv;
      g_pcpToVcMap.GetValue (sv);
      ParseVcMap (sv.Get (), 15, m.data ());
      g_vcMapCache.pcp = m;
    }
  return (*g_vcMapCache.pcp)[((dei ? 1u : 0u) << 3) | (pcp & 0x07)];
}

uint8_t
SueConfig::DefaultVc ()
{
  if (!g_vcMapCache.defaultVc)
    {
      UintegerValue v;
      g_defaultVc.GetValue (v);
      g_vcMapCache.defaultVc = (uint8_t) (v.Get () & 0x07);
    }
  return *g_vcMapCache.defaultVc;
}

// "k:vrf,..." -> apply entries to out[] (maxKey = largest valid key). Bad key
// (out of range) / malformed item: warn+skip. VRF values are not clamped
// (§12.40 — arbitrary 32-bit VRF ids). Keys not listed leave out[] zeroed
// (VRF0) — a mapped 0 and an unmapped key are both VRF0, as intended.
static void
ParseVrfMap (const std::string &s, uint32_t maxKey, uint32_t *out)
{
  std::istringstream iss (s);
  std::string item;
  while (std::getline (iss, item, ','))
    {
      const std::string::size_type c = item.find (':');
      if (c == std::string::npos)
        {
          NS_LOG_WARN ("ParseVrfMap: malformed item '" << item << "' skipped");
          continue;
        }
      const uint32_t k = (uint32_t) std::strtoul (item.substr (0, c).c_str (), nullptr, 0);
      const uint32_t v = (uint32_t) std::strtoul (item.substr (c + 1).c_str (), nullptr, 0);
      if (k > maxKey)
        {
          NS_LOG_WARN ("ParseVrfMap: key " << k << " out of range (0.." << maxKey << ") skipped");
          continue;
        }
      out[k] = v;
    }
}

uint32_t
SueConfig::DscpToVrf (uint8_t dscp)
{
  if (!g_vrfMapCache.dscp)
    {
      std::array<uint32_t, 64> m{};
      StringValue sv;
      g_dscpToVrfMap.GetValue (sv);
      ParseVrfMap (sv.Get (), 63, m.data ());
      g_vrfMapCache.dscp = m;
    }
  return (*g_vrfMapCache.dscp)[dscp & 0x3F];
}

uint32_t
SueConfig::VlanToVrf (uint16_t vid)
{
  if (!g_vrfMapCache.vlan)
    {
      std::array<uint32_t, 4096> m{};
      StringValue sv;
      g_vlanToVrfMap.GetValue (sv);
      ParseVrfMap (sv.Get (), 4095, m.data ());
      g_vrfMapCache.vlan = m;
    }
  return (*g_vrfMapCache.vlan)[vid & 0xFFF];
}

uint32_t
SueConfig::PortToVrf (uint32_t portIdx)
{
  if (!g_vrfMapCache.port)
    {
      std::array<uint32_t, kPortVrfEntries> m{};
      StringValue sv;
      g_portToVrfMap.GetValue (sv);
      ParseVrfMap (sv.Get (), kPortVrfEntries - 1, m.data ());
      g_vrfMapCache.port = m;
    }
  return (portIdx < kPortVrfEntries) ? (*g_vrfMapCache.port)[portIdx] : 0;
}

bool
SueReadConf (const std::string &confFile)
{
  std::ifstream f (confFile.c_str ());
  if (!f.is_open ()) { NS_LOG_WARN ("SueReadConf: cannot open " << confFile); return false; }
  std::string line;
  uint32_t lineno = 0;
  while (std::getline (f, line))
    {
      ++lineno;
      // strip '#' comments and re-read the remaining "KEY VALUE" tokens
      std::string::size_type h = line.find ('#');
      if (h != std::string::npos) { line.erase (h); }
      std::istringstream iss (line);
      std::string key, val;
      if (!(iss >> key)) { continue; }
      if (!(iss >> val)) { continue; }
      if (!SueSetGlobal (key, val))
        {
          NS_LOG_LOGIC ("SueReadConf: unknown key '" << key << "' (line " << lineno << ") skipped");
        }
    }
  // The parse may have changed SUE_ETH_TYPE_* / *_HAS_SHIM / the VC maps: drop
  // the first-use caches so the next accessor call re-reads (DESIGN §9; also
  // safe if no such key was set - the caches just refill from the unchanged
  // Globals).
  SueConfig::InvalidateCache ();
  // Prime BOTH first-use caches here (single-threaded config load, before any
  // node LP runs) so an MTP run never cold-caches concurrently from two LPs:
  // the VC maps on the first frame decode, and the EtherType/*HAS_SHIM cache on
  // the first frame build/parse (SueHeader ctor / Deserialize). Cheap: a few
  // GlobalValue reads once per conf load.
  SueConfig::DscpToVc (0);
  SueConfig::PcpToVc (0, false);
  SueConfig::PcpToVc (0, true);
  SueConfig::DefaultVc ();
  SueConfig::DscpToVrf (0);
  SueConfig::VlanToVrf (0);
  SueConfig::PortToVrf (0);
  SueConfig::EthType0 ();
  SueConfig::EthType1 ();
  SueConfig::HasShim (SueConfig::EthType0 ());
  SueConfig::HasShim (SueConfig::EthType1 ());
  return true;
}

void
ApplySueConfig (Ptr<SueNetDevice> dev)
{
  if (!dev) { return; }
  UintegerValue uv; IntegerValue iv; StringValue sv;
  // LLR replay/init timers: 0 (= auto) defers to a rate-derived 10x LLR RTT
  // (DESIGN §12.27) once the ACK period is known below; >0 stays explicit.
  bool autoReplay = false, autoInit = false;

  // --- device-level knobs ---
  g_pfcPauseQuanta.GetValue (uv); dev->SetPfcPauseQuanta ((uint16_t) (uv.Get () & 0xFFFF));
  g_ccMacDa.GetValue (sv);
  {
    // Defensive parse (no ns-3 string-ctor abort on bad input): 6 hex bytes.
    uint32_t b[6] = {0, 0, 0, 0, 0, 0};
    if (std::sscanf (sv.Get ().c_str (), "%x:%x:%x:%x:%x:%x",
                     &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
      {
        uint8_t m[6];
        for (uint32_t i = 0; i < 6; ++i) { m[i] = (uint8_t) (b[i] & 0xFF); }
        Mac48Address da;
        da.CopyFrom (m);
        dev->SetCcUpdateMacDa (da);
      }
    else
      {
        NS_LOG_WARN ("ApplySueConfig: CBFC_CC_MACDA '" << sv.Get () << "' unparsable — keeping default");
      }
  }
  g_ccCid.GetValue (uv); dev->SetCcUpdateCid (uv.Get ());

  // --- flow-control facade: build + wire when FC_MODE != OFF and none present ---
  if (!dev->GetFcController ())
    {
      g_fcMode.GetValue (sv);
      const std::string mode = sv.Get ();
      if (mode == "CBFC")
        {
          Ptr<SueCbfcEngine> cbfc = dev->GetCbfcEngine ();
          if (!cbfc) { cbfc = CreateObject<SueCbfcEngine> (); }
          Ptr<SueFcController> fc = CreateObject<SueFcController> ();
          fc->SetCbfc (cbfc);
          dev->SetFcController (fc);
        }
      else if (mode == "PFC")
        {
          Ptr<SuePfcEngine> pfc = dev->GetPfcEngine ();
          if (!pfc) { pfc = CreateObject<SuePfcEngine> (); }
          Ptr<SueFcController> fc = CreateObject<SueFcController> ();
          fc->SetPfc (pfc);
          dev->SetFcController (fc);
        }
      // FC_MODE == OFF: leave engines as-is (no facade)
    }

  // --- CBFC engine (now present via facade wiring if FC_MODE=CBFC) ---
  if (dev->GetCbfcEngine ())
    {
      Ptr<SueCbfcEngine> cbfc = dev->GetCbfcEngine ();
      g_creditSize.GetValue (uv); cbfc->SetCreditSize (uv.Get ());
      g_txCellChoices.GetValue (sv);
      {
        std::vector<uint32_t> choices;
        std::stringstream ss (sv.Get ());
        std::string item;
        while (std::getline (ss, item, ','))
          {
            std::stringstream is (item);
            uint32_t c;
            if ((is >> c) && c > 0) { choices.push_back (c); }
          }
        if (!choices.empty ()) { cbfc->SetTxCellChoices (choices); }
      }
      g_pktOvhd.GetValue (iv); cbfc->SetPktOvhd (iv.Get ());
      g_vcCreditLimit.GetValue (sv);
      std::stringstream ss (sv.Get ());
      std::string item;
      while (std::getline (ss, item, ','))
        {
          std::stringstream is (item);
          uint32_t vc, limit; char colon;
          if ((is >> vc >> colon >> limit) && colon == ':') { cbfc->SetVcCreditLimit (vc, limit); }
        }
      // UEC Table 5-27 requires 1 <= VC_CreditLimit[x]: a VC left at 0 by an
      // empty/partial CBFC_VC_CREDIT_LIMIT config would be PERMANENTLY gated
      // (CanSend false forever — a silent deadlock). Backfill the registration
      // default and warn so a typo'd map never bricks the VC silently.
      for (uint32_t v = 0; v < SueCbfcEngine::NVC; ++v)
        {
          if (cbfc->GetVcCreditLimit (v) == 0)
            {
              NS_LOG_WARN ("ApplySueConfig: VC " << v << " credit limit 0 (unset) — "
                           "backfilled to 512; UEC Table 5-27 requires >= 1");
              cbfc->SetVcCreditLimit (v, 512);
            }
        }
      cbfc->Init ();
    }
  // 数据量触发(SDK credit_threshold 对等物;字节键,独立于端口速率)。
  g_cfThresholdBytes.GetValue (uv);
  if (uv.Get () > 0) { dev->SetCfUpdateThresholdBytes (uv.Get ()); }

  // --- PFC engine (now present via facade wiring if FC_MODE=PFC) ---
  if (dev->GetPfcEngine ())
    {
      Ptr<SuePfcEngine> pfc = dev->GetPfcEngine ();
      // §6.4 modeling constraint: drainDelay > 0 or the buffer never
      // accumulates and pause never triggers (config-path PFC silently ~OFF).
      g_pfcDrainDelayNs.GetValue (uv);
      if (uv.Get () > 0) { dev->SetDrainDelay (NanoSeconds (uv.Get ())); }
      g_pfcBufferSize.GetValue (uv); pfc->SetBufferSize (uv.Get ());
      g_pfcDynamicThreshold.GetValue (uv);
      if (uv.Get () != 0)
        {
          uint32_t shared, headroom, resume, alpha;
          g_pfcSharedPool.GetValue (uv); shared = uv.Get ();
          g_pfcHeadroom.GetValue (uv); headroom = uv.Get ();
          g_pfcResumeOffset.GetValue (uv); resume = uv.Get ();
          g_pfcAlphaShift.GetValue (uv); alpha = uv.Get ();
          pfc->EnableSharedPool (shared, headroom, resume, alpha);
        }
      else
        {
          g_pfcPauseThresh.GetValue (uv); pfc->SetPauseThreshold (uv.Get ());
          g_pfcResumeThresh.GetValue (uv); pfc->SetResumeThreshold (uv.Get ());
        }
    }

  // --- CC_Update leak-recovery period (CBFC) ---
  if (dev->GetCbfcEngine ())
    {
      g_ccMsgTimer.GetValue (uv);
      if (uv.Get () > 0) { dev->StartCcUpdateTimer (MicroSeconds (uv.Get ())); }
    }

  // --- CF_Update min/max timers (UEC §5.2.6.1, DESIGN §5.6/§12.2) ---
  // CF_min/CF_max are given as wire-byte thresholds and converted to time at
  // the link rate (t = bytes*8/rate). Both 0 (default) keeps the legacy
  // per-frame CF_Update emission; sue-default.conf and the S8 scenario set
  // 125000 @100G = 10µs so CF_Update generation is timer-gated and the
  // CtlOS pacing FIFO stays stable (ACK + sparse CF_Update < drain rate).
  if (dev->GetCbfcEngine () && dev->IsDataRateSet ())
    {
      g_cfMinTimer.GetValue (uv);
      uint32_t cfMinB = uv.Get ();
      g_cfMaxTimer.GetValue (uv);
      uint32_t cfMaxB = uv.Get ();
      dev->SetCfUpdateTimers (dev->GetDataRate ().CalculateBytesTxTime (cfMinB),
                              dev->GetDataRate ().CalculateBytesTxTime (cfMaxB));
    }

  // --- LLR engine (if wired) ---
  if (dev->GetLlrEngine ())
    {
      Ptr<SueLlrEngine> llr = dev->GetLlrEngine ();
      // replay/init timers: explicit ns when > 0; 0 = auto-derive 10x LLR RTT
      // (DESIGN §12.27). A fixed ns default (the old 1000) is pathological at
      // 1G where the CtlOS gate (400B = 3.2µs) and ACK cadence (2048B = 16.4µs)
      // both exceed it -> spurious replay -> replay_ct exhausted -> FLUSH
      // cascade (s6-entry regression).
      g_replayTimerMax.GetValue (uv);
      if (uv.Get () == 0) { autoReplay = true; } else { llr->SetReplayTimerMax (NanoSeconds (uv.Get ())); }
      g_replayCtMax.GetValue (uv); llr->SetReplayCtMax (uv.Get ());
      g_dataAgeTimerMax.GetValue (uv); llr->SetDataAgeTimerMax (NanoSeconds (uv.Get ()));
      g_initTimerMax.GetValue (uv);
      if (uv.Get () == 0) { autoInit = true; } else { llr->SetInitTimerMax (NanoSeconds (uv.Get ())); }
      g_initRetryMax.GetValue (uv); llr->SetInitRetryMax (uv.Get ());
      g_outstandingSeqMax.GetValue (uv); llr->SetOutstandingSeqMax (uv.Get ());
      g_outstandingDataMax.GetValue (uv); llr->SetOutstandingDataMax (uv.Get ());
      g_replayBufMax.GetValue (uv); llr->SetReplayBufMax (uv.Get ());
      g_pcsLostTimerMax.GetValue (uv); llr->SetPcsLostTimerMax (NanoSeconds (uv.Get ()));
      // UEC §5.1.4 two-phase modes + INIT/FLUSH behaviors + re-init (DESIGN §9)
      g_llrModeLocal.GetValue (uv); llr->SetModeLocal (uv.Get () != 0);
      g_llrModeRemote.GetValue (uv); llr->SetModeRemote (uv.Get () != 0);
      g_llrInitBehavior.GetValue (sv); llr->SetInitBehavior (sv.Get ());
      g_llrFlushBehavior.GetValue (sv); llr->SetFlushBehavior (sv.Get ());
      g_llrReInitOnDiscard.GetValue (uv); llr->SetReInitOnDiscard (uv.Get () != 0);
    }

  // --- CtlOS wire gate + LLR ACK period: TWO independent quantities that
  // ApplySueConfig historically conflated (DESIGN §12.26). Both need a real
  // link rate (SetCtlOsTargetSpacing asserts otherwise); SetupSueNetwork
  // applies after link creation. Apply only when SetDataRate() was called
  // (explicit IsDataRateSet flag, not a comparison against the base's
  // placeholder rate). ---
  if (dev->IsDataRateSet ())
    {
      // Hard pacing gate (UEC §5.2.11.2): at least 400 B between ANY two
      // CtlOS — the 2%-of-bandwidth ceiling. Fixed regardless of the ACK
      // cadence. With gate > ackPeriod the ACK stream alone can occupy every
      // CtlOS slot on a control-only link and starve CF_UPDATE.
      dev->SetCtlOsTargetSpacing (CTLOS_MIN_SPACING_BYTES);
      g_ctlosTargetSpacing.GetValue (uv);
      // UEC Table 5-9: ctlos_target_spacing valid range [400, 16384] bytes —
      // 400B = max-rate boundary ("no CtlOS inserted in the first 400 bytes"),
      // 16384 < 17296B min-rate boundary. Clamp out-of-range configs.
      uint32_t spacing = uv.Get ();
      if (spacing < 400) { spacing = 400; }
      if (spacing > 16384) { spacing = 16384; }
      // UEC Table 5-9 / §5.1.6: ACK/NACK CtlOS are generated at the
      // ctlos_target_spacing cadence (periodic cumulative ACK, idle repeats
      // the last seq). This rides ON TOP of the 400B wire gate above; it is
      // NOT the gate. 0 stays per-frame only on non-config paths (tests /
      // scratch without ApplySueConfig); ApplySueConfig resolves >= 400B.
      if (dev->GetLlrEngine ())
        {
          const Time ackPeriod = dev->GetDataRate ().CalculateBytesTxTime (spacing);
          dev->GetLlrEngine ()->SetAckPeriod (ackPeriod);
          if (autoReplay || autoInit)
            {
              // §12.25/§12.27: auto timers = 10 x LLR RTT, RTT = 2*wire(data
              // frame + IFG) + 2*prop + ackPeriod. prop is read from the
              // SUE_LINK_PROPAGATION_NS config key (default 0: zero-propagation
              // baseline) instead of a hardcoded 1µs — a hardcoded value made
              // the auto path replay-pathological on links with prop > 1µs
              // (timer < real RTT -> spurious replays -> replay_ct FLUSH
              // cascade, the §12.27 failure mode) and 44x over-conservative at
              // the 10ns presets. Scales with the link rate, so a static
              // default can't be tight at 1G (ACK 16.4µs) yet loose enough at
              // 400G.
              const Time wireCycle = dev->GetDataRate ().CalculateBytesTxTime (LLR_AUTO_TIMER_FRAME_BYTES)
                                   + dev->GetDataRate ().CalculateBytesTxTime (LLR_AUTO_TIMER_IFG_BYTES);
              const Time rtt = 2 * wireCycle + 2 * SueConfig::LinkPropagation () + ackPeriod;
              if (autoReplay) { dev->GetLlrEngine ()->SetReplayTimerMax (10 * rtt); }
              if (autoInit) { dev->GetLlrEngine ()->SetInitTimerMax (10 * rtt); }
            }
        }
    }

  // --- IFG (§5.4): wire-time of SUE_IFG_BYTES at the link rate (not a fixed
  // 96ns, which is only correct at 1Gbps). Needs the rate for the conversion;
  // the device default m_ifg=0 holds until then. ---
  if (dev->IsDataRateSet ())
    {
      g_ifgBytes.GetValue (uv);
      dev->SetIfg (dev->GetDataRate ().CalculateBytesTxTime (uv.Get ()));
  // UE link negotiation (NEG-SP-01/02): enabled per device from the config key.
  g_lldpNegotiate.GetValue (uv);
  if (uv.Get () != 0) { dev->EnableLldpNegotiation (); }
    }
}

} // namespace ns3

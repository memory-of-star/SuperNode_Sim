/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE Scale-Up Ethernet - SueNetDevice (S2+ S3: wire-time + IFG + 8-VC queue
 * + CBFC + LLR). TX: SwitchSend -> Enqueue -> DequeueAndTransmit (RR+credit
 * gate -> OnTx -> LLR TxStore+replay -> prepend preamble -> TransmitStart).
 * RX: strip preamble -> LLR RxFrame (FCS+seq) -> OnRx -> drain -> CF_Update.
 */
#ifndef SUE_NET_DEVICE_H
#define SUE_NET_DEVICE_H

#include "ns3/point-to-point-net-device.h"
#include "ns3/ptr.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/mac48-address.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "sue-header.h"
#include "sue-mtp.h"
#include "sue-ctl-header.h"
#include "sue-lldp-header.h"
#include "sue-fc-controller.h"
#include <deque>
#include <functional>
#include <utility>

namespace ns3 {

class SueChannel;
class SueEgressQueue;
class SueCbfcEngine;
class SueErrorModel;
class SueLlrEngine;
class SuePfcEngine;
class Packet;

class SueNetDevice : public PointToPointNetDevice
{
public:
  static TypeId GetTypeId (void);
  SueNetDevice ();
  virtual ~SueNetDevice ();

  bool Attach (Ptr<SueChannel> ch);
  /** The attached SueChannel. NOTE: GetChannel() (base PointToPointNetDevice)
   *  returns the base's own m_channel member, which Attach never sets (it sets
   *  the subclass's m_channel); topology code must use this accessor. */
  Ptr<SueChannel> GetSueChannel () const;
  /** Return the attached SueChannel as a base Channel. Overrides the inherited
   *  PointToPointNetDevice::GetChannel() (which returns the base m_channel that
   *  SueNetDevice::Attach never sets — it sets the subclass m_channel). The MTP
   *  auto-partition (DESIGN.md §10.5) relies on GetChannel() being non-null to
   *  discover link delay and compute a real lookahead; without this override it
   *  sees no links and leaves lookahead at Time::Max()/2, which lets each
   *  logical process run its whole event list ahead of the mailbox clock. */
  virtual Ptr<Channel> GetChannel (void) const override;

  typedef Callback<void, Ptr<SueNetDevice>, Ptr<Packet>, SueHeader> ReceiveUpcall;
  void SetReceiveUpcall (ReceiveUpcall cb);

  void SetEgressQueue (Ptr<SueEgressQueue> q);
  void SetIfg (Time ifg);
  void SetCbfcEngine (Ptr<SueCbfcEngine> cbfc);
  Ptr<SueCbfcEngine> GetCbfcEngine () const;
  Ptr<SueLlrEngine> GetLlrEngine () const;
  Ptr<SuePfcEngine> GetPfcEngine () const;
  Ptr<SueErrorModel> GetErrorModel () const;
  /** Set flow-control facade (FC_MODE); wires its engines like the direct setters. */
  void SetFcController (Ptr<SueFcController> fc);
  Ptr<SueFcController> GetFcController () const;
  typedef Callback<void, uint32_t, uint32_t> CfUpdateSink;
  void SetCfUpdateSink (CfUpdateSink sink);
  void HandleCfUpdate (uint32_t vc, uint32_t rVcCf);
  void SetDrainDelay (Time d);

  // --- LLR ---
  void SetLlrEnabled (bool on);
  void SetErrorModel (Ptr<SueErrorModel> em);
  void SetLlrEngine (Ptr<SueLlrEngine> llr);
  /** Minimal PCS_status signal (UEC Table 5-10): FALSE models a link glitch
   *  (闪断). While FALSE the device drops every incoming frame (the physical
   *  layer cannot decode) and the LLR engine's pcs_lost_status_timer runs.
   *  The scenario drives this on both link ends for the glitch duration
   *  (e.g. via Simulator::Schedule); no PCS sublayer is modeled. */
  void SetPcsStatus (bool up);
  /** UEC Table 5-10 LLR_status, forwarded from the engine
   *  (SueLlrEngine::GetLlrStatus). */
  bool GetLlrStatus () const;
  // --- PFC (S4b: alternative to CBFC) ---
  void SetPfcEngine (Ptr<SuePfcEngine> pfc);
  /** Peer delivered a pause/resume -> update sender gate. */
  void HandlePfc (uint32_t vc, bool isPause);
  /** Engine callback: retransmit a stored MAC frame with its seq. */
  void Retransmit (uint32_t seq, Ptr<Packet> macFrame);

  // --- CtlOS on-wire TX (§5.5/§5.7) ---
  void SendCtlAckNack (uint32_t seq, bool isNack);
  void SendCtlInit (uint32_t initSeq, uint16_t initData);
  void SendCtlInitEcho (uint32_t initSeq, uint16_t initData);
  void SendCtlCfUpdate (uint32_t vc, uint32_t cf);
  void SendCtlCcUpdate (void);
  void SendCtlPause (uint32_t vc, bool isPause);
  void HandleCtlFrame (Ptr<Packet> p, SueCtlHeader::CtlType type);
  /** CtlOS send path (ACK/NACK/INIT/INIT_ECHO/CF_UPDATE). Enforces the
   *  optional min-interval pacing gate (§5.7); PFC/CC_Update are real
   *  Ethernet frames and bypass pacing (they use TransmitControl directly). */
  void SendCtlFrame (SueCtlHeader::CtlType type, Ptr<Packet> payload);
  /** Min interval between successive CtlOS transmissions (0 = disabled). */
  void SetCtlOsMinInterval (Time interval);
  /** Rate-control knob: min CtlOS spacing in wire bytes, converted to a time
   *  interval via the data rate. This is the pacing GATE for ANY two CtlOS
   *  (UEC §5.2.11.2: at least 400 B apart — the 2%-of-bandwidth ceiling), and
   *  is INDEPENDENT of the LLR ACK period: ApplySueConfig fixes the gate at
   *  the 400B floor (CTLOS_MIN_SPACING_BYTES) and drives the engine ACK
   *  cadence separately from LLR_CTLOS_TARGET_SPACING (DESIGN §12.26). Set the
   *  data rate before calling this. */
  void SetCtlOsTargetSpacing (uint32_t bytes);
  /** Set a real link data rate. NOTE: the base PointToPointNetDevice::SetDataRate
   *  is non-virtual and its default is a 32768 b/s placeholder; this subclass
   *  method records that a real rate was applied (IsDataRateSet) and forwards to
   *  the base. Call sites use Ptr<SueNetDevice>, so this resolves. */
  void SetDataRate (DataRate bps);
  /** True once SetDataRate has been called (real link rate applied). Used
   *  instead of comparing GetDataRate() against the base's placeholder. */
  bool IsDataRateSet () const;
  /** PFC pause_time_quanta for pause frames (802.1Qbb; 0xFFFF = pause until
   *  explicit resume, which matches the engine's explicit-resume semantic). */
  void SetPfcPauseQuanta (uint16_t q);
  /** Own MAC used as the MACSA of PFC/CC_Update Ethernet frames. */
  Mac48Address GetMacSa () const;
  /** Start periodic CC_Update timer (CBFC leak recovery, §5.2.5.6.4). */
  void StartCcUpdateTimer (Time interval);
  /** CC_Update MACDA / CID override (UEC §5.2.6.2 "建议可配", ULL-CBFC-MSG-CC-003).
   *  Defaults are the spec values (01-80-C2-00-00-01 / FA-7A-CB) — the wire is
   *  byte-identical unless explicitly reconfigured. */
  void SetCcUpdateMacDa (Mac48Address da) { m_ccUpdateMacDa = da; }
  Mac48Address GetCcUpdateMacDa () const { return m_ccUpdateMacDa; }
  void SetCcUpdateCid (uint32_t cid) { m_ccUpdateCid = cid & 0xFFFFFF; }
  uint32_t GetCcUpdateCid () const { return m_ccUpdateCid; }
  /** UEC §5.2.6.1 CF_Update generation (DESIGN §5.6/§12.2): gate per-drain
   *  CF_Update emission on the CF_min/CF_max timers instead of emitting one
   *  CF_Update per drained frame. Both intervals are derived by the config
   *  from the wire-byte thresholds at the link rate. Passing (0,0) keeps the
   *  legacy per-frame path (tests / non-config defaults). The CF_max timer
   *  (periodic full update of all R_VC_CF) is armed here, on the owning
   *  node's LP. */
  void SetCfUpdateTimers (Time minInterval, Time maxInterval);
  /** Data-volume trigger (SDK credit_threshold 对等物): after accumulated
   *  freed-credit BYTES reach the threshold, request a CF_Update of the dirty
   *  mask. Combined with CF_min>0 the floor grid dominates (chip 同构);
   *  standalone (CF_MIN=CF_MAX=0) it gives data-proportional emission —
   *  CF count ~= traffic bytes / threshold, independent of the port rate. */
  void SetCfUpdateThresholdBytes (uint64_t bytes) { m_cfThresholdBytes = bytes; m_cfAccumBytes = 0; }
  /** CBFC egress→ingress backpressure (§6.7/§12.30): when deferred, Receive()
   *  charges R_VC_CC but does NOT refund the credit (no OnDrain) until the
   *  switch node has admitted the frame onto an egress port and calls
   *  RefundCredit(). Without this, the synchronous drain at m_drainDelay=0
   *  decouples credit refund from egress congestion and a 2:1 incast never
   *  throttles the senders. Host devices keep the default (false) and refund
   *  synchronously. */
  void SetDeferCreditRefund (bool on);
  /** UE link negotiation (UEC §5.3, NEG-SP-01/02 + CBFC-SP-13): when enabled,
   *  the device sends its capability TLV at link start and the LLR engines are
   *  started only AFTER the peer's TLV is applied (LLR enabled iff both sides
   *  support it; CBFC sender adopts the receiver's advertised credit size /
   *  per-VC limits once). The §5.2.8.4 RTS/RTR handshake then runs as a
   *  multi-round exchange: VCs may not use credits until both the local
   *  S_VC_RTS and the remote R_VC_RTR bits are set. Off by default (config
   *  key LLDP_NEGOTIATE). */
  void EnableLldpNegotiation ();
  bool GetLldpNegotiate () const { return m_lldpNegotiate; }
  /** Send the local capability TLV now (requires the channel attached) and
   *  arm the no-peer fallback. Called instead of the direct LLR Start(). */
  void StartLldpExchange ();
  /** Management-plane want flip (ULL-NEG-CBF-RCV-006 / ULL-CBFC-INIT-008,
   *  UEC §5.2.8.5): the receiver revokes (or re-grants) lossless service for
   *  one VC after the exchange started. Updates the local want snapshot +
   *  advertised bitmap, re-derives R_VC_RTR from the current S_VC_RTS and
   *  re-sends the LLDP frame; the peer's assignment semantics then clear its
   *  S_VC_RTS and its lossless-active gate closes again (no BE channel in the
   *  minimal model — that VC's frames queue, §12.32). */
  void SetVcWant (uint8_t vc, bool on);
  /** Remote LLDP frame processed: first-frame block (adopt parameters once,
   *  cancel fallback, start LLR) + per-frame RTS/RTR state machine. */
  void OnRemoteLldpTlv ();
  void ApplyLldpFallback ();      // timeout: no peer -> local-only + start
  /** ULL-CBFC-INIT-001 (UEC §5.2.8.1) adoption-consistency observables: set
   *  when the first remote TLV disagrees with the local config (NumVCs /
   *  lossless-VC want set). Adoption still proceeds (warning only — the
   *  RTS/RTR gate already handles a mismatched VC set functionally). */
  bool GetLldpNumVcsMismatch () const { return m_lldpNumVcsMismatch; }
  bool GetLldpLosslessSetMismatch () const { return m_lldpLosslessSetMismatch; }
  /** Rebuild the LLDP frame from the current rts/rtr bitmaps and transmit it
   *  (handshake round; control path, no pacing gate). */
  void SendLldpWithState ();
  bool GetDeferCreditRefund () const { return m_deferCreditRefund; }
  /** Public drain (credit refund) used by the switch node once an ingress frame
   *  is admitted onto an egress port (or dropped by the switch). Routes through
   *  the same CF_min/CF_max-gated OnDrain as the synchronous path. */
  void RefundCredit (uint32_t vc, uint32_t bytes);
  /** Egress-queue depth admission (switch node's Forward/ServicePending):
   *  per-VC byte depth limit set via SetEgressQueueVcLimit. */
  bool EgressCanAccept (uint32_t vc, uint32_t bytes) const;
  void SetEgressQueueVcLimit (uint32_t vc, uint64_t bytes);
  /** Callback fired after each egress dequeue/transmit in DequeueAndTransmit
   *  (a slot freed). The switch node hooks this to ServicePending(port). */
  void SetEgressFreedHook (std::function<void (void)> hook);
  /** Unified counter dump (DESIGN.md §6.11): LLR + CBFC counters to FILE*. */
  void PrintCounters (FILE *f, Time now);

  bool SwitchSend (uint32_t qIndex, Ptr<Packet> p);

  void Receive (Ptr<Packet> p);
  void DequeueAndTransmit (void);
  void TransmitComplete (void);
  bool TransmitStart (Ptr<Packet> p);

protected:
  virtual void DoDispose (void);

private:
  /** Timer arm, §10.5. Under the fork's MTP the plain Schedule routes to the
   *  CURRENT LP and returns a cancellable EventId, whereas ScheduleWithContext
   *  returns void and cross-LP mailbox events are fire-and-forget (not
   *  cancellable) — so node timers must be armed from the owning node's LP.
   *  Every arm site here sits in the data plane (Receive / TX / pacing
   *  callbacks), which the channel's context-routed receive seam keeps on node
   *  LPs; the CC_Update cadence, armed at config time on the public LP, is
   *  deferred to the node's LP by StartCcUpdateTimer. Plain Schedule is
   *  behavior-neutral on the default DefaultSimulatorImpl (it ignores context),
   *  so the single-threaded baseline is unchanged. The resident-LP convention
   *  is enforced by SueMtpResidencyCheck (§10.5 驻留不变量): the check aborts
   *  under MTP if this arm ever runs off the device node's owning LP. */
  template <typename FN, typename... Args>
  EventId
  ScheduleSelf (Time delay, FN fn, Args&&... args)
  {
    NS_ASSERT_MSG (GetNode () || !SueMtpActive (),
                   "MTP residency violation: SueNetDevice armed a timer without a node");
    if (GetNode ())
      {
        SueMtpResidencyCheck (GetNode ()->GetId ());
      }
    return Simulator::Schedule (delay, fn, std::forward<Args> (args)...);
  }

  void OnDrain (uint32_t vc, uint32_t bytes);
  void OnDrainPfc (uint32_t vc, uint32_t bytes);
  void OnCcUpdateTimer (void);
  /** UEC §5.2.6.1 trigger 1: R_VC_CF[vc] updated — mark dirty; arm the CF_min
   *  one-shot if it isn't running, at m_lastCfUpdate + CF_min (not Now +
   *  CF_min) so the fire time is a deterministic grid independent of which
   *  drain armed it (the send happens when it fires, see OnCfMinTimer). */
  void OnCfDirty (uint32_t vc);
  /** CF_min one-shot fired: send the dirty VCs (batch), clear + stamp. */
  void OnCfMinTimer (void);
  /** UEC trigger 2: CF_max timer fired — periodic full update of all
   *  R_VC_CF values (loss resilience); self-rearms. */
  void OnCfMaxTimer (void);
  /** Send CF_Update CtlOS for the VCs set in vcMask (2 entries per 8B
   *  message, §5.6), each through the CtlOS pacing gate. */
  void SendCfUpdateBatch (uint16_t vcMask);
  /** §10.5 MTP: arm the CC_Update cadence on the owning node's LP (StartCcUpdateTimer
   *  is called at config time on the public LP; this is the deferred re-arm). */
  void ArmCcUpdateTimerOnLp (Time interval);
  /** §10.5 MTP: arm the CF_max periodic full-update timer on the owning node's
   *  LP (SetCfUpdateTimers is called at config time on the public LP; this is
   *  the deferred re-arm). */
  void ArmCfUpdateTimerOnLp (Time maxInterval);
  /** Pacing timer fired: transmit the next deferred CtlOS (FIFO). */
  void OnCtlOsPacingTimer (void);
  /** Common control-frame TX path: busy -> m_ctlQueue (priority), else start. */
  void TransmitControl (Ptr<Packet> p);
  /** Try to send one frame from the retx queue (if not busy/paused). Returns true if sent. */
  bool DrainRetx ();
  uint8_t ComputeVc (const SueHeader &h) const;
  void PrependLlrPreamble (Ptr<Packet> p, uint32_t seq, bool eligible);
  void OnLlrStateChange ();   // engine -> ADVANCE: clear retx + retry dequeue
  void OnLlrTxOpportunity (); // engine: partial window release -> retry dequeue

  /** Flow-control gate for a VC, via the facade if set, else the legacy PFC
   *  engine. CBFC mode (facade or legacy) has no pause -> never blocked. */
  bool FcPaused (uint32_t vc) const;
  /** Enter/leave the LLR replay window for CBFC credit accounting, via the
   *  facade if set, else the legacy engine. No-op for PFC. */
  void FcSuspendForLlrWindow (bool on) const;

  Ptr<SueChannel> m_channel;
  bool m_rateSet{false};   // SetDataRate() applied a real link rate
  Ptr<SueEgressQueue> m_queue;
  Ptr<SueCbfcEngine> m_cbfc;
  Ptr<SueFcController> m_fc;   // flow-control facade (optional; FC_MODE)
  Ptr<SueErrorModel> m_errModel;
  Ptr<SueLlrEngine> m_llr;
  Ptr<SuePfcEngine> m_pfc;
  Time m_ifg;
  Time m_drainDelay;
  bool m_deferCreditRefund{false};   // CBFC backpressure (§12.30): switch ports refund only on egress admission
  bool m_lldpNegotiate{false};       // UE link negotiation enabled (NEG-SP-01/02)
  bool m_lldpRemoteRcvd{false};
  bool m_lldpFirstRemoteApplied{false};   // first-frame block ran (adopt + LLR start)
  bool m_lldpNumVcsMismatch{false};       // remote NumVCs != local (§5.2.8.1 WARN)
  Mac48Address m_ccUpdateMacDa{"01:80:c2:00:00:01"};   // CC_Update MACDA (§5.2.6.2 可配)
  uint32_t m_ccUpdateCid{0xFA7ACB};                    // CC_Update CID (§5.2.6.2 可配)
  bool m_lldpLosslessSetMismatch{false};  // remote want bitmap != local (§5.2.8.1 WARN)
  SueLldpHeader m_lldpLocal, m_lldpRemote;
  EventId m_lldpFallbackTimer;
  // §5.2.8.4 handshake state (CBFC-SP-13): monotone OR bitmaps, one bit per VC.
  uint8_t m_lldpWant{0};   // local R_VC_Want (VCs with configured CL > 0)
  uint8_t m_lldpRts{0};    // local S_VC_RTS (= remote want, OR-accumulated)
  uint8_t m_lldpRtr{0};    // local R_VC_RTR (= remote rts & want, OR-accumulated)
  std::function<void (void)> m_egressFreedHook;   // switch node hook: ServicePending(port) on slot freed
  bool m_busy{false};
  bool m_llrEnabled{false};
  bool m_pcsStatus{true};   // minimal PCS_status signal (glitch model)
  uint32_t m_llrSeq{0};   // simple seq (S3a, no engine)
  ReceiveUpcall m_upcall;
  CfUpdateSink m_cfSink;
  TracedCallback<Ptr<const Packet>> m_rxTrace;
  // Retransmit queue (priority over new data during REPLAY)
  struct RetxEntry { uint32_t seq; Ptr<Packet> p; };
  std::deque<RetxEntry> m_retxQueue;
  std::deque<Ptr<Packet>> m_ctlQueue;   // control frames (ACK/NACK/INIT/CF_Update/PFC), priority TX
  Time m_ccUpdateInterval{Seconds (0)}; // 0 = disabled; else periodic CC_Update
  EventId m_ccUpdateTimer;
  // CtlOS pacing (§5.7): min interval between successive CtlOS transmissions.
  Time m_ctlOsMinInterval{Seconds (0)};   // 0 = disabled
  Time m_lastCtlOsTx{Seconds (0)};        // last CtlOS admitted to the wire path
  bool m_ctlOsFirstTx{true};              // first CtlOS always passes the gate
  std::deque<Ptr<Packet>> m_ctlOsPending; // CtlOS deferred by the pacing gate (FIFO)
  EventId m_ctlOsEvent;
  // CF_Update generation (UEC §5.2.6.1, DESIGN §5.6/§12.2): CF_min = one-shot
  // flush floor for updated VCs (dirty-trigger), CF_max = periodic full
  // update of all R_VC_CF (loss resilience; typically 10x CF_min so the full
  // update never coincides with the dirty cadence). Both 0 = legacy per-frame
  // emission.
  Time m_cfMinInterval{Seconds (0)};
  Time m_cfMaxInterval{Seconds (0)};
  Time m_lastCfUpdate{Seconds (0)};
  uint16_t m_cfDirtyMask{0};   // VCs whose R_VC_CF changed since last CF_Update
  uint64_t m_cfThresholdBytes{0};  // credit_threshold 对等物:累计释放信用字节
                                    // ≥ 阈值即触发一次 CF_Update(0=关闭,零回归)
  uint64_t m_cfAccumBytes{0};      // 自上次 CF_Update 以来累计的释放字节
  EventId m_cfMinTimer;
  EventId m_cfMaxTimer;
  uint16_t m_pfcPauseQuanta{0xFFFF};      // PFC pause quanta (0xFFFF = pause until resume)
};

} // namespace ns3

#endif /* SUE_NET_DEVICE_H */

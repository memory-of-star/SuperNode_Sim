/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE LLR engine (DESIGN.md §6.2, UEC §5.1). Per-device TX/RX state machines,
 * replay buffer, ACK/NACK processing, retransmit, FLUSH.
 *
 * UEC Table 5-10/5-11 coverage: seq assign + replay + NACK-triggered
 *   retransmit + ACK release + replay_ct_max -> FLUSH + INIT re-sync +
 *   POISONED path + data_age timer + 22-item counters + acked_seq /
 *   init_echo_sent ACK trigger / outstanding_seq formula / LLR_status +
 *   minimal pcs_lost_status_timer (PCS_status signal only; no PCS sublayer).
 */
#ifndef SUE_LLR_ENGINE_H
#define SUE_LLR_ENGINE_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/nstime.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "sue-llr-counters.h"
#include "sue-mtp.h"
#include <deque>
#include <cstdint>
#include <string>
#include <utility>

namespace ns3 {

class SueLlrEngine : public Object
{
public:
  static const uint32_t SEQ_MOD = 1u << 20;   // 20-bit sequence space

  static TypeId GetTypeId (void);
  SueLlrEngine ();

  enum class TxState { OFF, INIT, ADVANCE, REPLAY, FLUSH };

  /** How the TX LLR handles new LLR-desired frames from the MAC client
   *  (UEC §5.1.4 Table 5-9 / §5.1.5). BLOCK: do not take frames (stay queued,
   *  lossless). DISCARD: take + drop, counted in tx_discard. BEST_EFFORT: pass
   *  through without seq assignment / replay buffer (as if mode_remote OFF).
   *  LLR_ACTIVE: eligible frame (ADVANCE/REPLAY with mode_remote ON) -> seq +
   *  replay. */
  enum class TxBehavior { BLOCK, DISCARD, BEST_EFFORT, LLR_ACTIVE };

  // --- TX ---
  /** True if a new LLR-desired frame may be accepted (§5.1.5): ADVANCE state +
   *  outstanding_seq/data/replay-buffer limits. */
  bool CanTx () const;
  /** Assign seq, store MAC frame in replay buffer. Returns seq. */
  uint32_t TxStore (Ptr<Packet> macFrame);
  /** Start replay timer for the last stored frame (if not already running). */
  void StartReplayTimer ();
  /** ACK/NACK received from receiver. */
  void OnAckNack (uint32_t seq, bool isNack);
  /** Replay timer expired. */
  void OnReplayTimer ();

  // --- RX ---
  enum RxResult { ACCEPT, DROP, ACCEPT_POISONED };
  /** Process received frame. fcsStatus: GOOD/POISONED/BAD.
   *  POISONED: accepted (seq++), no NACK, frame_bad reported (§5.1.10). */
  RxResult RxFrame (uint32_t seq, int fcsStatus);   // 0=GOOD,1=POISONED,2=BAD

  // --- init / config ---
  void Start ();   // m_tx = INIT, send INIT, wait for INIT_ECHO
  void SetReplayCtMax (uint32_t n);
  void SetReplayTimerMax (Time t);
  void SetOutstandingSeqMax (uint32_t n);
  void SetOutstandingDataMax (uint32_t bytes);
  void SetReplayBufMax (uint32_t n);
  void SetDataAgeTimerMax (Time t);
  void SetInitRetryMax (uint32_t n);
  /** INIT handshake retry timeout (independent of the replay timer). */
  void SetInitTimerMax (Time t);
  /** pcs_lost_status_timer_max (UEC Table 5-10): how long PCS_status may stay
   *  FALSE in ADVANCE/REPLAY before the timer expires and forces FLUSH. 0
   *  (default) = immediate expiry (UEC "0 = immediate expiration"). */
  void SetPcsLostTimerMax (Time t);
  /** Minimal PCS_status signal (UEC Table 5-10). FALSE models a link glitch
   *  (闪断): while the TX LLR is in ADVANCE/REPLAY the pcs_lost_status_timer
   *  runs; on expiry the machine FLUSHes (replay buffer discarded) even with
   *  no unacknowledged frames, then re-syncs via INIT. No PCS sublayer is
   *  modeled — the scenario drives this per device (e.g. Simulator::Schedule).
   *  The device also drops incoming frames while PCS_status is FALSE. */
  void SetPcsStatus (bool up);
  bool GetPcsStatus () const;
  /** UEC Table 5-10 pcs_lost_status_timer_expired (latched until PCS recovers). */
  bool GetPcsLostTimerExpired () const;

  // --- NS3_MTP context routing (DESIGN.md §10.5) ---
  /** Bind this engine's timers to a node context. Under the single-process
   *  multithreaded simulator the node owns all of its state on one logical
   *  process, so every engine timer must be scheduled (and cancelled) on the
   *  owning node's context — otherwise it lands on the public LP and a later
   *  EventId::Cancel from the node's LP cannot find it. Set by
   *  SueNetDevice::SetLlrEngine from the owning node's id. */
  void SetContext (uint32_t nodeId) { m_context = nodeId; }
  uint32_t GetContext () const { return m_context; }

  // --- UEC §5.1.4 two-phase modes + INIT/FLUSH behaviors (DESIGN §9) ---
  /** llr_mode_local: ON = RX LLR active (LLR_INIT may be received -> re-sync +
   *  INIT_ECHO; LLR-eligible frames are seq-filtered). OFF = RX passes frames
   *  through unfiltered. */
  void SetModeLocal (bool on);
  bool GetModeLocal () const;
  /** llr_mode_remote: ON = TX LLR active (LLR_INIT may be sent; eligible frames
   *  get seq + replay). OFF = TX uses BEST_EFFORT. */
  void SetModeRemote (bool on);
  bool GetModeRemote () const;
  /** llr_init_behavior / llr_flush_behavior: DISCARD|BLOCK|BEST_EFFORT. */
  void SetInitBehavior (const std::string &b);
  void SetFlushBehavior (const std::string &b);
  /** re_init_on_discard: TRUE = FLUSH auto-enters INIT (send INIT); FALSE =
   *  stay in FLUSH until management calls Start(). */
  void SetReInitOnDiscard (bool on);
  /** TX handling style for a new LLR-desired frame in the current state. */
  TxBehavior GetTxBehavior () const;
  TxBehavior GetInitBehavior () const;
  TxBehavior GetFlushBehavior () const;
  /** Count a tx_discard for a DISCARD-style drop (UEC: DISCARD counted in
   *  tx_discards). */
  void DiscardNew ();
  /** COUNTER-007 (§5.1.10): an eligible frame this engine transmitted with a
   *  poisoned FCS (TX-side poisoning; device drives via CheckPoisonTx). */
  void IncTxPoisoned ();

  // --- callbacks (engine -> device/peer) ---
  typedef Callback<void, uint32_t, bool> AckNackSink;   // (seq, isNack) -> peer
  void SetAckNackSink (AckNackSink cb);
  /** UEC §5.1.6 ACK pacing: 0 (default) = ACK per received frame; > 0 = once the
   *  first LLR_INIT_ECHO has been sent (init_echo_sent — the chip's trigger
   *  source for entering SEND_ACKS) a cumulative ACK(next_rx_seq-1) is sent on
   *  each period, with or without new frames (idle ACKs repeat the last seq; the
   *  sender dup-skips them). Suppressed while NACK_SENT. Period = wire time of
   *  ctlos_target_spacing at the link rate. */
  void SetAckPeriod (Time t);
  Time GetAckPeriod () const { return m_ackPeriod; }
  typedef Callback<void, uint32_t, Ptr<Packet>> RetransmitCb;  // (seq, macFrame) -> device
  void SetRetransmitCb (RetransmitCb cb);
  /** FLUSH -> send INIT(initSeq, initData) to the peer receiver to re-sync
   *  next_rx_seq; initData is this engine's optional RTT field (§5.1.9.1.1). */
  typedef Callback<void, uint32_t, uint16_t> InitSink;
  void SetInitSink (InitSink cb);
  /** Receiver: INIT arrived -> reset next_rx_seq + nackSent + send INIT_ECHO
   *  (init_data echoed verbatim, opaque to the receiver, §5.1.9.1.1). */
  void OnInit (uint32_t initSeq, uint16_t initData = 0);
  /** Receiver -> sender: INIT_ECHO callback (seq, echoed init_data). */
  typedef Callback<void, uint32_t, uint16_t> InitEchoSink;
  void SetInitEchoSink (InitEchoSink cb);
  /** Sender: INIT_ECHO received -> ADVANCE; captures the echoed init_data
   *  (GetPeerInitData) for RTT measurement. */
  void OnInitEcho (uint32_t initSeq, uint16_t initData = 0);
  /** Optional init_data this engine puts into its LLR_INIT (§5.1.9.1.1). */
  void SetInitData (uint16_t d) { m_initData = d; }
  uint16_t GetInitData () const { return m_initData; }
  uint16_t GetPeerInitData () const { return m_peerInitData; }
  /** Sender: init timeout -> resend INIT (up to retry max), then GoAdvance. */
  void OnInitTimeout ();
  /** Engine -> device: TX state changed to ADVANCE (wake up queued frames). */
  typedef Callback<void> StateChangeCb;
  void SetStateChangeCb (StateChangeCb cb);
  /** Engine -> device: an ACK/NACK freed window space while frames are still
   *  buffered (partial release). The device should re-attempt DequeueAndTransmit
   *  without waiting for the whole window to drain (stop-and-go prevention). */
  typedef Callback<void> TxOpportunityCb;
  void SetTxOpportunityCb (TxOpportunityCb cb);
  /** Engine -> device: the §5.2.9 LLR window (TX REPLAY state OR RX discard
   *  window = NACK sent) opened (true) / closed (false). The device forwards
   *  this into the CBFC engine's m_inLlrWindow so rules 3/9 gate the REAL
   *  windows (which span many events), not just the synchronous retransmit. */
  typedef Callback<void, bool> LlrWindowCb;
  void SetLlrWindowCb (LlrWindowCb cb);

  // --- for test / observe ---
  TxState GetTxState () const;
  uint32_t GetTxSeq () const;
  /** UEC Table 5-10 acked_seq: sequence number acknowledged in the most
   *  recently successfully received LLR_ACK or LLR_NACK (public observe). */
  uint32_t GetAckedSeq () const;
  uint32_t GetNextRxSeq () const;
  /** UEC §5.1.7: next_rx_seq is valid (set TRUE by a received LLR_INIT with
   *  llr_mode_local ON; FALSE until then / after FLUSH until re-INIT). */
  bool GetNextRxSeqVld () const { return m_nextRxSeqVld; }
  /** UEC Table 5-10 LLR_status: TRUE when llr_mode_remote is ON and the TX LLR
   *  is in ADVANCE/REPLAY; when llr_mode_remote is OFF (no TX LLR) it tracks
   *  PCS_status (pass-through). */
  bool GetLlrStatus () const;
  /** UEC Table 5-10 outstanding_seq = ((tx_seq - 1) - acked_seq) mod 2^20 (a
   *  sequence-space quantity; equals the replay-buffer size in every state). */
  uint32_t GetOutstandingSeq () const { return OutstandingSeq (); }
  size_t GetReplaySize () const;
  uint64_t GetCntTxDiscard () const;
  const SueLlrCounters &GetCounters () const;

private:
  void DoReplay ();
  void GoAdvance ();   // m_tx=ADVANCE + cancel timer + fire state-change cb
  /** §5.2.9 window = (m_tx == REPLAY) || m_nackSent; push changes via m_llrWindowCb. */
  void RecomputeLlrWindow ();
  void StartInit ();   // send INIT + start init timer
  void StartOnLp ();   // §10.5: StartInit run on the owning node's LP (MTP kick)
  /** FLUSH: discard path done -> mark FLUSH, then enter INIT and wait for
   *  INIT_ECHO (reuses StartInit's send + timeout/retry machinery). */
  void FlushAndInit ();
  /** Independent data-age timer fired (buffer-driven; §5.1.4). */
  void OnDataAge ();
  /** UEC §5.1.6: periodic ACK cadence timer (armed on init_echo_sent, enters
   *  SEND_ACKS; cumulative ACK each period, suppressed while NACK_SENT). */
  void OnAckTimer ();
  /** Arm the pcs_lost_status_timer when PCS_status is FALSE and the TX LLR is
   *  in ADVANCE/REPLAY (UEC Table 5-10 rule: timer stopped in OFF/INIT/FLUSH).
   *  No-op if it is already running. */
  void ArmPcsLostTimer ();
  /** pcs_lost_status_timer expired: progress to FLUSH (even with an empty
   *  replay buffer), then re-sync the peer via INIT. */
  void OnPcsLostTimeout ();
  /** UEC Table 5-10: outstanding_seq = ((tx_seq - 1) - acked_seq) mod 2^20 —
   *  the number of unacknowledged frames in the sequence space (not a buffer
   *  length). Valid because acked_seq is reset to all-1s / init_seq-1 and
   *  tx_seq to 0 on llr_mode_remote change. */
  uint32_t OutstandingSeq () const;
  uint32_t OutstandingData () const;

  /** Timer arm, §10.5. Under the fork's MTP the plain Schedule routes to the
   *  CURRENT LP and returns a cancellable EventId, whereas ScheduleWithContext
   *  returns void and cross-LP mailbox events are fire-and-forget (not
   *  cancellable) — so node timers must be armed from the owning node's LP.
   *  That residency is guaranteed by the channel's existing context-routed
   *  receive seam (the whole data plane runs on node LPs) plus Start()'s t==0
   *  context kick. Plain Schedule is behavior-neutral on the default
   *  DefaultSimulatorImpl (it ignores context), so the single-threaded baseline
   *  is unchanged. The resident-LP convention is enforced by
   *  SueMtpResidencyCheck (§10.5 驻留不变量): the check aborts under MTP if this
   *  arm ever runs off the node's owning LP. */
  template <typename FN, typename... Args>
  EventId
  ScheduleCtx (Time delay, FN fn, Args&&... args)
  {
    NS_ASSERT_MSG (m_context != Simulator::NO_CONTEXT || !SueMtpActive (),
                   "MTP residency violation: SueLlrEngine armed a timer without a node context");
    if (m_context != Simulator::NO_CONTEXT)
      {
        SueMtpResidencyCheck (m_context);
      }
    return Simulator::Schedule (delay, fn, std::forward<Args> (args)...);
  }

  // UEC §5.1.4 two-phase modes + behaviors (defaults preserve the historical
  // "single LLR enable" = both modes ON; BLOCK = frames stay queued in
  // INIT/FLUSH, lossless; re_init auto = current FlushAndInit).
  bool m_modeLocal{true};
  bool m_modeRemote{true};
  TxBehavior m_initBehavior{TxBehavior::BLOCK};
  TxBehavior m_flushBehavior{TxBehavior::BLOCK};
  bool m_reInitOnDiscard{true};

  TxState m_tx{TxState::OFF};
  uint32_t m_txSeq{0};
  // UEC Table 5-10 acked_seq: seq confirmed by the most recent successfully
  // received ACK/NACK. 20-bit; init to all-1s (SEQ_MOD-1, "nothing acked yet"),
  // reset to all-1s on llr_mode_remote change, reset to init_seq-1 on LLR_INIT.
  uint32_t m_ackedSeq{SEQ_MOD - 1};
  uint32_t m_nextRxSeq{0};
  bool m_nextRxSeqVld{false};   // UEC §5.1.7: TRUE after a valid INIT, FALSE otherwise
  bool m_nackSent{false};
  // §12.48(UEC §5.2.9 note 3): CC counter check stays INVALID until every
  // frame discarded during the NACK window has been re-accepted (next_rx_seq
  // passed the highest discarded seq). The window (CC_Update drop gate) is
  // extended accordingly — a CC_Update landing mid-GoBackN-burst refunds
  // in-flight retransmits as "lost"; when they land and refund normally the
  // credit returns twice (CU=(CC-CF) mod 2^15 flips → permanent block).
  uint32_t m_discardMaxSeq{0};
  bool m_discardActive{false};

public:
  /** §12.48: frames discarded during the last NACK window have NOT all been
   *  re-accepted yet (next_rx_seq has not passed the highest discarded seq).
   *  The CC counter check is INVALID in this state (UEC §5.2.9 note 3's
   *  rationale) — the CC_Update handler must drop the message, else it
   *  refunds in-flight retransmits as "lost" and they refund again on
   *  landing (double refund → CU mod-2^15 flip, §12.48). */
  bool HasPendingDiscard () const { return m_discardActive; }

private:
  uint64_t m_txDiscard{0};
  SueLlrCounters m_cnt;

  struct ReplayEntry { uint32_t seq; Ptr<Packet> p; Time ts; };
  std::deque<ReplayEntry> m_replayBuf;

  uint32_t m_replayCt{0};
  uint32_t m_replayCtMax{5};
  // No-config floor (DESIGN §12.27): the config path auto-derives 10x LLR RTT
  // when LLR_REPLAY_TIMER_MAX_NS=0; this static floor backs only engines built
  // without ApplySueConfig and must exceed the 1G CtlOS gate (400B = 3.2µs).
  Time m_replayTimerMax{NanoSeconds (5000)};
  EventId m_replayTimer;

  // §5.1.5 limits
  uint32_t m_outstandingSeqMax{524288};   // UEC Table 5-9 absolute maximum
  uint32_t m_outstandingDataMax{0xFFFFFFFFu};
  uint32_t m_replayBufMax{1u << 20};
  // §5.1.4 data-age timer (independent: armed on empty->non-empty buffer)
  Time m_dataAgeTimerMax{Seconds (10)};
  EventId m_dataAgeTimer;
  // INIT handshake
  uint32_t m_initRetryMax{3};
  uint32_t m_initRetry{0};
  Time m_initTimerMax{NanoSeconds (5000)};   // independent of m_replayTimerMax; §12.27 floor, see above
  EventId m_initTimer;
  InitEchoSink m_initEchoSink;
  uint16_t m_initData{0};
  uint16_t m_peerInitData{0};

  AckNackSink m_ackNackSink;
  RetransmitCb m_retransmitCb;
  InitSink m_initSink;
  StateChangeCb m_stateChangeCb;
  TxOpportunityCb m_txOpportunityCb;
  LlrWindowCb m_llrWindowCb;
  bool m_llrWindowNotified{false};   // last value pushed via m_llrWindowCb

  // UEC §5.1.6 periodic ACK pacing (0 = per-frame, the historical behavior).
  // > 0: armed on init_echo_sent (SEND_ACKS), self-rearming, cumulative ACK.
  Time m_ackPeriod{Seconds (0)};
  // CTL-INIT-001 (UEC §5.1.9.1.1): a subsequent LLR_INIT is sent after at
  // least 4x ctlos_target_spacing bytes of wire time. The engine's ackPeriod
  // IS the spacing converted to time (device SetAckPeriod from config), so the
  // INIT re-arm floor tracks it as 4x; 0 = no spacing configured -> no floor.
  Time m_initSpacingFloor{Seconds (0)};
  EventId m_ackTimer;
  // UEC Table 5-10 pcs_lost_status_timer: PCS_status FALSE in ADVANCE/REPLAY
  // for longer than pcs_lost_status_timer_max -> FLUSH even with no unacked
  // frames. Minimal PCS model: a scenario-driven bool (no PCS sublayer).
  bool m_pcsStatus{true};
  Time m_pcsLostTimerMax{Seconds (0)};   // UEC: 0 = immediate expiry
  EventId m_pcsLostTimer;
  bool m_pcsLostTimerExpired{false};     // latched until PCS recovers

  // §10.5 MTP: owning node id (0xffffffff = none), set by
  // SueNetDevice::SetLlrEngine. Used only to route Start()'s boot kick to the
  // node's LP; the timer arms themselves rely on LP residency (ScheduleCtx).
  uint32_t m_context{Simulator::NO_CONTEXT};
};

} // namespace ns3

#endif /* SUE_LLR_ENGINE_H */

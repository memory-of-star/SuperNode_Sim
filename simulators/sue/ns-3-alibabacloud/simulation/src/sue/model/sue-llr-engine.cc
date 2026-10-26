/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "sue-llr-engine.h"
#include "sue-mtp.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueLlrEngine");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueLlrEngine);

TypeId
SueLlrEngine::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueLlrEngine")
    .SetParent<Object> ()
    .AddConstructor<SueLlrEngine> ()
  ;
  return tid;
}

SueLlrEngine::SueLlrEngine () {}

/** Parse an INIT/FLUSH behavior string (UEC §5.1.4: DISCARD|BLOCK|BEST_EFFORT).
 *  Unknown values fall back to BLOCK (lossless; the historical behavior). */
static SueLlrEngine::TxBehavior
ParseBehavior (const std::string &b)
{
  if (b == "DISCARD") { return SueLlrEngine::TxBehavior::DISCARD; }
  if (b == "BEST_EFFORT") { return SueLlrEngine::TxBehavior::BEST_EFFORT; }
  return SueLlrEngine::TxBehavior::BLOCK;
}

void SueLlrEngine::SetReplayCtMax (uint32_t n) { m_replayCtMax = n; }
void SueLlrEngine::SetReplayTimerMax (Time t) { m_replayTimerMax = t; }
void
SueLlrEngine::SetOutstandingSeqMax (uint32_t n)
{
  // UEC Table 5-9: "The absolute maximum permitted value ... is 524288" — an
  // outstanding window beyond half the 2^20 seq space makes the §5.1.8 ok-formula
  // ambiguous. Clamp and warn instead of honoring the request.
  if (n > 524288u)
    {
      NS_LOG_WARN ("SetOutstandingSeqMax(" << n << ") clamped to UEC max 524288");
      n = 524288u;
    }
  m_outstandingSeqMax = n;
}
void SueLlrEngine::SetOutstandingDataMax (uint32_t b) { m_outstandingDataMax = b; }
void SueLlrEngine::SetReplayBufMax (uint32_t n) { m_replayBufMax = n; }
void SueLlrEngine::SetDataAgeTimerMax (Time t) { m_dataAgeTimerMax = t; }
void SueLlrEngine::SetInitRetryMax (uint32_t n) { m_initRetryMax = n; }
void SueLlrEngine::SetInitTimerMax (Time t) { m_initTimerMax = t; }
void SueLlrEngine::SetPcsLostTimerMax (Time t) { m_pcsLostTimerMax = t; }
bool SueLlrEngine::GetPcsStatus () const { return m_pcsStatus; }
bool SueLlrEngine::GetPcsLostTimerExpired () const { return m_pcsLostTimerExpired; }

bool
SueLlrEngine::GetLlrStatus () const
{
  // UEC Table 5-10: LLR_status = TRUE when llr_mode_remote is ON and the TX LLR
  // is in ADVANCE/REPLAY; with llr_mode_remote OFF the TX LLR is off and
  // LLR_status tracks PCS_status (pass-through).
  if (!m_modeRemote) { return m_pcsStatus; }
  return m_tx == TxState::ADVANCE || m_tx == TxState::REPLAY;
}

void
SueLlrEngine::SetPcsStatus (bool up)
{
  if (up == m_pcsStatus) { return; }
  m_pcsStatus = up;
  if (!up)
    {
      // UEC Table 5-10: the pcs_lost_status_timer is stopped in OFF/INIT/FLUSH
      // and activated when PCS_status is FALSE in ADVANCE/REPLAY.
      ArmPcsLostTimer ();
    }
  else
    {
      // PCS recovered before expiry: the timer is cancelled (held at zero) and
      // the expired latch clears.
      if (m_pcsLostTimer.IsRunning ()) { m_pcsLostTimer.Cancel (); }
      m_pcsLostTimerExpired = false;
    }
}

void
SueLlrEngine::ArmPcsLostTimer ()
{
  if (!m_pcsStatus
      && (m_tx == TxState::ADVANCE || m_tx == TxState::REPLAY)
      && !m_pcsLostTimer.IsRunning ())
    {
      m_pcsLostTimer = ScheduleCtx (m_pcsLostTimerMax,
                                    &SueLlrEngine::OnPcsLostTimeout, this);
    }
}

void
SueLlrEngine::OnPcsLostTimeout ()
{
  m_pcsLostTimerExpired = true;
  // UEC Table 5-10: expiry progresses the LLR TX state machine into FLUSH even
  // if there are no unacknowledged frames — discard whatever remains and
  // re-sync the peer via the INIT handshake. No-op if we already left
  // ADVANCE/REPLAY (e.g. the machine FLUSHed via data_age in the meantime).
  if (m_tx == TxState::ADVANCE || m_tx == TxState::REPLAY)
    {
      NS_LOG_LOGIC ("pcs_lost_status_timer expired -> FLUSH ("
                    << m_replayBuf.size () << " frames in buffer)");
      m_cnt.tx_discard += m_replayBuf.size ();
      m_replayBuf.clear ();
      m_replayCt = 0;
      FlushAndInit ();
    }
}
void SueLlrEngine::SetModeLocal (bool on) { m_modeLocal = on; }
bool SueLlrEngine::GetModeLocal () const { return m_modeLocal; }
void
SueLlrEngine::SetModeRemote (bool on)
{
  if (on != m_modeRemote)
    {
      // UEC Table 5-10: tx_seq is set to 0 and acked_seq to all-1s whenever
      // llr_mode_remote changes. The TX LLR path is re-initialized (replay
      // buffer cleared, replay count zeroed, replay/data-age/pcs-lost timers
      // cancelled) so the UEC outstanding_seq formula stays consistent with the
      // buffer. (A runtime toggle mid-traffic needs a management re-INIT to
      // re-sync the peer's next_rx_seq.)
      m_txSeq = 0;
      m_ackedSeq = SEQ_MOD - 1;
      m_replayBuf.clear ();
      m_replayCt = 0;
      if (m_replayTimer.IsRunning ()) { m_replayTimer.Cancel (); }
      if (m_dataAgeTimer.IsRunning ()) { m_dataAgeTimer.Cancel (); }
      if (m_pcsLostTimer.IsRunning ()) { m_pcsLostTimer.Cancel (); }
    }
  m_modeRemote = on;
}
bool SueLlrEngine::GetModeRemote () const { return m_modeRemote; }
void SueLlrEngine::SetInitBehavior (const std::string &b) { m_initBehavior = ParseBehavior (b); }
void SueLlrEngine::SetFlushBehavior (const std::string &b) { m_flushBehavior = ParseBehavior (b); }
void SueLlrEngine::SetReInitOnDiscard (bool on) { m_reInitOnDiscard = on; }
void SueLlrEngine::DiscardNew () { ++m_cnt.tx_discard; }
void SueLlrEngine::IncTxPoisoned () { ++m_cnt.tx_poisoned; }
SueLlrEngine::TxBehavior SueLlrEngine::GetInitBehavior () const { return m_initBehavior; }
SueLlrEngine::TxBehavior SueLlrEngine::GetFlushBehavior () const { return m_flushBehavior; }
void SueLlrEngine::SetAckNackSink (AckNackSink cb) { m_ackNackSink = cb; }
void
SueLlrEngine::SetAckPeriod (Time t)
{
  m_ackPeriod = t;
  // CTL-INIT-001: the INIT re-send floor follows the configured CtlOS spacing
  // (4x wire bytes, §5.1.9.1.1) whenever a spacing is configured.
  m_initSpacingFloor = t * 4;
}
void SueLlrEngine::SetRetransmitCb (RetransmitCb cb) { m_retransmitCb = cb; }
void SueLlrEngine::SetInitSink (InitSink cb) { m_initSink = cb; }
void SueLlrEngine::SetInitEchoSink (InitEchoSink cb) { m_initEchoSink = cb; }
void SueLlrEngine::SetStateChangeCb (StateChangeCb cb) { m_stateChangeCb = cb; }
void SueLlrEngine::SetTxOpportunityCb (TxOpportunityCb cb) { m_txOpportunityCb = cb; }
void SueLlrEngine::SetLlrWindowCb (LlrWindowCb cb) { m_llrWindowCb = cb; }

void
SueLlrEngine::RecomputeLlrWindow ()
{
  // §5.2.9: the LLR window spans the TX replay burst (REPLAY state, many
  // events) AND the RX discard window (NACK sent until the retransmit lands).
  // Previously the CBFC engine's m_inLlrWindow was only held during the
  // synchronous Retransmit TransmitStart call — periodic CC_Updates could fire
  // mid-window (rule 9 violation) and be processed mid-window (rule 3
  // violation: CC/CF skew -> sender CU ~2^15 -> VC misfire outage). The real
  // window is state-derived here and pushed to the device on every change.
  bool active = (m_tx == TxState::REPLAY) || m_nackSent;
  if (active != m_llrWindowNotified)
    {
      m_llrWindowNotified = active;
      if (!m_llrWindowCb.IsNull ()) { m_llrWindowCb (active); }
    }
}

SueLlrEngine::TxState SueLlrEngine::GetTxState () const { return m_tx; }

SueLlrEngine::TxBehavior
SueLlrEngine::GetTxBehavior () const
{
  // UEC §5.1.5: LLR_OFF -> BEST_EFFORT; INIT/FLUSH -> the configured behavior;
  // ADVANCE/REPLAY -> eligible frames (seq + replay). mode_remote OFF disables
  // the TX LLR entirely (BEST_EFFORT), so no INIT may be sent and no seq is
  // assigned.
  if (!m_modeRemote) { return TxBehavior::BEST_EFFORT; }
  switch (m_tx)
    {
    case TxState::INIT:   return m_initBehavior;
    case TxState::FLUSH:  return m_flushBehavior;
    case TxState::ADVANCE:
    case TxState::REPLAY: return TxBehavior::LLR_ACTIVE;
    case TxState::OFF:
    default:              return TxBehavior::BEST_EFFORT;
    }
}
uint32_t SueLlrEngine::GetTxSeq () const { return m_txSeq; }
uint32_t SueLlrEngine::GetAckedSeq () const { return m_ackedSeq; }
uint32_t SueLlrEngine::GetNextRxSeq () const { return m_nextRxSeq; }
size_t SueLlrEngine::GetReplaySize () const { return m_replayBuf.size (); }
uint64_t SueLlrEngine::GetCntTxDiscard () const { return m_cnt.tx_discard; }
const SueLlrCounters &SueLlrEngine::GetCounters () const { return m_cnt; }

uint32_t
SueLlrEngine::OutstandingSeq () const
{
  // UEC Table 5-10: outstanding_seq = ((tx_seq - 1) - acked_seq) mod 2^20 —
  // a sequence-space quantity, not a buffer length. It equals the replay-buffer
  // size in every state because acked_seq is reset to all-1s / init_seq-1 and
  // tx_seq to 0 on llr_mode_remote change (verified in SueLlrAckedSeqTestCase).
  return (m_txSeq + SEQ_MOD - 1 - m_ackedSeq) % SEQ_MOD;
}

uint32_t
SueLlrEngine::OutstandingData () const
{
  uint32_t bytes = 0;
  for (const auto &e : m_replayBuf) { bytes += e.p->GetSize (); }
  return bytes;
}

bool
SueLlrEngine::CanTx () const
{
  return m_tx == TxState::ADVANCE
    && OutstandingSeq () < m_outstandingSeqMax
    && OutstandingData () < m_outstandingDataMax
    && m_replayBuf.size () < m_replayBufMax;
}

void
SueLlrEngine::GoAdvance ()
{
  // UEC Table 5-13: tx_replay = "number of times that the transmitter exited
  // the REPLAY state (i.e., completed a replay operation)" — count per REPLAY
  // session completion, not per retransmitted frame. (Exits to FLUSH are not
  // completed replays and are not counted.)
  if (m_tx == TxState::REPLAY) { ++m_cnt.tx_replay; }
  m_tx = TxState::ADVANCE;
  if (m_replayTimer.IsRunning ()) { m_replayTimer.Cancel (); }
  if (m_initTimer.IsRunning ()) { m_initTimer.Cancel (); }
  if (m_dataAgeTimer.IsRunning ()) { m_dataAgeTimer.Cancel (); }
  ArmPcsLostTimer ();   // UEC: PCS_status FALSE in ADVANCE arms the timer
  RecomputeLlrWindow ();   // TX replay window closed (unless RX still NACKing)
  if (!m_stateChangeCb.IsNull ()) { m_stateChangeCb (); }
}

// ---- INIT handshake (§5.1.5 INIT state) ----
void
SueLlrEngine::StartInit ()
{
  // UEC §5.1.4: LLR_INIT may be sent only when llr_mode_remote is ON.
  if (!m_modeRemote) { GoAdvance (); return; }
  m_tx = TxState::INIT;
  // UEC Table 5-10 rule 1: the pcs_lost_status_timer is stopped in INIT.
  if (m_pcsLostTimer.IsRunning ()) { m_pcsLostTimer.Cancel (); }
  m_initRetry = 0;
  if (!m_initSink.IsNull ())
    {
      ++m_cnt.tx_init_ctl_os;
      // UEC Table 5-10: acked_seq is set to init_seq-1 when sending an LLR_INIT.
      uint32_t initSeq = m_txSeq % SEQ_MOD;
      m_ackedSeq = (initSeq + SEQ_MOD - 1) % SEQ_MOD;
      m_initSink (initSeq, m_initData);
      m_initTimer = ScheduleCtx (std::max (m_initTimerMax, m_initSpacingFloor),
                                 &SueLlrEngine::OnInitTimeout, this);
    }
  else
    {
      GoAdvance ();   // no peer sink wired yet (test pattern: Start before SetLlrEngine)
      return;
    }
  RecomputeLlrWindow ();   // REPLAY (if any) -> INIT: TX window closed
}

void
SueLlrEngine::Start ()
{
  SueMtpContractCheck ();   // MTP enabled after the cached lookup -> loud abort
  // §10.5 MTP: Start() is normally called during setup on the public LP, but
  // the INIT frame and the init/replay timers must run from the owning node's
  // LP (the fork's ScheduleWithContext returns no cancellable EventId). Under
  // MTP a t==0 context event is kicked here; Partition() re-homes it onto the
  // node's LP (InvokeNow) before any thread starts, and StartInit's arms land
  // in that LP's heap. Behavior-neutral on the default DefaultSimulatorImpl
  // (SueMtpActive() is false), so the single-threaded baseline is unchanged —
  // including the synchronous-return contract the tests rely on.
  if (SueMtpActive () && m_context != Simulator::NO_CONTEXT)
    {
      Simulator::ScheduleWithContext (m_context, TimeStep (0),
                                      &SueLlrEngine::StartOnLp, this);
      return;
    }
  StartInit ();
}

void
SueLlrEngine::StartOnLp ()
{
  StartInit ();
}

void
SueLlrEngine::OnInit (uint32_t initSeq, uint16_t initData)
{
  // UEC §5.1.4: an LLR_INIT may be received only when llr_mode_local is ON.
  if (!m_modeLocal) { return; }
  // Receiver: re-sync next_rx_seq + nackSent, reply INIT_ECHO.
  // UEC §5.1.7: next_rx_seq becomes valid (TRUE) when a LLR_INIT is received
  // with llr_mode_local ON; eligible frames are filtered until then.
  m_nextRxSeq = initSeq % SEQ_MOD;
  m_nextRxSeqVld = true;
  m_nackSent = false;
  m_discardActive = false; m_discardMaxSeq = 0;
  RecomputeLlrWindow ();   // re-sync closes the RX discard window
  ++m_cnt.rx_init_ctl_os;
  if (!m_initEchoSink.IsNull ())
    {
      ++m_cnt.tx_init_echo_ctl_os;
      m_initEchoSink (m_nextRxSeq, initData);
      // UEC §5.1.6: once an LLR_INIT_ECHO has been sent (init_echo_sent — the
      // chip's trigger source), the ACK/NACK machine enters SEND_ACKS and ACKs
      // are sent on the ackPeriod cadence. This replaces the old forward-
      // progress (per-received-frame) arming.
      if (m_ackPeriod > Seconds (0) && !m_ackTimer.IsRunning ())
        {
          m_ackTimer = ScheduleCtx (m_ackPeriod, &SueLlrEngine::OnAckTimer, this);
        }
    }
}

void
SueLlrEngine::OnInitEcho (uint32_t initSeq, uint16_t initData)
{
  // Sender: INIT_ECHO received. UEC §5.1.9.1.2: only an echo carrying the
  // same init_seq as our INIT (== tx_seq at INIT time) advances us out of
  // INIT; a stale / mismatched echo is ignored (still counted).
  ++m_cnt.rx_init_echo_ctl_os;
  if (initSeq != (m_txSeq % SEQ_MOD))
    {
      NS_LOG_LOGIC ("INIT_ECHO seq=" << initSeq
                    << " != tx_seq=" << (m_txSeq % SEQ_MOD) << "; ignore");
      return;
    }
  if (m_initTimer.IsRunning ()) { m_initTimer.Cancel (); }
  m_peerInitData = initData;
  GoAdvance ();
}

void
SueLlrEngine::OnInitTimeout ()
{
  if (m_initRetry < m_initRetryMax)
    {
      ++m_initRetry;
      ++m_cnt.tx_init_ctl_os;
      // UEC Table 5-10: acked_seq set to init_seq-1 on each LLR_INIT send (resend).
      uint32_t initSeq = m_txSeq % SEQ_MOD;
      m_ackedSeq = (initSeq + SEQ_MOD - 1) % SEQ_MOD;
      m_initSink (initSeq, m_initData);
      m_initTimer = ScheduleCtx (std::max (m_initTimerMax, m_initSpacingFloor),
                                 &SueLlrEngine::OnInitTimeout, this);
    }
  else
    {
      NS_LOG_WARN ("INIT handshake timeout after " << m_initRetryMax << " retries; proceeding");
      GoAdvance ();
    }
}

void
SueLlrEngine::FlushAndInit ()
{
  // FLUSH (§5.1.5): replay frames discarded; re-sync the peer via the INIT
  // handshake (send INIT, wait for INIT_ECHO). StartInit sends the INIT (single
  // send, counting tx_init_ctl_os once) and schedules the init timeout; if no
  // peer sink is wired it falls back to GoAdvance immediately. With
  // re_init_on_discard=FALSE (UEC §5.1.4) the machine stays in FLUSH and waits
  // for management intervention (Start() re-enters INIT).
  m_tx = TxState::FLUSH;
  if (m_replayTimer.IsRunning ()) { m_replayTimer.Cancel (); }
  if (m_dataAgeTimer.IsRunning ()) { m_dataAgeTimer.Cancel (); }
  if (m_ackTimer.IsRunning ()) { m_ackTimer.Cancel (); }
  // UEC Table 5-10 rule 1: the pcs_lost_status_timer is stopped in FLUSH (the
  // expired latch is held until PCS recovers, for observability).
  if (m_pcsLostTimer.IsRunning ()) { m_pcsLostTimer.Cancel (); }
  // UEC §5.1.7: on link teardown (FLUSH) the receiver's next_rx_seq is stale
  // until the re-INIT handshake re-syncs it — clear vld so eligible frames are
  // filtered (not mis-parsed as expected) in the interim.
  m_nextRxSeqVld = false;
  if (m_reInitOnDiscard) { StartInit (); }
  else { RecomputeLlrWindow (); }   // REPLAY -> FLUSH: TX window closed
}

void
SueLlrEngine::OnDataAge ()
{
  if (m_replayBuf.empty ()) { return; }
  Time oldestAge = Simulator::Now () - m_replayBuf.front ().ts;
  // UEC: the data_age_timer expires at dataAgeTimerMax. Use >= (not >): at
  // age == max the re-arm delay (max - age) would be zero and OnDataAge would
  // re-schedule itself at the same timestamp forever (a stall whose oldest
  // frame ages to exactly max would spin the event loop).
  if (oldestAge >= m_dataAgeTimerMax)
    {
      NS_LOG_LOGIC ("data_age timer (independent) expired (age "
                    << oldestAge.GetNanoSeconds () << "ns) -> FLUSH");
      m_cnt.tx_discard += m_replayBuf.size ();
      m_replayBuf.clear ();
      m_replayCt = 0;
      FlushAndInit ();
      return;
    }
  // Front advanced (ACKs released some frames): re-arm for the remaining age.
  m_dataAgeTimer = ScheduleCtx (m_dataAgeTimerMax - oldestAge,
                                &SueLlrEngine::OnDataAge, this);
}

// ---- TX ----

uint32_t
SueLlrEngine::TxStore (Ptr<Packet> macFrame)
{
  uint32_t seq = m_txSeq++ % SEQ_MOD;
  ++m_cnt.tx_ok;
  bool wasEmpty = m_replayBuf.empty ();
  m_replayBuf.push_back ({seq, macFrame, Simulator::Now ()});
  // §5.1.4 independent data-age timer: arm on the empty->non-empty transition
  // (one timer covers the whole burst) so a buffer stall with no replay timer
  // scheduled still FLUSHes the oldest frame when it exceeds dataAgeTimerMax.
  if (wasEmpty && m_dataAgeTimerMax > Seconds (0))
    {
      m_dataAgeTimer = ScheduleCtx (m_dataAgeTimerMax, &SueLlrEngine::OnDataAge, this);
    }
  return seq;
}

void
SueLlrEngine::StartReplayTimer ()
{
  if (!m_replayTimer.IsRunning () && m_tx == TxState::ADVANCE && !m_replayBuf.empty ())
    {
      m_replayTimer = ScheduleCtx (m_replayTimerMax, &SueLlrEngine::OnReplayTimer, this);
    }
}

void
SueLlrEngine::OnAckNack (uint32_t seq, bool isNack)
{
  uint32_t ack_nack_seq = seq % SEQ_MOD;
  // UEC Table 5-13: rx_ack_ctl_os / rx_nack_ctl_os count CtlOS RECEIVED —
  // out-of-range and duplicate ACKs are received too (the spec defines
  // rx_ack_nack_seq_error separately, which implies such CtlOS are counted on
  // arrival). Counting before validation keeps the two ends of a link paired
  // (peer tx_ack == local rx_ack) even on an idle link where every periodic
  // cumulative ACK is a duplicate seq.
  if (isNack) { ++m_cnt.rx_nack_ctl_os; }
  else { ++m_cnt.rx_ack_ctl_os; }

  bool ok = (((m_txSeq + SEQ_MOD - 1 - ack_nack_seq) % SEQ_MOD) <= (SEQ_MOD / 2))
         && (((ack_nack_seq + SEQ_MOD - m_ackedSeq) % SEQ_MOD) < (SEQ_MOD / 2));
  if (!ok) { ++m_cnt.rx_ack_nack_seq_error; NS_LOG_LOGIC ("ACK/NACK seq out of range, ignore"); return; }
  // True dup ACK: confirms an already-acked seq. Safe because acked_seq is
  // initialized to all-1s (init_seq-1 on LLR_INIT, UEC Table 5-10), so the first
  // ACK(0) is never misread as a dup; a repeated ACK(seq==acked_seq) is skipped.
  if (ack_nack_seq == m_ackedSeq && !isNack) { return; }

  // Release frames <= ack_nack_seq from replay buffer (both ACK and NACK)
  while (!m_replayBuf.empty ())
    {
      uint32_t fs = m_replayBuf.front ().seq;
      if (fs == ack_nack_seq
          || ((fs + SEQ_MOD - m_ackedSeq) % SEQ_MOD) <= ((ack_nack_seq + SEQ_MOD - m_ackedSeq) % SEQ_MOD))
        {
          m_replayBuf.pop_front ();
        }
      else { break; }
    }
  m_ackedSeq = ack_nack_seq;
  // UEC Table 5-10: replay_ct is set to zero when an LLR_ACK is received (or in
  // the INIT state) — NOT on a NACK. Resetting on NACK let a NACK-driven replay
  // cadence keep replay_ct pinned near 0, so the replay-count FLUSH could only
  // be reached via the replay timer path.
  if (!isNack) { m_replayCt = 0; }

  if (m_replayBuf.empty ())
    {
      // UEC §5.1.5: only ADVANCE/REPLAY may return to ADVANCE. In INIT/FLUSH the
      // buffer is empty by construction; a late ACK/NACK arriving there (e.g.
      // the lost ACK that caused replay_ct exhaustion, or a peer's periodic
      // cumulative ACK during re-sync) must NOT lift the machine into ADVANCE —
      // that would skip the INIT re-sync (DESIGN §6.10: with re_init_on_discard
      // =0 the machine stays in FLUSH until management Start()s it) and
      // oscillate FLUSH<->ADVANCE with data never delivered.
      if (m_tx == TxState::ADVANCE || m_tx == TxState::REPLAY) { GoAdvance (); }
      return;
    }

  if (isNack)
    {
      if (m_tx == TxState::REPLAY) { return; }
      NS_LOG_LOGIC ("NACK seq=" << ack_nack_seq << " -> REPLAY");
      m_tx = TxState::REPLAY;
      RecomputeLlrWindow ();   // TX replay window opened (rule 9 gate on)
      DoReplay ();
    }
  else
    {
      // UEC §5.1.4 replay_timer rule 4: an LLR_ACK acknowledging buffered
      // frames resets the replay timer to zero; since outstanding_seq is still
      // non-zero (buffer not empty), it is re-activated. Without this reset the
      // timer free-runs through ACK activity and expires on the in-flight
      // window, spuriously replaying frames whose ACK is merely in transit
      // (pathological at rates where ACK RTT ~= replay_timer_max).
      if (m_replayTimer.IsRunning ()) { m_replayTimer.Cancel (); }
      m_replayTimer = ScheduleCtx (m_replayTimerMax, &SueLlrEngine::OnReplayTimer, this);
    }

  // Window space was freed while frames are still buffered: give the device a
  // chance to push the next frame now. Without this, TX stalls until the WHOLE
  // window drains (GoAdvance only fires on an empty buffer) — stop-and-go at
  // window granularity, or an indefinite stall if the last frame's ACK is lost
  // and the replay timer is not bound (S8-style configs). The device-side
  // handler just re-runs DequeueAndTransmit (CanTx re-checks the limits), so a
  // still-full window is a cheap no-op.
  if (!m_txOpportunityCb.IsNull ()) { m_txOpportunityCb (); }
}

void
SueLlrEngine::DoReplay ()
{
  // UEC Table 5-10: the pcs_lost_status_timer also runs in REPLAY.
  ArmPcsLostTimer ();
  // §5.1.4 data-age timer: FLUSH if the oldest replay frame is too old
  if (!m_replayBuf.empty ())
    {
      Time oldestAge = Simulator::Now () - m_replayBuf.front ().ts;
      if (oldestAge > m_dataAgeTimerMax)
        {
          NS_LOG_LOGIC ("data_age_timer expired (age " << oldestAge.GetNanoSeconds () << "ns) -> FLUSH");
          m_cnt.tx_discard += m_replayBuf.size ();
          m_replayBuf.clear ();
          m_replayCt = 0;
          FlushAndInit ();   // enter INIT, wait for INIT_ECHO (with timeout)
          return;
        }
    }
  // UEC Table 5-10: replay_ct_is_max = replay_ct >= replay_ct_max; a
  // replay_ct_max of all-ones (255) means replay_ct_is_max is never TRUE, so
  // the replay-count limit never triggers FLUSH (only data_age / pcs_lost can).
  // Checking before the increment keeps the observable number of replay rounds
  // before FLUSH equal to the UEC (replay_ct_max rounds, then FLUSH on the next
  // timer expiry).
  if (m_replayCtMax != 255 && m_replayCt >= m_replayCtMax)
    {
      NS_LOG_LOGIC ("FLUSH: replay_ct=" << m_replayCt << " >= max=" << m_replayCtMax
                    << ", discard " << m_replayBuf.size () << " frames");
      m_cnt.tx_discard += m_replayBuf.size ();
      m_replayBuf.clear ();
      m_replayCt = 0;
      FlushAndInit ();   // enter INIT, wait for INIT_ECHO (with timeout)
      return;
    }
  ++m_replayCt;
  NS_LOG_LOGIC ("DoReplay ct=" << m_replayCt << ": retransmit " << m_replayBuf.size () << " unacked");
  for (const auto &e : m_replayBuf)
    {
      if (!m_retransmitCb.IsNull ()) { m_retransmitCb (e.seq, e.p); }
      ++m_cnt.tx_replay_frames;   // debug observability: per-frame retransmit volume
    }
  if (m_replayTimer.IsRunning ()) { m_replayTimer.Cancel (); }
  m_replayTimer = ScheduleCtx (m_replayTimerMax, &SueLlrEngine::OnReplayTimer, this);
}

void
SueLlrEngine::OnReplayTimer ()
{
  if (!m_replayBuf.empty () && m_tx != TxState::FLUSH)
    {
      NS_LOG_LOGIC ("replay_timer expired -> DoReplay");
      m_tx = TxState::REPLAY;
      RecomputeLlrWindow ();   // TX replay window opened (rule 9 gate on)
      DoReplay ();
    }
}

void
SueLlrEngine::OnAckTimer ()
{
  // Self-rearm: while in SEND_ACKS the cadence keeps running (idle periods
  // repeat the last cumulative ACK; the sender dup-skips it — the chip's LLR
  // keeps sending ACKs on the ctlos_target_spacing cadence even when idle).
  if (m_ackPeriod > Seconds (0))
    {
      m_ackTimer = ScheduleCtx (m_ackPeriod, &SueLlrEngine::OnAckTimer, this);
    }
  // UEC §5.1.6: each period send ONE cumulative ACK covering everything
  // received since the last ACK (ack_nack_seq = next_rx_seq-1), unless the
  // ACK/NACK machine is in NACK_SENT (m_nackSent suppresses ACKs).
  if (m_nackSent) { return; }
  if (!m_ackNackSink.IsNull ())
    {
      ++m_cnt.tx_ack_ctl_os;
      m_ackNackSink ((m_nextRxSeq + SEQ_MOD - 1) % SEQ_MOD, false);
    }
}

// ---- RX ----

SueLlrEngine::RxResult
SueLlrEngine::RxFrame (uint32_t seq, int fcsStatus)
{
  seq = seq % SEQ_MOD;
  // UEC §5.1.4/§5.1.6: when llr_mode_local is OFF the RX LLR does not filter
  // LLR-eligible frames (pass through unfiltered; no seq tracking / NACK).
  if (!m_modeLocal) { return ACCEPT; }
  // UEC §5.1.7: expected_frame = next_rx_seq_vld && (frame_seq == next_rx_seq).
  // Until a valid LLR_INIT is received (vld FALSE) eligible frames are filtered
  // by the RX LLR — dropped without counting or NACK.
  if (!m_nextRxSeqVld) { return DROP; }
  if (fcsStatus == 0) { ++m_cnt.rx_ok; }
  else if (fcsStatus == 1) { ++m_cnt.rx_poisoned; }
  else { ++m_cnt.rx_bad; }

  if (seq == m_nextRxSeq)
    {
      if (fcsStatus == 0)
        {
          // rx_replay: the expected frame arrived while we were NACKing ->
          // this is a retransmission being accepted (UEC Table 5-13).
          if (m_nackSent) { ++m_cnt.rx_replay; }
          ++m_cnt.rx_expected_seq_good;
          // 2^20 seq 空间回绕:next_rx_seq 必须留在取模空间,裸递增会在
          // 第 2^20 帧后与 wire seq(0..2^20-1)永不相等 → 全部判 missing →
          // NACK/FLUSH 重同步(512B 长突发每回绕丢 ~3 帧,thpt 实测暴露)。
          m_nextRxSeq = (m_nextRxSeq + 1) % SEQ_MOD;
          m_nackSent = false;
          if (m_discardActive && m_nextRxSeq == (m_discardMaxSeq + 1) % SEQ_MOD)
            { m_discardActive = false; m_discardMaxSeq = 0; }
          // The expected frame arrived: the RX discard window closes.
          RecomputeLlrWindow ();
          // UEC §5.1.6: with ackPeriod > 0 the ACK is sent by the SEND_ACKS
          // cadence (armed on init_echo_sent), not per frame; per-frame ACK is
          // only the ackPeriod == 0 path.
          if (m_ackPeriod == Seconds (0) && !m_ackNackSink.IsNull ())
            {
              ++m_cnt.tx_ack_ctl_os;
              m_ackNackSink (seq, false);
            }
          return ACCEPT;
        }
      else if (fcsStatus == 1)
        {
          if (m_nackSent) { ++m_cnt.rx_replay; }
          ++m_cnt.rx_expected_seq_poisoned;
          m_nextRxSeq = (m_nextRxSeq + 1) % SEQ_MOD;   // 回绕同 GOOD 分支
          m_nackSent = false;
          RecomputeLlrWindow ();   // discard window closed
          if (m_ackPeriod == Seconds (0) && !m_ackNackSink.IsNull ())
            {
              ++m_cnt.tx_ack_ctl_os;
              m_ackNackSink (seq, false);
            }
          return ACCEPT_POISONED;
        }
      else
        {
          ++m_cnt.rx_expected_seq_bad;
          if (!m_nackSent)
            {
              uint32_t nackSeq = (m_nextRxSeq + SEQ_MOD - 1) % SEQ_MOD;
              if (!m_ackNackSink.IsNull ()) { ++m_cnt.tx_nack_ctl_os; m_ackNackSink (nackSeq, true); }
              m_nackSent = true;
              RecomputeLlrWindow ();   // discard window opened (rule 3 gate on)
            }
          return DROP;
        }
    }
  if (((seq + SEQ_MOD - m_nextRxSeq) % SEQ_MOD) > (SEQ_MOD / 2)) { ++m_cnt.rx_duplicate_seq; return DROP; }
  ++m_cnt.rx_missing_seq;
  // §12.48: first discard records unconditionally (m_discardMaxSeq's init 0
  // is a valid seq — comparing against it on the first hit is wrong and
  // would leave the clear condition unreachable → window stuck open).
  if (!m_discardActive)
    { m_discardMaxSeq = seq; m_discardActive = true; }
  else
    {
      const uint32_t ahead = (seq + SEQ_MOD - m_nextRxSeq) % SEQ_MOD;
      const uint32_t curAhead = (m_discardMaxSeq + SEQ_MOD - m_nextRxSeq) % SEQ_MOD;
      if (ahead > curAhead) { m_discardMaxSeq = seq; }
    }
  if (!m_nackSent)
    {
      uint32_t nackSeq = (m_nextRxSeq + SEQ_MOD - 1) % SEQ_MOD;
      if (!m_ackNackSink.IsNull ()) { ++m_cnt.tx_nack_ctl_os; m_ackNackSink (nackSeq, true); }
      m_nackSent = true;
      RecomputeLlrWindow ();   // discard window opened (rule 3 gate on)
    }
  return DROP;
}

} // namespace ns3

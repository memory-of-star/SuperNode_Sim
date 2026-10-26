/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * MTP detection helper (DESIGN.md §10.5).
 *
 * The fork's single-process multithreaded simulator exposes its facade through
 * the Simulator implementation; the sue module must stay free of any src/mtp
 * dependency, so MTP is detected by comparing the active implementation's
 * TypeId name instead of including mtp headers.
 *
 * When SueMtpActive() is true, node-owned timers must be armed from the owning
 * node's LP (the fork's ScheduleWithContext returns no cancellable EventId and
 * cross-LP mailbox events are fire-and-forget). The sue model guarantees that
 * residency by (a) routing the whole data plane over the channel's existing
 * ScheduleWithContext seam, and (b) deferring the two setup-time timer arms
 * (SueLlrEngine::Start -> m_initTimer, SueNetDevice::StartCcUpdateTimer ->
 * m_ccUpdateTimer) to a t==0 context event that Partition() re-homes onto the
 * node's LP before any thread starts.
 *
 * SueMtpResidencyCheck() (§10.5 驻留不变量, S5 收尾清单 ①) turns that
 * residency convention into an executable assertion at every ScheduleCtx /
 * ScheduleSelf arm site.
 */
#ifndef SUE_MTP_H
#define SUE_MTP_H

#include "ns3/assert.h"
#include "ns3/node.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/simulator-impl.h"

#include <string>

namespace ns3 {

/** True when the active Simulator implementation is the fork's
 *  ns3::MultithreadedSimulatorImpl (auto-partition, §10.5).
 *
 *  The implementation type is fixed for the whole run — it is installed once at
 *  setup (MtpInterface::Enable + Simulator::SetImplementation before any sue
 *  timer is armed; see sue-s8-mtp.cc) — so the TypeId comparison is cached in a
 *  function-local static (thread-safe magic static). */
inline bool
SueMtpActive ()
{
  static const bool active = Simulator::GetImplementation ()->GetInstanceTypeId ().GetName ()
                             == "ns3::MultithreadedSimulatorImpl";
  return active;
}

/** Current LP (LogicalProcess) id under the fork's MTP. SimulatorImpl::GetSystemId
 *  is pure virtual; MultithreadedSimulatorImpl returns the current thread's LP
 *  (MtpInterface::GetSystem()->GetSystemId()). Only meaningful when MTP is
 *  active; during Partition()'s re-home InvokeNow the current LP is temporarily
 *  switched to the node's owning LP (LogicalProcess::InvokeNow), so a nested
 *  arm there reports the owner too. */
inline uint32_t
SueMtpCurrentLp ()
{
  return Simulator::GetImplementation ()->GetSystemId ();
}

/** Fresh (uncached) check of the active implementation — the contract
 *  enforcement counterpart to SueMtpActive()'s cached magic static. */
inline bool
SueMtpImplInstalled ()
{
  return Simulator::GetImplementation ()->GetInstanceTypeId ().GetName ()
         == "ns3::MultithreadedSimulatorImpl";
}

/** Setup-contract assertion: the active implementation must not BE the MTP
 *  simulator while the cached SueMtpActive() reads false — that combination
 *  means MtpInterface::Enable ran AFTER the first sue MTP lookup, so every
 *  residency check silently short-circuits and node timers land on the public
 *  LP (Cancel can never find them; cross-LP errors are untraceable). Fail
 *  loudly instead of silently degrading. Call from Start()/config entry points. */
inline void
SueMtpContractCheck ()
{
  NS_ASSERT_MSG (!SueMtpImplInstalled () || SueMtpActive (),
                 "MTP was enabled after the first sue MTP lookup (SueMtpActive() cached false): "
                 "call MtpInterface::Enable BEFORE any sue config/network setup");
}

/** MTP residency assertion (§10.5 驻留不变量).
 *
 *  ScheduleCtx / ScheduleSelf route by LP residency: the caller must already be
 *  executing on the logical process that owns `nodeId`, so the plain
 *  Simulator::Schedule inside those templates falls into that node's timer heap
 *  — where Simulator::Cancel is later issued from the same LP and can find it.
 *  A caller on any other LP (e.g. a future public-LP send path like a bare
 *  SueLiteEndpoint driving SwitchSend without a node context) would land the
 *  timer on the wrong heap and Cancel would never fire.
 *
 *  This check aborts the moment an arm site runs off-owner under MTP; it is a
 *  no-op when MTP is inactive, so the single-threaded baseline is unchanged. */
inline void
SueMtpResidencyCheck (uint32_t nodeId)
{
  NS_ASSERT_MSG (!SueMtpActive () || SueMtpCurrentLp () == NodeList::GetNode (nodeId)->GetSystemId (),
                 "MTP residency violation: timer armed on LP " << SueMtpCurrentLp ()
                 << " but node " << nodeId << " is owned by LP "
                 << NodeList::GetNode (nodeId)->GetSystemId ()
                 << " (§10.5: 定时器 arm 必须在节点 LP 内执行)");
}

} // namespace ns3

#endif /* SUE_MTP_H */

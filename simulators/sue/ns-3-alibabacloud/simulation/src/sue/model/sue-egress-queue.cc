/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE egress queue implementation.
 */
#include "sue-egress-queue.h"
#include "sue-cbfc-engine.h"
#include "sue-pfc-engine.h"
#include "sue-fc-controller.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueEgressQueue");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueEgressQueue);

TypeId
SueEgressQueue::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueEgressQueue")
    .SetParent<Object> ()
    .AddConstructor<SueEgressQueue> ()
  ;
  return tid;
}

SueEgressQueue::SueEgressQueue () { NS_LOG_FUNCTION_NOARGS (); }

bool
SueEgressQueue::Enqueue (uint32_t qIndex, Ptr<Packet> p)
{
  if (qIndex >= qCnt) { return false; }
  m_queues[qIndex].push_back (p);
  return true;
}

Ptr<Packet>
SueEgressQueue::Dequeue (uint32_t qIndex)
{
  if (qIndex >= qCnt || m_queues[qIndex].empty ()) { return 0; }
  Ptr<Packet> p = m_queues[qIndex].front ();
  m_queues[qIndex].pop_front ();
  return p;
}

int
SueEgressQueue::GetNextQindex ()
{
  for (uint32_t i = 0; i < qCnt; ++i)
    {
      uint32_t q = (m_rrLast + i) % qCnt;
      if (!m_queues[q].empty ())
        {
          m_rrLast = (q + 1) % qCnt;
          return (int) q;
        }
    }
  return -1;
}

int
SueEgressQueue::GetNextSendableQindex (Ptr<SueCbfcEngine> cbfc)
{
  if (!cbfc) { return GetNextQindex (); }
  for (uint32_t i = 0; i < qCnt; ++i)
    {
      uint32_t q = (m_rrLast + i) % qCnt;
      if (m_queues[q].empty ()) { continue; }
      uint32_t sz = m_queues[q].front ()->GetSize ();
      if (cbfc->CanSend (q, sz))
        {
          m_rrLast = (q + 1) % qCnt;
          return (int) q;
        }
    }
  return -1;   // no VC sendable (empty or credit-exhausted)
}

int
SueEgressQueue::GetNextSendableQindexPfc (Ptr<SuePfcEngine> pfc)
{
  if (!pfc) { return GetNextQindex (); }
  for (uint32_t i = 0; i < qCnt; ++i)
    {
      uint32_t q = (m_rrLast + i) % qCnt;
      if (m_queues[q].empty ()) { continue; }
      if (pfc->CanSend (q))
        {
          m_rrLast = (q + 1) % qCnt;
          return (int) q;
        }
    }
  return -1;   // no VC sendable (empty or paused)
}

int
SueEgressQueue::GetNextSendableQindex (Ptr<SueFcController> fc)
{
  if (!fc || fc->GetMode () == SueFcController::OFF) { return GetNextQindex (); }
  if (fc->GetMode () == SueFcController::PFC) { return GetNextSendableQindexPfc (fc->GetPfc ()); }
  return GetNextSendableQindex (fc->GetCbfc ());   // CBFC (or degenerate engine-less)
}

uint32_t
SueEgressQueue::GetNBytes (uint32_t qIndex) const
{
  if (qIndex >= qCnt) { return 0; }
  uint32_t bytes = 0;
  for (const auto &p : m_queues[qIndex]) { bytes += p->GetSize (); }
  return bytes;
}

uint32_t
SueEgressQueue::GetFrontSize (uint32_t qIndex) const
{
  if (qIndex >= qCnt || m_queues[qIndex].empty ()) { return 0; }
  return m_queues[qIndex].front ()->GetSize ();
}

bool
SueEgressQueue::HasPackets () const
{
  for (uint32_t i = 0; i < qCnt; ++i) { if (!m_queues[i].empty ()) { return true; } }
  return false;
}

void
SueEgressQueue::SetVcLimitBytes (uint32_t vc, uint64_t bytes)
{
  if (vc < qCnt) { m_vcLimitBytes[vc] = bytes; }
}

bool
SueEgressQueue::CanAccept (uint32_t vc, uint32_t size) const
{
  if (vc >= qCnt) { return false; }
  uint64_t limit = m_vcLimitBytes[vc];
  if (limit == 0) { return true; }   // unbounded
  return (uint64_t) GetNBytes (vc) + (uint64_t) size <= limit;
}

} // namespace ns3

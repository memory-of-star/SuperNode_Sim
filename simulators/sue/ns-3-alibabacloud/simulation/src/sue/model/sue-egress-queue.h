/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE egress queue (DESIGN.md §6.7). 8-VC FIFO queues, weighted round-robin.
 * S2 wire-time step: no flow-control gating yet (CBFC credit gating added
 * in the CBFC step via GetNextSendableQindex(cbfc, llrBusy)).
 */
#ifndef SUE_EGRESS_QUEUE_H
#define SUE_EGRESS_QUEUE_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/packet.h"
#include <deque>
#include <cstdint>

namespace ns3 {

class SueCbfcEngine;
class SuePfcEngine;
class SueFcController;

class SueEgressQueue : public Object
{
public:
  static const uint32_t qCnt = 8;

  static TypeId GetTypeId (void);
  SueEgressQueue ();

  bool Enqueue (uint32_t qIndex, Ptr<Packet> p);
  Ptr<Packet> Dequeue (uint32_t qIndex);

  /** Round-robin over non-empty queues; returns qIndex or -1 if all empty. */
  int GetNextQindex ();

  /** RR over non-empty queues that also pass the CBFC credit gate
   *  (cbfc->CanSend(vc, frontSize)). If cbfc is null, behaves as GetNextQindex. */
  int GetNextSendableQindex (Ptr<SueCbfcEngine> cbfc);

  /** RR over non-empty queues that pass PFC gate (pfc->CanSend(vc)). */
  int GetNextSendableQindexPfc (Ptr<SuePfcEngine> pfc);

  /** RR over non-empty queues that pass the flow-control facade gate
   *  (fc->CanSend(vc, frontSize)). Dispatches to the CBFC/PFC overloads by
   *  mode; null/OFF behaves as GetNextQindex. */
  int GetNextSendableQindex (Ptr<SueFcController> fc);

  uint32_t GetNBytes (uint32_t qIndex) const;
  uint32_t GetFrontSize (uint32_t qIndex) const;   // size of head-of-line packet (0 if empty)
  bool HasPackets () const;

  /** Per-VC byte depth limit (0 = unbounded). The egress gate (§6.7 CBFC
   *  backpressure): the switch accepts into this queue only while
   *  CanAccept(vc, size) holds; DequeueAndTransmit always takes an already-
   *  admitted frame, so the depth can never exceed the limit. */
  void SetVcLimitBytes (uint32_t vc, uint64_t bytes);
  /** Depth admission check: limit==0 (unbounded) or current depth + size <= limit. */
  bool CanAccept (uint32_t vc, uint32_t size) const;

private:
  std::deque<Ptr<Packet>> m_queues[qCnt];
  uint64_t m_vcLimitBytes[qCnt]{};   // 0 = unbounded
  uint32_t m_rrLast{0};
};

} // namespace ns3

#endif /* SUE_EGRESS_QUEUE_H */

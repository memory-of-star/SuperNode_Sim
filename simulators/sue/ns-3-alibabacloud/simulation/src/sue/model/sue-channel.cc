/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE Scale-Up Ethernet - SueChannel implementation.
 * Mirrors QbbChannel::TransmitStart: static dispatch to &SueNetDevice::Receive.
 */
#include "sue-channel.h"
#include "sue-net-device.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include "ns3/trace-source-accessor.h"

NS_LOG_COMPONENT_DEFINE ("SueChannel");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueChannel);

TypeId
SueChannel::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueChannel")
    .SetParent<Channel> ()
    .AddConstructor<SueChannel> ()
    .AddAttribute ("Delay", "Transmission delay through the channel",
                   TimeValue (Seconds (0)),
                   MakeTimeAccessor (&SueChannel::m_delay),
                   MakeTimeChecker ())
    .AddTraceSource ("TxRxSue",
                     "Trace source indicating transmission of packet from the SueChannel.",
                     MakeTraceSourceAccessor (&SueChannel::m_txrxSue),
                     "ns3::SueChannel::Double")
  ;
  return tid;
}

SueChannel::SueChannel ()
  : PointToPointChannel ()
{
  NS_LOG_FUNCTION_NOARGS ();
  m_nDevices = 0;
}

void
SueChannel::Attach (Ptr<SueNetDevice> device)
{
  NS_LOG_FUNCTION (this << device);
  NS_ASSERT_MSG (m_nDevices < N_DEVICES, "Only two devices permitted");
  NS_ASSERT (device != 0);

  m_link[m_nDevices++].m_src = device;
  if (m_nDevices == N_DEVICES)
    {
      m_link[0].m_dst = m_link[1].m_src;
      m_link[1].m_dst = m_link[0].m_src;
      m_link[0].m_state = IDLE;
      m_link[1].m_state = IDLE;
    }
}

bool
SueChannel::TransmitStart (Ptr<Packet> p, Ptr<SueNetDevice> src, Time txTime)
{
  NS_LOG_FUNCTION (this << p << src);

  NS_ASSERT (m_link[0].m_state != INITIALIZING);
  NS_ASSERT (m_link[1].m_state != INITIALIZING);

  uint32_t wire = src == m_link[0].m_src ? 0 : 1;

  // Static dispatch to SueNetDevice::Receive (NOT base PointToPointNetDevice::Receive).
  Simulator::ScheduleWithContext (m_link[wire].m_dst->GetNode ()->GetId (),
                                  txTime + m_delay, &SueNetDevice::Receive,
                                  m_link[wire].m_dst, p);

  m_txrxSue (p, src, m_link[wire].m_dst, txTime, txTime + m_delay);
  return true;
}

size_t
SueChannel::GetNDevices (void) const
{
  NS_LOG_FUNCTION_NOARGS ();
  return m_nDevices;
}

Ptr<SueNetDevice>
SueChannel::GetSueDevice (uint32_t i) const
{
  NS_LOG_FUNCTION_NOARGS ();
  NS_ASSERT (i < 2);
  return m_link[i].m_src;
}

Ptr<NetDevice>
SueChannel::GetDevice (std::size_t i) const
{
  NS_LOG_FUNCTION_NOARGS ();
  return GetSueDevice (i);
}

Time
SueChannel::GetDelay (void) const
{
  return m_delay;
}

Ptr<SueNetDevice>
SueChannel::GetSource (uint32_t i) const
{
  return m_link[i].m_src;
}

Ptr<SueNetDevice>
SueChannel::GetDestination (uint32_t i) const
{
  return m_link[i].m_dst;
}

bool
SueChannel::IsInitialized (void) const
{
  NS_ASSERT (m_link[0].m_state != INITIALIZING);
  NS_ASSERT (m_link[1].m_state != INITIALIZING);
  return true;
}

void
SueChannel::DoDispose (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  // Break the SueChannel<->SueNetDevice reference cycle.
  for (int i = 0; i < N_DEVICES; ++i)
    {
      m_link[i].m_src = 0;
      m_link[i].m_dst = 0;
    }
  PointToPointChannel::DoDispose (); // inherited -> Object::DoDispose
}

} // namespace ns3

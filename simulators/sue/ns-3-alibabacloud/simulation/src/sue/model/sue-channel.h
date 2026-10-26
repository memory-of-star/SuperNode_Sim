/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE Scale-Up Ethernet - SueChannel
 *
 * Mirrors QbbChannel: a dedicated channel that statically dispatches to
 * &SueNetDevice::Receive (the base PointToPointChannel dispatches to the
 * non-virtual &PointToPointNetDevice::Receive, so a subclass Receive would
 * never be called -- see DESIGN.md §6.1/§6.13).
 *
 * IMPORTANT (name-hiding, not override): TransmitStart/Attach take
 * Ptr<SueNetDevice>, whereas the base takes Ptr<PointToPointNetDevice>.
 * Callers MUST hold a Ptr<SueChannel> static type and call
 *   m_channel->TransmitStart(p, this, txTime)
 * i.e. SueNetDevice::TransmitStart must use its m_channel (Ptr<SueChannel>),
 * NOT an upcast to PointToPointChannel. Upcasting would resolve to the base
 * TransmitStart and schedule &PointToPointNetDevice::Receive, silently
 * bypassing the LLR/FC RX path entirely.
 */
#ifndef SUE_CHANNEL_H
#define SUE_CHANNEL_H

#include "ns3/point-to-point-channel.h"
#include "ns3/ptr.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

namespace ns3 {

class SueNetDevice;
class Packet;

/**
 * \ingroup sue
 * \brief Simple point-to-point channel for SUE scale-up links.
 *
 * Two-device channel (same model as QbbChannel). TransmitStart schedules a
 * call to SueNetDevice::Receive on the destination device after txTime+m_delay.
 */
class SueChannel : public PointToPointChannel
{
public:
  static TypeId GetTypeId (void);
  SueChannel ();

  void Attach (Ptr<SueNetDevice> device);

  /// \brief Transmit a packet; statically dispatches to &SueNetDevice::Receive.
  /// \note Name-hides (not overrides) the base TransmitStart -- see file header.
  virtual bool TransmitStart (Ptr<Packet> p, Ptr<SueNetDevice> src, Time txTime);

  virtual size_t GetNDevices (void) const;
  Ptr<SueNetDevice> GetSueDevice (uint32_t i) const;
  virtual Ptr<NetDevice> GetDevice (std::size_t i) const;
  Time GetDelay (void) const;

protected:
  /// Break the SueChannel<->SueNetDevice reference cycle (null link src/dst).
  virtual void DoDispose (void);
  bool IsInitialized (void) const;
  Ptr<SueNetDevice> GetSource (uint32_t i) const;
  Ptr<SueNetDevice> GetDestination (uint32_t i) const;

private:
  static const int N_DEVICES = 2;
  Time          m_delay;
  int32_t       m_nDevices;

  TracedCallback<Ptr<const Packet>, // Packet being transmitted
                 Ptr<NetDevice>,    // Transmitting NetDevice
                 Ptr<NetDevice>,    // Receiving NetDevice
                 Time,              // Amount of time to transmit the pkt
                 Time               // Last bit receive time (relative to now)
                 > m_txrxSue;

  enum WireState
  {
    INITIALIZING,
    IDLE,
    TRANSMITTING,
    PROPAGATING
  };

  class Link
  {
  public:
    Link () : m_state (INITIALIZING), m_src (0), m_dst (0) {}
    WireState            m_state;
    Ptr<SueNetDevice>   m_src;
    Ptr<SueNetDevice>   m_dst;
  };

  Link m_link[N_DEVICES];
};

} // namespace ns3

#endif /* SUE_CHANNEL_H */

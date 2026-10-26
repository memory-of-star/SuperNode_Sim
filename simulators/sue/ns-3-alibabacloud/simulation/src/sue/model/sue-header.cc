/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * SUE AFH Gen1 L2 frame header implementation.
 */
#include "sue-header.h"
#include "ns3/sue-config.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

NS_LOG_COMPONENT_DEFINE ("SueHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (SueHeader);

TypeId
SueHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SueHeader")
    .SetParent<Header> ()
    .AddConstructor<SueHeader> ()
  ;
  return tid;
}

TypeId
SueHeader::GetInstanceTypeId () const
{
  return GetTypeId ();
}

SueHeader::SueHeader ()
  : m_etherType (SueConfig::EthType0 ())
{
}

uint32_t
SueHeader::GetSerializedSize () const
{
  uint32_t sz = 6 + 6;                       // MAC DA + MAC SA
  if (m_vlan) { sz += 4; }                   // TPID + PCP/DEI/VID
  sz += 2;                                   // EtherType
  if (m_shimPresent) { sz += m_shim.Size (); }
  return sz;
}

void
SueHeader::Serialize (Buffer::Iterator start) const
{
  uint8_t mac[6];
  m_dst.CopyTo (mac);
  for (int i = 0; i < 6; ++i) { start.WriteU8 (mac[i]); }
  m_src.CopyTo (mac);
  for (int i = 0; i < 6; ++i) { start.WriteU8 (mac[i]); }

  if (m_vlan)
    {
      start.WriteHtonU16 (SUE_VLAN_TPID);
      start.WriteU8 ((uint8_t) (((m_pcp & 0x07) << 5) | ((m_dei & 0x01) << 4) | ((m_vid >> 8) & 0x0F)));
      start.WriteU8 ((uint8_t) (m_vid & 0xFF));
    }
  start.WriteHtonU16 (m_etherType);

  if (m_shimPresent)
    {
      uint8_t buf[3];
      m_shim.Pack (buf);
      for (uint8_t i = 0; i < m_shim.Size (); ++i) { start.WriteU8 (buf[i]); }
    }
}

uint32_t
SueHeader::Deserialize (Buffer::Iterator start)
{
  uint8_t mac[6];
  for (int i = 0; i < 6; ++i) { mac[i] = start.ReadU8 (); }
  m_dst.CopyFrom (mac);
  for (int i = 0; i < 6; ++i) { mac[i] = start.ReadU8 (); }
  m_src.CopyFrom (mac);

  uint16_t et = start.ReadNtohU16 ();
  if (et == SUE_VLAN_TPID)
    {
      m_vlan = true;
      uint8_t t0 = start.ReadU8 ();
      uint8_t t1 = start.ReadU8 ();
      m_pcp = (t0 >> 5) & 0x07;
      m_dei = (t0 >> 4) & 0x01;
      m_vid = ((uint16_t) (t0 & 0x0F) << 8) | t1;
      et = start.ReadNtohU16 ();
    }
  else
    {
      m_vlan = false;
    }
  m_etherType = et;

  // Shim presence is a per-etype CONFIG decision (SueConfig::HasShim), not
  // which EtherType value it is; both SUE EtherTypes parse identically here.
  if (SueConfig::HasShim (et))
    {
      m_shimPresent = true;
      uint8_t b0 = start.ReadU8 ();
      uint8_t b1 = start.ReadU8 ();
      uint8_t buf[3] = { b0, b1, 0 };
      bool fbit = (b0 >> 6) & 0x01;
      if (fbit) { buf[2] = start.ReadU8 (); }
      m_shim.Unpack (buf);
    }
  else
    {
      m_shimPresent = false;
    }
  return GetSerializedSize ();
}

void
SueHeader::Print (std::ostream &os) const
{
  os << "sue[dst=" << m_dst << " src=" << m_src;
  if (m_vlan) { os << " vlan(pcp=" << (uint32_t) m_pcp << ",vid=" << m_vid << ")"; }
  os << " et=0x" << std::hex << m_etherType << std::dec;
  if (m_shimPresent)
    {
      os << " shim(ttl=" << (uint32_t) m_shim.ttl << ",dscp=" << (uint32_t) m_shim.dscp
         << ",f=" << m_shim.f << ")";
    }
  os << "]";
}

// --- construction ---
void SueHeader::SetDst (Mac48Address d) { m_dst = d; }
void SueHeader::SetSrc (Mac48Address s) { m_src = s; }
void SueHeader::SetVlan (bool on, uint8_t pcp, uint8_t dei, uint16_t vid)
{ m_vlan = on; m_pcp = pcp; m_dei = dei; m_vid = vid; }
void SueHeader::SetEtherType (uint16_t et) { m_etherType = et; }
void SueHeader::SetShim (const SueShimHeader &sh)
{ m_shim = sh; m_shimPresent = true; }   // EtherType unchanged; caller picks one configured with a shim
void SueHeader::ClearShim () { m_shimPresent = false; }   // EtherType unchanged; caller picks one configured without a shim

// --- accessors ---
Mac48Address SueHeader::GetDst () const { return m_dst; }
Mac48Address SueHeader::GetSrc () const { return m_src; }
bool SueHeader::HasVlan () const { return m_vlan; }
uint8_t SueHeader::GetPcp () const { return m_pcp; }
uint8_t SueHeader::GetDei () const { return m_dei; }
uint16_t SueHeader::GetVid () const { return m_vid; }
uint16_t SueHeader::GetEtherType () const { return m_etherType; }
bool SueHeader::HasShim () const { return m_shimPresent; }
const SueShimHeader &SueHeader::GetShim () const { return m_shim; }
SueShimHeader &SueHeader::GetShimRef () { return m_shim; }

uint32_t
SueHeader::GetXpuId () const
{
  uint8_t mac[6];
  m_dst.CopyTo (mac);
  // high 10 bits = byte0 (8) | byte1 high 2 bits
  return ((uint32_t) mac[0] << 2) | ((mac[1] >> 6) & 0x03);
}

uint8_t SueHeader::GetTtl () const { return m_shimPresent ? m_shim.ttl : 0; }
void SueHeader::DecrementTtl () { if (m_shimPresent && m_shim.ttl > 0) { m_shim.ttl--; } }

Mac48Address
SueMacFromXpuId (uint32_t xpuId)
{
  uint8_t b[6] = { 0, 0, 0, 0, 0, 0 };
  b[0] = (xpuId >> 2) & 0xFF;
  b[1] = (xpuId & 0x03) << 6;
  Mac48Address m;
  m.CopyFrom (b);
  return m;
}

uint32_t
SueXpuFromMac (Mac48Address m)
{
  uint8_t mac[6];
  m.CopyTo (mac);
  // high 10 bits = byte0 (8) | byte1 high 2 bits (same decode as GetXpuId)
  return ((uint32_t) mac[0] << 2) | ((mac[1] >> 6) & 0x03);
}

} // namespace ns3

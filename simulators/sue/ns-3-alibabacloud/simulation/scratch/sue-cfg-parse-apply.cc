/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * S6 config-system check (DESIGN.md §9): SueReadConf (comments/hex/unknown
 * skip), runtime EtherType override, ApplySueConfig facade + credit-limit
 * wiring. Local fast check (scratch/ is gitignored). Run:
 *   cmake --build build -j8 --target scratch_sue-s6-config
 *   ./build/scratch/ns3.36.1-sue-cfg-parse-apply-default
 */
#include "ns3/sue-config.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-pfc-engine.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/sue-header.h"
#include "ns3/config.h"
#include "ns3/global-value.h"
#include "ns3/uinteger.h"
#include "ns3/string.h"
#include "ns3/packet.h"
#include "ns3/mac48-address.h"
#include <fstream>
#include <iostream>

using namespace ns3;

static bool g_ok = true;
// GlobalValue registry lookup (GetValueByName returns void; detect unknown
// keys by scanning the registered names).
static bool
HasGlobal (const std::string &name)
{
  for (GlobalValue::Iterator i = GlobalValue::Begin (); i != GlobalValue::End (); ++i)
    { if ((*i)->GetName () == name) { return true; } }
  return false;
}
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    bool c = (cond);                                                           \
    std::cout << "  [" << (c ? "ok" : "FAIL") << "] " << msg << "\n";          \
    g_ok = g_ok && c;                                                          \
  } while (0)


static SueHeader
RoundTrip (const SueHeader &h)
{
  Ptr<Packet> p = Create<Packet> ();
  p->AddHeader (h);
  SueHeader h2;
  p->PeekHeader (h2);
  return h2;
}

int
main ()
{
  const char *confPath = "/tmp/sue-s6-test.conf";
  std::ofstream conf (confPath);
  conf << "# SUE config test (comments + blank lines skipped)\n"
       << "\n"
       << "ENABLE_SCALEUP 1\n"
       << "LLR_ENABLE 0          # legacy key removed in the UEC-aligned config; skipped\n"
       << "LLR_MODE_LOCAL 0\n"
       << "LLR_MODE_REMOTE 0\n"
       << "LLR_INIT_BEHAVIOR DISCARD\n"
       << "LLR_FLUSH_BEHAVIOR BEST_EFFORT\n"
       << "LLR_RE_INIT_ON_DISCARD 0\n"
       << "SUE_ETH_TYPE_0 0x1234\n"
       << "SUE_ETH_TYPE_0_HAS_SHIM 0\n"
       << "SUE_ETH_TYPE_1 0x1235\n"
       << "SUE_ETH_TYPE_1_HAS_SHIM 1\n"
       << "FC_MODE CBFC\n"
       << "CBFC_CREDIT_SIZE 256\n"
       << "CBFC_VC_CREDIT_LIMIT 0:1,1:2\n"
       << "CBFC_CC_MSG_TIMER_US 0\n"
       << "LLR_REPLAY_CT_MAX 9\n"
       << "PFC_PAUSE_QUANTA 0x1000\n"
       << "UNKNOWN_KEY 42  # unknown keys are skipped\n";
  conf.close ();

  std::cout << "[A] SueReadConf parsing\n";
  UintegerValue uv; StringValue sv;
  CHECK (SueReadConf (confPath), "SueReadConf returns true");
  GlobalValue::GetValueByName ("ENABLE_SCALEUP", uv);
  CHECK (uv.Get () == 1, "ENABLE_SCALEUP = 1 (hex/dec ok)");
  GlobalValue::GetValueByName ("LLR_REPLAY_CT_MAX", uv);
  CHECK (uv.Get () == 9, "LLR_REPLAY_CT_MAX = 9");
  GlobalValue::GetValueByName ("PFC_PAUSE_QUANTA", uv);
  CHECK (uv.Get () == 0x1000, "PFC_PAUSE_QUANTA = 0x1000");
  GlobalValue::GetValueByName ("FC_MODE", sv);
  CHECK (sv.Get () == "CBFC", "FC_MODE = CBFC");
  // UEC §5.1.4: LLR_ENABLE removed (legacy conf lines silently skipped); the
  // two-phase modes + three behaviors + re_init_on_discard are the config keys.
  CHECK (!HasGlobal ("LLR_ENABLE"), "LLR_ENABLE removed (legacy line skipped)");
  CHECK (HasGlobal ("LLR_MODE_LOCAL") && HasGlobal ("LLR_MODE_REMOTE"),
         "LLR_MODE_LOCAL/LLR_MODE_REMOTE registered");
  GlobalValue::GetValueByName ("LLR_MODE_LOCAL", uv);
  CHECK (uv.Get () == 0, "LLR_MODE_LOCAL = 0 (parsed)");
  GlobalValue::GetValueByName ("LLR_MODE_REMOTE", uv);
  CHECK (uv.Get () == 0, "LLR_MODE_REMOTE = 0 (parsed)");
  GlobalValue::GetValueByName ("LLR_INIT_BEHAVIOR", sv);
  CHECK (sv.Get () == "DISCARD", "LLR_INIT_BEHAVIOR = DISCARD (parsed)");
  GlobalValue::GetValueByName ("LLR_FLUSH_BEHAVIOR", sv);
  CHECK (sv.Get () == "BEST_EFFORT", "LLR_FLUSH_BEHAVIOR = BEST_EFFORT (parsed)");
  GlobalValue::GetValueByName ("LLR_RE_INIT_ON_DISCARD", uv);
  CHECK (uv.Get () == 0, "LLR_RE_INIT_ON_DISCARD = 0 (parsed)");
  CHECK (!HasGlobal ("UNKNOWN_KEY"), "UNKNOWN_KEY not registered (skipped)");
  CHECK (!SueReadConf ("/nonexistent/conf"), "SueReadConf false on unopenable file");

  std::cout << "[B] Runtime EtherType override (per-etype shim config)\n";
  {
    // Two symmetric SUE EtherTypes; shim presence is the per-etype *HAS_SHIM
    // config, not a property of the EtherType value (§5.2/§9).
    CHECK (SueConfig::HasShim (0x1234) == false, "eth0 0x1234 configured without shim");
    CHECK (SueConfig::HasShim (0x1235) == true,  "eth1 0x1235 configured with shim");
    CHECK (SueConfig::HasShim (0x9999) == false, "unconfigured value has no shim");
    SueHeader h;
    CHECK (h.GetEtherType () == 0x1234, "default EtherType = SUE_ETH_TYPE_0 0x1234 (got 0x"
            << std::hex << h.GetEtherType () << std::dec << ")");
    // shim frame: pick the with-shim EtherType explicitly (SetShim no longer
    // chooses the EtherType)
    SueHeader s;
    s.SetDst (SueMacFromXpuId (0x2)); s.SetEtherType (0x1235);
    SueShimHeader sh; sh.ttl = 5; sh.f = false;
    s.SetShim (sh);
    CHECK (s.GetEtherType () == 0x1235, "SetShim leaves EtherType unchanged");
    SueHeader r = RoundTrip (s);
    CHECK (r.HasShim () && r.GetEtherType () == 0x1235, "shim round-trips via eth1 0x1235");
    // no-shim frame: default EtherType 0x1234 parses without a shim
    SueHeader n; n.SetDst (SueMacFromXpuId (0x3));
    SueHeader rn = RoundTrip (n);
    CHECK (!rn.HasShim () && rn.GetEtherType () == 0x1234, "no-shim round-trips via eth0 0x1234");
    // ClearShim leaves EtherType unchanged too
    s.ClearShim ();
    CHECK (s.GetEtherType () == 0x1235, "ClearShim leaves EtherType unchanged");
  }

  std::cout << "[C] ApplySueConfig: CBFC facade + credit limit\n";
  {
    Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
    ApplySueConfig (dev);
    CHECK (dev->GetFcController () != 0, "FC_MODE=CBFC built the facade");
    CHECK (dev->GetCbfcEngine () != 0, "CBFC engine wired");
    CHECK (dev->GetPfcEngine () == 0, "no PFC engine in CBFC mode");
    Ptr<SueCbfcEngine> cbfc = dev->GetCbfcEngine ();
    // limits are in credit units: 0:1 -> 1 credit, 1:2 -> 2 credits.
    // a 400B frame needs ceil(400/256)=2 credits.
    CHECK (!cbfc->CanSend (0, 400), "VC0 (limit 1 credit) blocks 400B");
    CHECK (cbfc->CanSend (1, 400), "VC1 (limit 2 credits) admits 400B");
  }

  std::cout << "[D] ApplySueConfig: PFC facade via FC_MODE switch\n";
  {
    Config::SetGlobal ("FC_MODE", StringValue ("PFC"));
    Config::SetGlobal ("CBFC_CC_MSG_TIMER_US", UintegerValue (0));
    Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
    ApplySueConfig (dev);
    CHECK (dev->GetFcController () != 0, "FC_MODE=PFC built the facade");
    CHECK (dev->GetPfcEngine () != 0, "PFC engine wired");
    CHECK (dev->GetCbfcEngine () == 0, "no CBFC engine in PFC mode");
  }

  std::cout << "[E] ApplySueConfig: UEC §5.1.4 LLR modes/behaviors onto the engine\n";
  {
    Ptr<SueNetDevice> dev = CreateObject<SueNetDevice> ();
    Ptr<SueLlrEngine> llr = CreateObject<SueLlrEngine> ();
    dev->SetLlrEngine (llr);
    ApplySueConfig (dev);
    CHECK (dev->GetLlrEngine () == llr, "LLR engine wired to device");
    CHECK (llr->GetModeLocal () == false, "LLR_MODE_LOCAL=0 applied -> mode_local OFF");
    CHECK (llr->GetModeRemote () == false, "LLR_MODE_REMOTE=0 applied -> mode_remote OFF");
    CHECK (llr->GetInitBehavior () == SueLlrEngine::TxBehavior::DISCARD,
           "LLR_INIT_BEHAVIOR=DISCARD applied");
    CHECK (llr->GetFlushBehavior () == SueLlrEngine::TxBehavior::BEST_EFFORT,
           "LLR_FLUSH_BEHAVIOR=BEST_EFFORT applied");
    CHECK (llr->GetTxBehavior () == SueLlrEngine::TxBehavior::BEST_EFFORT,
           "mode_remote OFF -> GetTxBehavior=BEST_EFFORT");
  }

  std::cout << (g_ok ? "CFG PARSE-APPLY PASS" : "CFG PARSE-APPLY FAIL") << std::endl;
  return g_ok ? 0 : 1;
}

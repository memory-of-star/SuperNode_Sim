// SUE workload adapter. Protocol engines remain in src/sue, unchanged.
// Immutable DAG; TX/dependency state belongs to source LP, RX state to destination LP.
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/sue-entry.h"
#include "ns3/sue-config.h"
#include "ns3/sue-header.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-frame-bad-tag.h"
#include "ns3/sue-error-model.h"
#include "ns3/mtp-interface.h"
#include "ns3/multithreaded-simulator-impl.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <stdexcept>
using namespace ns3;

// Simulation metadata only: no added application bytes on wire.
class TaskTag : public Tag {
public:
  uint32_t id=0, fragment=0, bytes=0;
  static TypeId GetTypeId() { static TypeId t=TypeId("ns3::SueHarnessTaskTag").SetParent<Tag>().AddConstructor<TaskTag>(); return t; }
  TypeId GetInstanceTypeId() const override { return GetTypeId(); }
  uint32_t GetSerializedSize() const override { return 12; }
  void Serialize(TagBuffer b) const override { b.WriteU32(id); b.WriteU32(fragment); b.WriteU32(bytes); }
  void Deserialize(TagBuffer b) override { id=b.ReadU32(); fragment=b.ReadU32(); bytes=b.ReadU32(); }
  void Print(std::ostream& o) const override { o << id << ':' << fragment; }
};
struct Task { uint32_t src,dst,vc; uint64_t bytes,delay; std::vector<uint32_t> deps,next; };
struct Tx { uint32_t remaining=0,fragment=0; uint64_t sent=0,blocked=0; int64_t ready=-1,start=-1,lastEnqueue=-1; };
struct Rx { uint64_t bytes=0,duplicates=0,invalid=0; int64_t end=-1; std::set<uint32_t> seen; };
static std::vector<Task> tasks;
static std::vector<Tx> tx;
static std::vector<Rx> rx;
static uint64_t visibility=100, interval=200;
static uint32_t payload=2048;
static Ptr<SueNetDevice> Device(uint32_t host) { return SueGetDevice(SueGetHostNodeId(host),0); }
static void Begin(uint32_t id);
static void Dependency(uint32_t id) {
  auto& s=tx.at(id);
  NS_ABORT_MSG_IF(s.remaining==0,"duplicate dependency notification");
  if (--s.remaining==0) {
    s.ready=Simulator::Now().GetPicoSeconds();
    Simulator::Schedule(NanoSeconds(tasks[id].delay), &Begin, id);
  }
}
static void Complete(uint32_t id) {
  rx[id].end=Simulator::Now().GetPicoSeconds();
  for (auto child:tasks[id].next)
    Simulator::ScheduleWithContext(SueGetHostNodeId(tasks[child].src), NanoSeconds(visibility), &Dependency, child);
}
static void Pump(uint32_t id) {
  auto& t=tasks[id]; auto& s=tx[id];
  uint32_t n=static_cast<uint32_t>(std::min<uint64_t>(payload,t.bytes-s.sent));
  Ptr<Packet> p=Create<Packet>(n);
  SueHeader h; h.SetSrc(SueMacFromXpuId(t.src)); h.SetDst(SueMacFromXpuId(t.dst));
  h.SetEtherType(SueConfig::EthType0()); h.SetVlan(true,t.vc,false,0);
  p->AddHeader(h);
  auto dev=Device(t.src);
  if (!dev->EgressCanAccept(t.vc,p->GetSize())) { ++s.blocked; Simulator::Schedule(NanoSeconds(interval),&Pump,id); return; }
  TaskTag tag; tag.id=id; tag.fragment=s.fragment++; tag.bytes=n; p->AddPacketTag(tag);
  dev->SwitchSend(t.vc,p); s.sent+=n; s.lastEnqueue=Simulator::Now().GetPicoSeconds();
  if (s.sent<t.bytes) Simulator::Schedule(NanoSeconds(interval),&Pump,id);
}
static void Begin(uint32_t id) {
  tx[id].start=Simulator::Now().GetPicoSeconds();
  if (tx[id].ready<0) tx[id].ready=tx[id].start-static_cast<int64_t>(tasks[id].delay)*1000;
  if (tasks[id].bytes==0) { Complete(id); return; }
  Pump(id);
}
static void Receive(Ptr<SueNetDevice> dev, Ptr<Packet> p, SueHeader h) {
  TaskTag tag;
  NS_ABORT_MSG_IF(!p->PeekPacketTag(tag) || tag.id>=tasks.size(),"missing/invalid task tag");
  auto& t=tasks[tag.id]; auto& r=rx[tag.id];
  SueFrameBadTag bad;
  if (p->PeekPacketTag(bad) || dev->GetNode()->GetId()!=SueGetHostNodeId(t.dst) || h.GetXpuId()!=t.dst ||
      h.GetSrc()!=SueMacFromXpuId(t.src) || tag.bytes!=p->GetSize()-h.GetSerializedSize()) { ++r.invalid; return; }
  if (!r.seen.insert(tag.fragment).second) { ++r.duplicates; return; }
  r.bytes+=tag.bytes;
  NS_ABORT_MSG_IF(r.bytes>t.bytes,"payload overcount");
  if (r.bytes==t.bytes) Complete(tag.id);
}
int main(int argc,char** argv) {
  std::string caseDir, out;
  uint32_t threads=1, seed=1, badEvery=0;
  uint64_t stopNs=2000000, queueBytes=65536;
  CommandLine cmd;
  cmd.AddValue("case","Generated case directory",caseDir); cmd.AddValue("out","Fresh output directory",out);
  cmd.AddValue("threads","Simulation threads",threads); cmd.AddValue("seed","Seed",seed);
  cmd.AddValue("payload","Payload bytes per frame",payload); cmd.AddValue("intervalNs","Per-task injection/poll interval",interval);
  cmd.AddValue("visibilityNs","Dependency completion notification delay",visibility);
  cmd.AddValue("stopNs","Absolute simulation deadline",stopNs); cmd.AddValue("queueBytes","Host per-VC egress capacity",queueBytes);
  cmd.AddValue("badEvery","Corrupt every Nth data frame at host0 peer receive port",badEvery);
  cmd.Parse(argc,argv);
  try {
    if(caseDir.empty()||out.empty()||!threads||!payload||!interval||!visibility) throw std::runtime_error("invalid arguments");
    Time::SetResolution(Time::PS); RngSeedManager::SetSeed(seed);
    std::ifstream f(caseDir+"/tasks.tsv"); if(!f) throw std::runtime_error("cannot open tasks.tsv");
    std::string line;
    while(std::getline(f,line)) {
      if(line.empty()||line[0]=='#') continue;
      std::istringstream ss(line); uint32_t id; Task t; std::string ds;
      if(!(ss>>id>>t.src>>t.dst>>t.bytes>>t.delay>>t.vc>>ds)||id!=tasks.size()||t.vc>7) throw std::runtime_error("invalid task row");
      if(ds!="-") { std::istringstream d(ds); std::string part; while(std::getline(d,part,',')) t.deps.push_back(std::stoul(part)); }
      tasks.push_back(t);
    }
    if(tasks.empty()) throw std::runtime_error("empty workload");
    tx.resize(tasks.size()); rx.resize(tasks.size());
    for(uint32_t i=0;i<tasks.size();++i) {
      tx[i].remaining=tasks[i].deps.size();
      for(auto d:tasks[i].deps) { if(d>=tasks.size()||d==i) throw std::runtime_error("invalid dependency"); tasks[d].next.push_back(i); }
    }
    if(threads>1) { MtpInterface::Enable(threads); Simulator::SetImplementation(CreateObject<MultithreadedSimulatorImpl>()); }
    SueEntryDispatch(caseDir+"/topology.txt",caseDir+"/sue.conf",nullptr);
    for(auto& t:tasks) if(t.src>=SueGetNHosts()||t.dst>=SueGetNHosts()||(t.bytes>0&&t.src==t.dst)||(t.bytes==0&&t.src!=t.dst)) throw std::runtime_error("invalid task endpoints");
    for(uint32_t h=0;h<SueGetNHosts();++h) {
      auto d=Device(h); d->SetReceiveUpcall(MakeCallback(&Receive));
      for(uint32_t vc=0;vc<8;++vc) d->SetEgressQueueVcLimit(vc,queueBytes);
    }
    if(badEvery) {
      auto ch=Device(0)->GetChannel();
      for(uint32_t j=0;j<ch->GetNDevices();++j) if(ch->GetDevice(j)!=Device(0)) {
        auto e=CreateObject<SueErrorModel>(); e->SetBadEvery(badEvery);
        DynamicCast<SueNetDevice>(ch->GetDevice(j))->SetErrorModel(e);
      }
    }
    for(uint32_t i=0;i<tasks.size();++i) if(tasks[i].deps.empty())
      Simulator::ScheduleWithContext(SueGetHostNodeId(tasks[i].src),NanoSeconds(tasks[i].delay),&Begin,i);
    Simulator::Stop(NanoSeconds(stopNs)); Simulator::Run();
    std::ofstream stats(out+"/task_statistics.csv");
    stats<<"task_id,src,dst,bytes,received_bytes,ready_ps,start_ps,last_enqueue_ps,end_ps,queue_blocked_polls,duplicates,invalid_frames,status\n";
    bool ok=true;
    for(uint32_t i=0;i<tasks.size();++i) {
      auto& t=tasks[i]; auto& s=tx[i]; auto& r=rx[i];
      bool done=r.end>=0&&r.bytes==t.bytes&&r.invalid==0;
      for(auto d:t.deps) if(rx[d].end<0 || s.start<rx[d].end+static_cast<int64_t>(visibility+t.delay)*1000) done=false;
      ok=ok&&done;
      stats<<i<<','<<t.src<<','<<t.dst<<','<<t.bytes<<','<<r.bytes<<','<<s.ready<<','<<s.start<<','<<s.lastEnqueue<<','<<r.end<<','<<s.blocked<<','<<r.duplicates<<','<<r.invalid<<','<<(done?"complete":"incomplete")<<'\n';
    }
    std::ofstream ports(out+"/port_statistics.csv");
    ports<<"node,port,replay_sessions,replay_frames,nack_tx,discard,credit_exhausted\n";
    for(uint32_t n=0;n<SueGetNNodes();++n) {
      auto first=SueGetDevice(n,0); if(!first) continue;
      for(uint32_t j=0;j<first->GetNode()->GetNDevices();++j) {
        auto d=SueGetDevice(n,j); if(!d) continue;
        uint64_t sessions=0,replays=0,nacks=0,discards=0,blocked=0;
        if(d->GetLlrEngine()) { auto c=d->GetLlrEngine()->GetCounters(); sessions=c.tx_replay; replays=c.tx_replay_frames; nacks=c.tx_nack_ctl_os; discards=c.tx_discard; }
        if(d->GetCbfcEngine()) { auto c=d->GetCbfcEngine()->GetCounters(); for(auto v:c.credit_exhausted) blocked+=v; }
        ports<<n<<','<<j<<','<<sessions<<','<<replays<<','<<nacks<<','<<discards<<','<<blocked<<'\n';
        if(discards) ok=false;
      }
    }
    std::cout<<(ok?"SUE HARNESS PASS":"SUE HARNESS FAIL")<<" tasks="<<tasks.size()<<'\n';
    Simulator::Destroy(); return ok?0:1;
  } catch(const std::exception& e) { std::cerr<<"SUE HARNESS ERROR: "<<e.what()<<'\n'; return 2; }
}

// SUE v3 adapter: protocol engines are unchanged. Single-thread discrete events.
// The UCIe abstraction is a bounded per-card shared packet pool and eight
// directed, independently serialized ring edges. It is not a UCIe PHY model.
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/sue-config.h"
#include "ns3/sue-header.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-channel.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-routing.h"
#include "ns3/sue-frame-bad-tag.h"
#include "ns3/sue-error-model.h"
#include <array>
#include <map>
#include <set>
#include <vector>
#include <fstream>
#include <sstream>
#include <functional>
#include <stdexcept>
#include <algorithm>
using namespace ns3;
using Fn=std::function<void()>;
static void Later(Time delay, Fn f) { Simulator::Schedule(delay, [f](){f();}); }
class V3Tag: public Tag {
public:
 uint32_t id=0,frag=0,bytes=0,reply=0;
 static TypeId GetTypeId(){static TypeId t=TypeId("ns3::SueV3TaskTag").SetParent<Tag>().AddConstructor<V3Tag>();return t;}
 TypeId GetInstanceTypeId()const override{return GetTypeId();}
 uint32_t GetSerializedSize()const override{return 16;}
 void Serialize(TagBuffer b)const override{b.WriteU32(id);b.WriteU32(frag);b.WriteU32(bytes);b.WriteU32(reply);}
 void Deserialize(TagBuffer b)override{id=b.ReadU32();frag=b.ReadU32();bytes=b.ReadU32();reply=b.ReadU32();}
 void Print(std::ostream&o)const override{o<<id<<":"<<frag<<":"<<reply;}
};
struct Port {uint32_t chip,die,i,sw,vrf,physical,swidx; Ptr<SueNetDevice> host,fabric;};
struct Task {
 uint32_t sc,sd,dc,dd,vc; uint64_t bytes,delay; int group;
 std::vector<uint32_t> deps,next; uint32_t remaining=0,frag=0; uint64_t sent=0,received=0,blocked=0,invalid=0,duplicates=0;
 int64_t ready=-1,start=-1,dataEnd=-1,end=-1; int port=-1,proxy=-1,hops=0;
 std::set<uint32_t> seen; std::string status="pending";
 int resource=-1,peer=-1; uint64_t serviceNs=0,units=0,localSent=0,localReceived=0;
 int64_t serviceStart=-1;
};
struct Edge {Time available; uint64_t bytes=0,frames=0,queuePs=0;};
struct Ring {uint64_t used=0,peak=0,blocked=0; std::array<Edge,8> edges;};
struct Wire {uint64_t bytes=0,frames=0,busyPs=0; int64_t lastEnd=0;};
static std::vector<Ptr<Node>> hosts;
static std::vector<Ptr<SUESwitchNode>> switches;
static std::vector<Port> ports;
static std::map<std::tuple<uint32_t,uint32_t,uint32_t>,uint32_t> indexPort;
static std::map<Ptr<SueNetDevice>,uint32_t> devPort;
static std::map<Ptr<NetDevice>,Wire> wires;
static std::vector<Task> tasks;
static std::map<Ptr<SueNetDevice>,Ptr<SueEgressQueue>> observedQueues;
static uint64_t portSampleNs=0;
static std::ofstream portSamples;
static int64_t lastPortSample=-1;
static std::vector<Ring> rings;
static std::map<std::tuple<uint32_t,uint32_t,int>,Time> resourceAvailable;
static uint32_t nx=0,nm=0,nsw=0,pure=0,seed=1,payload=2048,badEvery=0;
static uint64_t interval=10,visibility=20,queueBytes=65536,ringBytes=65536,ringGbps=400,ringDelay=20,stopNs=1000000;
static uint32_t finished=0;
static uint64_t pathErrors=0,unknownFrames=0;
static Port& P(uint32_t c,uint32_t d,uint32_t i){return ports.at(indexPort.at({c,d,i}));}
static uint32_t Key(uint32_t c,uint32_t d){return pure?c:4*c+d;}
static Mac48Address SaMac(uint32_t c,uint32_t d){
 uint32_t k=Key(c,d); if(k>=1024)throw std::runtime_error("route key overflow");
 uint8_t b[6]={uint8_t(k>>2),uint8_t((k&3)<<6),0xA3,uint8_t(c>>8),uint8_t(c),uint8_t(d)};Mac48Address m; m.CopyFrom(b); return m;
}
static bool Same(const Port&a,const Port&b){return a.sw==b.sw&&a.vrf==b.vrf;}
static int Distance(int a,int b){return std::min((a-b+4)%4,(b-a+4)%4);}
static int Proxy(uint32_t sc,uint32_t sd,uint32_t dc,uint32_t dd,uint32_t i){
 auto&a=P(sc,sd,i); if(Same(a,P(dc,dd,i)))return dd;
 if(sc>=nx||dc>=nx)return -1;
 int best=-1;
 for(int d=0;d<4;++d)if(Same(a,P(dc,d,i))&&(best<0||Distance(d,dd)<Distance(best,dd)))best=d;
 return best;
}
static uint32_t FlowHash(uint32_t id,const Task&t){
 uint32_t h=2166136261u;
 for(uint32_t n:{seed,id,t.sc,t.sd,t.dc,t.dd})for(int shift:{0,8,16,24})h=(h^((n>>shift)&255))*16777619u;
 h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;return h^(h>>16);
}
static void WireTrace(Ptr<const Packet> p,Ptr<NetDevice> src,Ptr<NetDevice>,Time tx,Time){
 auto&w=wires[src];w.bytes+=p->GetSize();++w.frames;w.busyPs+=tx.GetPicoSeconds();w.lastEnd=(Simulator::Now()+tx).GetPicoSeconds();
}
// Passive snapshots only: never call CanSend, dequeue or other stateful APIs.
static void SamplePorts(){
 int64_t now=Simulator::Now().GetPicoSeconds();
 if(now==lastPortSample)return;lastPortSample=now;
 for(auto&r:ports)for(int side=0;side<2;++side){
  auto d=side?r.fabric:r.host;auto w=wires[d];auto cb=d->GetCbfcEngine();
  uint64_t queued=0,blocked=0;double peakCredit=0;
  for(uint32_t vc=0;vc<8;++vc){queued+=observedQueues.at(d)->GetNBytes(vc);blocked+=cb->GetCounters().credit_exhausted[vc];
   auto limit=cb->GetSvcCreditLimit(vc);if(limit)peakCredit=std::max(peakCredit,double(cb->GetSvcCu(vc))/limit);}
  int64_t busy=int64_t(w.busyPs)-std::max<int64_t>(0,w.lastEnd-now);
  portSamples<<now<<','<<r.chip<<','<<r.die<<','<<r.i<<','<<r.sw<<','<<r.physical<<','<<side<<','<<busy<<','<<queued<<','<<peakCredit<<','<<blocked<<'\n';
 }
}
static void PollPorts(){SamplePorts();Later(NanoSeconds(portSampleNs),[](){PollPorts();});}
static void Begin(uint32_t id);
static void Complete(uint32_t id){
 auto&t=tasks.at(id);if(t.end>=0){++t.invalid;return;}
 t.end=Simulator::Now().GetPicoSeconds();t.status="complete";
 for(auto child:t.next)Simulator::ScheduleWithContext(hosts[tasks[child].sc]->GetId(),NanoSeconds(visibility),[child](){
   auto&c=tasks[child];if(!c.remaining)throw std::runtime_error("duplicate dependency");
   if(--c.remaining==0){c.ready=Simulator::Now().GetPicoSeconds();Later(NanoSeconds(c.delay),[child](){Begin(child);});}
 });
 if(++finished==tasks.size())Simulator::Stop();
}
// Ring pool bounds all admitted packets, including inter-hop queues. A packet
// keeps its reservation until final delivery. Admission holds SUE ingress credit
// when the pool is full; separate directed links share it without VC deadlock.
static void RingHop(uint32_t chip,int from,int to,uint32_t bytes,Fn done){
 if(from==to){rings[chip].used-=bytes;done();return;}
 int cw=(to-from+4)%4,ccw=(from-to+4)%4;
 int dir=cw<=ccw?0:1,next=(from+(dir?3:1))%4;
 auto&e=rings[chip].edges[2*from+dir];Time now=Simulator::Now(),start=std::max(now,e.available);
 Time serial=PicoSeconds((uint64_t(bytes)*8000+ringGbps-1)/ringGbps);
 e.available=start+serial;e.bytes+=bytes;++e.frames;e.queuePs+=(start-now).GetPicoSeconds();
 Later(start-now+serial+NanoSeconds(ringDelay),[=](){RingHop(chip,next,to,bytes,done);});
}
static void RingTransfer(uint32_t chip,int from,int to,uint32_t bytes,Fn admitted,Fn done){
 if(from==to){admitted();done();return;}
 auto&r=rings.at(chip);
 if(r.used+bytes>ringBytes){++r.blocked;Later(NanoSeconds(interval),[=](){RingTransfer(chip,from,to,bytes,admitted,done);});return;}
 r.used+=bytes;r.peak=std::max(r.peak,r.used);admitted();RingHop(chip,from,to,bytes,done);
}
static Ptr<Packet> Frame(uint32_t id,uint32_t n,uint32_t frag,bool reply){
 auto&t=tasks[id];SueHeader h;
 h.SetSrc(reply?SaMac(t.dc,t.proxy):SaMac(t.sc,t.sd));h.SetDst(reply?SaMac(t.sc,t.sd):SaMac(t.dc,t.dd));
 h.SetEtherType(SueConfig::EthType1());h.SetVlan(false,0,false,0);
 SueShimHeader shim;shim.f=true;shim.ttl=2;shim.dscp=t.vc;shim.flowLabel=id&255;h.SetShim(shim);
 auto p=Create<Packet>(n);p->AddHeader(h);V3Tag tag;tag.id=id;tag.frag=frag;tag.bytes=n;tag.reply=reply;p->AddPacketTag(tag);return p;
}
static void SendReply(uint32_t id){
 auto&t=tasks[id];auto dev=P(t.dc,t.proxy,t.port).host;auto p=Frame(id,64,0,true);
 if(!dev->EgressCanAccept(t.vc,p->GetSize())){++t.blocked;Later(NanoSeconds(interval),[id](){SendReply(id);});return;}
 dev->SwitchSend(t.vc,p);
}
static void Delivered(uint32_t id,uint32_t frag,uint32_t bytes){
 auto&t=tasks[id];if(!t.seen.insert(frag).second){++t.duplicates;return;}
 t.received+=bytes;if(t.received>t.bytes)throw std::runtime_error("payload overcount");
 if(t.received==t.bytes){t.dataEnd=Simulator::Now().GetPicoSeconds();
   RingTransfer(t.dc,t.dd,t.proxy,64,[](){},[id](){SendReply(id);});}
}
static void Receive(Ptr<SueNetDevice> dev,Ptr<Packet> p,SueHeader h){
 V3Tag tag;uint32_t wire=p->GetSize();
 if(!p->PeekPacketTag(tag)||tag.id>=tasks.size()){++unknownFrames;dev->RefundCredit(h.GetShim().dscp&7,wire);return;}
 auto&t=tasks[tag.id];auto&pp=ports.at(devPort.at(dev));SueFrameBadTag bad;
 bool valid=!p->PeekPacketTag(bad)&&tag.bytes==wire-h.GetSerializedSize()&&!h.HasVlan()&&h.GetShim().dscp==t.vc;
 if(tag.reply)valid=valid&&pp.chip==t.sc&&pp.die==t.sd&&int(pp.i)==t.port&&
   h.GetDst()==SaMac(t.sc,t.sd)&&h.GetSrc()==SaMac(t.dc,t.proxy)&&tag.bytes==64&&t.dataEnd>=0;
 else valid=valid&&pp.chip==t.dc&&int(pp.die)==t.proxy&&int(pp.i)==t.port&&
   h.GetDst()==SaMac(t.dc,t.dd)&&h.GetSrc()==SaMac(t.sc,t.sd)&&tag.frag<(t.bytes+payload-1)/payload&&
   tag.bytes==std::min<uint64_t>(payload,t.bytes-uint64_t(tag.frag)*payload);
 if(!valid){++t.invalid;++pathErrors;dev->RefundCredit(t.vc,wire);return;}
 if(tag.reply){dev->RefundCredit(t.vc,wire);Complete(tag.id);return;}
 RingTransfer(t.dc,t.proxy,t.dd,tag.bytes,[=](){dev->RefundCredit(t.vc,wire);},[=](){Delivered(tag.id,tag.frag,tag.bytes);});
}
static void Pump(uint32_t id){
 auto&t=tasks[id];uint32_t n=std::min<uint64_t>(payload,t.bytes-t.sent);auto p=Frame(id,n,t.frag,false);
 auto dev=P(t.sc,t.sd,t.port).host;
 if(!dev->EgressCanAccept(t.vc,p->GetSize())){++t.blocked;Later(NanoSeconds(interval),[id](){Pump(id);});return;}
 dev->SwitchSend(t.vc,p);t.sent+=n;++t.frag;
 if(t.sent<t.bytes)Later(NanoSeconds(interval),[id](){Pump(id);});
}
// Optional local resources: one FIFO server per Die/type (MPU: per chip).
// Kind 3 is a real local ring transfer, never sent through the SUE fabric.
// Kind 4 is a host-to-XPU PCIe service queue shared by all Die of one chip.
// It is a bandwidth/FIFO abstraction, not a PCIe transaction-layer model.
static void LocalPump(uint32_t id){
 auto&t=tasks[id];uint32_t n=std::min<uint64_t>(payload,t.units-t.localSent);
 RingTransfer(t.sc,t.sd,t.peer,n,[id,n](){auto&t=tasks[id];t.localSent+=n;
   if(t.localSent<t.units)Later(NanoSeconds(interval),[id](){LocalPump(id);});
 },[id,n](){auto&t=tasks[id];t.localReceived+=n;if(t.localReceived==t.units){t.dataEnd=Simulator::Now().GetPicoSeconds();Complete(id);}});
}
static void Begin(uint32_t id){
 auto&t=tasks[id];t.start=Simulator::Now().GetPicoSeconds();if(t.ready<0)t.ready=t.start-int64_t(t.delay)*1000;
 if(t.resource>=0){
   if(t.resource==3){t.serviceStart=t.start;LocalPump(id);return;}
   uint32_t resourceDie=t.resource==4?0:t.sd;
   auto&available=resourceAvailable[{t.sc,resourceDie,t.resource}];Time start=std::max(Simulator::Now(),available);
   t.serviceStart=start.GetPicoSeconds();available=start+NanoSeconds(t.serviceNs);
   Later(available-Simulator::Now(),[id](){tasks[id].dataEnd=Simulator::Now().GetPicoSeconds();Complete(id);});return;
 }
 if(!t.bytes){t.dataEnd=t.start;Complete(id);return;}
 uint32_t hash=FlowHash(id,t);std::vector<int> groups;
 for(int g=0;g<2;++g){bool ok=true;for(int i=4*g;i<4*g+4;++i)ok=ok&&Proxy(t.sc,t.sd,t.dc,t.dd,i)>=0;if(ok)groups.push_back(g);}
 if(t.group<0)t.group=groups.empty()?int((hash>>2)%2):groups[(hash>>2)%groups.size()];
 t.port=4*t.group+hash%4;t.proxy=Proxy(t.sc,t.sd,t.dc,t.dd,t.port);
 if(t.proxy<0){t.status="unreachable";return;}
 t.hops=Distance(t.proxy,t.dd);t.status="running";Pump(id);
}
static uint64_t xpuPortBps=200000000000ULL,mpuPortBps=200000000000ULL;
static Ptr<SueNetDevice> Device(Ptr<Node> node,Ptr<SUESwitchNode> sw,uint64_t bps){
 auto d=CreateObject<SueNetDevice>();if(sw)sw->AddPort(d);else node->AddDevice(d);
 d->SetDataRate(DataRate(bps));auto q=CreateObject<SueEgressQueue>();d->SetEgressQueue(q);observedQueues[d]=q;
 auto fc=CreateObject<SueFcController>();fc->SetCbfc(CreateObject<SueCbfcEngine>());d->SetFcController(fc);
 d->SetLlrEngine(CreateObject<SueLlrEngine>());d->SetLlrEnabled(true);return d;
}
static void Build(const std::string&dir){
 SueReadConf(dir+"/sue.conf");
 for(uint32_t c=0;c<nx+nm;++c)hosts.push_back(CreateObject<Node>());
 for(uint32_t s=0;s<nsw;++s)switches.push_back(CreateObject<SUESwitchNode>());
 rings.resize(nx+nm);
 std::ifstream f(dir+"/ports.tsv");if(!f)throw std::runtime_error("missing ports.tsv");Port r;
 std::set<std::pair<uint32_t,uint32_t>> physical;
 while(f>>r.chip>>r.die>>r.i>>r.sw>>r.vrf>>r.physical){
   if(r.chip>=nx+nm||r.die>=(r.chip<nx?4u:1u)||r.i>=8||r.sw>=nsw||
      !physical.insert({r.sw,r.physical}).second||indexPort.count({r.chip,r.die,r.i}))throw std::runtime_error("invalid physical port");
   auto sw=switches[r.sw];r.swidx=sw->GetNPorts();
   uint64_t bps=r.chip<nx?xpuPortBps:mpuPortBps;
   r.host=Device(hosts[r.chip],nullptr,bps);r.fabric=Device(sw,sw,bps);
   auto ch=CreateObject<SueChannel>();ch->SetAttribute("Delay",TimeValue(SueConfig::LinkPropagation()));
   r.host->Attach(ch);r.fabric->Attach(ch);ch->TraceConnectWithoutContext("TxRxSue",MakeCallback(&WireTrace));
   indexPort[{r.chip,r.die,r.i}]=ports.size();devPort[r.host]=ports.size();ports.push_back(r);
 }
 if(!f.eof()||ports.size()!=32*nx+8*nm)throw std::runtime_error("incomplete port map");
 for(auto&r:ports){
   for(auto d:{r.host,r.fabric})ApplySueConfig(d);
   r.host->SetDeferCreditRefund(true);r.host->SetReceiveUpcall(MakeCallback(&Receive));
   for(int vc=0;vc<8;++vc)r.host->SetEgressQueueVcLimit(vc,queueBytes);
   if(SueConfig::PortToVrf(r.swidx)!=r.vrf)throw std::runtime_error("port VRF mismatch");
   if(badEvery&&r.chip==0){auto e=CreateObject<SueErrorModel>();e->SetBadEvery(badEvery);r.fabric->SetErrorModel(e);}
 }
 // Static routes only. Hosts are never used as fabric transit nodes.
 std::map<std::pair<uint32_t,uint32_t>,std::vector<uint32_t>> domains;
 for(uint32_t j=0;j<ports.size();++j)domains[{ports[j].sw,ports[j].vrf}].push_back(j);
 for(auto&sw:switches){sw->SetRoutingTable(0,CreateObject<SueRoutingTable>());sw->SetForwardingBudget(SueConfig::SwitchForwardingBudget());sw->ApplyCbfcBackpressure(SueConfig::CbfcEgressBufBytes());}
 for(auto&item:domains){
   auto rt=CreateObject<SueRoutingTable>();std::map<std::pair<uint32_t,uint32_t>,uint32_t> member;
   for(auto j:item.second){auto&p=ports[j];member[{p.chip,p.die}]=j;}
   std::map<uint32_t,uint32_t> keyPorts;
   for(uint32_t c=0;c<nx+nm;++c)for(uint32_t d=0;d<(c<nx?4u:1u);++d){
     int chosen=-1;auto found=member.find({c,d});
     if(found!=member.end())chosen=found->second;
     else if(c<nx)for(int pd=0;pd<4;++pd){auto it=member.find({c,uint32_t(pd)});
       if(it!=member.end()&&(chosen<0||Distance(pd,d)<Distance(ports[chosen].die,d)))chosen=it->second;}
     if(chosen<0)continue;auto&p=ports[chosen];uint32_t k=Key(c,d);
     if(k>=1024)throw std::runtime_error("route overflow");
     if(keyPorts.count(k)&&keyPorts[k]!=p.swidx)throw std::runtime_error("route key collision");keyPorts[k]=p.swidx;
   }
   for(auto kv:keyPorts)rt->AddPrefix(kv.first,10,{kv.second});switches[item.first.first]->SetRoutingTable(item.first.second,rt);
 }
 for(auto&r:ports)for(auto d:{r.host,r.fabric})d->GetLlrEngine()->Start();
}
int main(int argc,char**argv){
 std::string dir,out;CommandLine cmd;
 cmd.AddValue("case","Validated v3 case",dir);cmd.AddValue("out","Fresh run directory",out);
 cmd.AddValue("xpu","XPU cards",nx);cmd.AddValue("mpu","MPU cards",nm);cmd.AddValue("switches","Switches",nsw);cmd.AddValue("pure","Per-Die domains: chip in routing key, die in MAC low bits",pure);
 cmd.AddValue("seed","Hash seed",seed);cmd.AddValue("payload","Fragment bytes",payload);cmd.AddValue("intervalNs","Injection/retry spacing",interval);
 cmd.AddValue("visibilityNs","DAG notification delay",visibility);cmd.AddValue("stopNs","Deadline",stopNs);cmd.AddValue("queueBytes","Endpoint egress per-VC buffer",queueBytes);
 cmd.AddValue("ringBytes","Per-card UCIe shared buffer",ringBytes);cmd.AddValue("ringGbps","Per-direction ring rate",ringGbps);cmd.AddValue("ringDelayNs","Ring hop propagation and processing",ringDelay);
 cmd.AddValue("portSampleNs","Passive port telemetry period, 0 disables",portSampleNs);
 cmd.AddValue("badEvery","Corrupt every Nth ingress data frame from card0",badEvery);cmd.AddValue("xpuPortBps","XPU link rate in bit/s",xpuPortBps);cmd.AddValue("mpuPortBps","MPU link rate in bit/s",mpuPortBps);cmd.Parse(argc,argv);
 try{
   if(!xpuPortBps||!mpuPortBps||dir.empty()||out.empty()||!nx||nx+nm>1024||!nsw||!payload||!interval||!visibility||!ringGbps||ringBytes<std::max(payload,64u)||queueBytes<payload+32)throw std::runtime_error("invalid arguments");
   Time::SetResolution(Time::PS);RngSeedManager::SetSeed(seed);
   std::ifstream f(dir+"/tasks.tsv");if(!f)throw std::runtime_error("missing tasks");std::string line;
   while(std::getline(f,line)){if(line.empty()||line[0]=='#')continue;std::istringstream s(line);uint32_t id;Task t;std::string deps;
     if(!(s>>id>>t.sc>>t.sd>>t.dc>>t.dd>>t.bytes>>t.delay>>t.vc>>t.group>>deps)||id!=tasks.size()||t.sc>=nx+nm||t.dc>=nx+nm||t.sd>=(t.sc<nx?4u:1u)||t.dd>=(t.dc<nx?4u:1u)||t.vc>7||t.group<-1||t.group>1)throw std::runtime_error("invalid task");
     if(!t.bytes&&(t.sc!=t.dc||t.sd!=t.dd))throw std::runtime_error("wait must be local");
     if(t.bytes&&t.sc==t.dc&&t.sd==t.dd)throw std::runtime_error("same-SA network task");
     if(deps!="-"){std::istringstream ds(deps);std::string d;while(std::getline(ds,d,',')){auto dep=std::stoul(d);if(dep>=id||std::find(t.deps.begin(),t.deps.end(),dep)!=t.deps.end())throw std::runtime_error("non-topological DAG");t.deps.push_back(dep);}}
     tasks.push_back(t);
   }
   if(tasks.empty())throw std::runtime_error("empty tasks");
   std::ifstream resources(dir+"/resources.tsv");
   if(resources){uint32_t id;int kind,peer;uint64_t service,units;std::set<uint32_t> seen;
     while(resources>>id>>kind>>service>>units>>peer){
       if(id>=tasks.size()||!seen.insert(id).second||kind<0||kind>4||tasks[id].bytes||
          (kind==2?(tasks[id].sc<nx):(tasks[id].sc>=nx))||
          (kind==3?(peer<0||peer>3||peer==int(tasks[id].sd)||!units||service!=0):(peer!=-1||!service)))throw std::runtime_error("invalid resource");
       auto&t=tasks[id];t.resource=kind;t.serviceNs=service;t.units=units;t.peer=peer;
     }if(!resources.eof())throw std::runtime_error("malformed resource file");
   }
   for(uint32_t i=0;i<tasks.size();++i){tasks[i].remaining=tasks[i].deps.size();for(auto d:tasks[i].deps)tasks[d].next.push_back(i);}
   Build(dir);
   if(portSampleNs){portSamples.open(out+"/port_timeseries.csv");if(!portSamples)throw std::runtime_error("cannot open telemetry");
    portSamples<<"time_ps,chip,die,port,switch,physical_port,side,tx_busy_ps,egress_bytes,credit_used_fraction_max,credit_exhausted\n";PollPorts();}
   for(uint32_t i=0;i<tasks.size();++i)if(tasks[i].deps.empty())Simulator::ScheduleWithContext(hosts[tasks[i].sc]->GetId(),NanoSeconds(tasks[i].delay),[i](){Begin(i);});
   Simulator::Stop(NanoSeconds(stopNs));Simulator::Run();if(portSampleNs){SamplePorts();portSamples.close();}bool ok=pathErrors==0&&unknownFrames==0;
   std::ofstream stats(out+"/task_statistics.csv");stats<<"id,src_chip,src_die,dst_chip,dst_die,bytes,received_bytes,ready_ps,start_ps,data_end_ps,end_ps,group,port,proxy_die,ucie_hops,queue_blocked_polls,duplicates,invalid_frames,status\n";
   for(uint32_t i=0;i<tasks.size();++i){auto&t=tasks[i];bool complete=t.end>=0&&t.received==t.bytes&&!t.invalid;
     for(auto d:t.deps)if(tasks[d].end<0||t.start<tasks[d].end+int64_t(visibility+t.delay)*1000)complete=false;
     if(t.status=="complete"&&!complete)t.status="invalid";ok=ok&&complete;
     stats<<i<<','<<t.sc<<','<<t.sd<<','<<t.dc<<','<<t.dd<<','<<t.bytes<<','<<t.received<<','<<t.ready<<','<<t.start<<','<<t.dataEnd<<','<<t.end<<','<<t.group<<','<<t.port<<','<<t.proxy<<','<<t.hops<<','<<t.blocked<<','<<t.duplicates<<','<<t.invalid<<','<<t.status<<'\n';}
   std::ofstream ps(out+"/port_statistics.csv");ps<<"chip,die,port,switch,physical_port,side,wire_bytes,frames,tx_busy_ps,replay_frames,discard,credit_exhausted\n";
   for(auto&r:ports)for(int side=0;side<2;++side){auto d=side?r.fabric:r.host;auto w=wires[d];auto l=d->GetLlrEngine()->GetCounters();auto c=d->GetCbfcEngine()->GetCounters();uint64_t blocked=0;for(auto v:c.credit_exhausted)blocked+=v;
     ps<<r.chip<<','<<r.die<<','<<r.i<<','<<r.sw<<','<<r.physical<<','<<side<<','<<w.bytes<<','<<w.frames<<','<<(w.busyPs-std::max<int64_t>(0,w.lastEnd-Simulator::Now().GetPicoSeconds()))<<','<<l.tx_replay_frames<<','<<l.tx_discard<<','<<blocked<<'\n';if(l.tx_discard)ok=false;}
   std::ofstream rs(out+"/ring_statistics.csv");rs<<"chip,from_die,to_die,bytes,frames,queue_ps,pool_peak_bytes,pool_blocked_polls\n";
   for(uint32_t c=0;c<nx;++c)for(int e=0;e<8;++e){auto&r=rings[c];auto&q=r.edges[e];rs<<c<<','<<e/2<<','<<(e/2+(e%2?3:1))%4<<','<<q.bytes<<','<<q.frames<<','<<q.queuePs<<','<<r.peak<<','<<r.blocked<<'\n';}
   std::ofstream resourceStats(out+"/resource_statistics.csv");resourceStats<<"id,chip,die,kind,units,service_ns,queued_ps,service_start_ps,end_ps,local_received_bytes\n";
   for(uint32_t i=0;i<tasks.size();++i){auto&t=tasks[i];if(t.resource>=0)resourceStats<<i<<','<<t.sc<<','<<t.sd<<','<<t.resource<<','<<t.units<<','<<t.serviceNs<<','<<(t.serviceStart<0?-1:t.serviceStart-t.start)<<','<<t.serviceStart<<','<<t.end<<','<<t.localReceived<<'\n';}
   std::ofstream meta(out+"/engine.json");meta<<"{\"ok\":"<<(ok?"true":"false")<<",\"finished\":"<<finished<<",\"tasks\":"<<tasks.size()<<",\"sim_end_ps\":"<<Simulator::Now().GetPicoSeconds()<<",\"path_errors\":"<<pathErrors<<",\"unknown_frames\":"<<unknownFrames<<"}\n";
   std::cout<<(ok?"SUE V3 PASS":"SUE V3 FAIL")<<" tasks="<<tasks.size()<<" finished="<<finished<<"\n";
   Simulator::Destroy();return ok?0:1;
 }catch(const std::exception&e){std::cerr<<"SUE V3 ERROR "<<e.what()<<"\n";return 2;}
}

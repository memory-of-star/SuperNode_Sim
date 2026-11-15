// Adapter only: uses the original SUE AFH, LLR and CBFC engines unchanged.
#include "ns3/network-module.h"
#include "ns3/sue-config.h"
#include "ns3/sue-header.h"
#include "ns3/sue-net-device.h"
#include "ns3/sue-channel.h"
#include "ns3/sue-egress-queue.h"
#include "ns3/sue-fc-controller.h"
#include "ns3/sue-llr-engine.h"
#include "ns3/sue-cbfc-engine.h"
#include "ns3/sue-switch-node.h"
#include "ns3/sue-routing.h"
#include "ns3/sue-error-model.h"
#include <unordered_map>
class HmPacketTag:public Tag {
public:uint64_t id=0;uint32_t die=0,frag=0,bytes=0;
 static TypeId GetTypeId(){static TypeId t=TypeId("ns3::HmPacketTag").SetParent<Tag>().AddConstructor<HmPacketTag>();return t;}
 TypeId GetInstanceTypeId()const override{return GetTypeId();}
 uint32_t GetSerializedSize()const override{return 20;}
 void Serialize(TagBuffer b)const override{b.WriteU64(id);b.WriteU32(die);b.WriteU32(frag);b.WriteU32(bytes);}
 void Deserialize(TagBuffer b)override{id=b.ReadU64();die=b.ReadU32();frag=b.ReadU32();bytes=b.ReadU32();}
 void Print(std::ostream&o)const override{o<<id<<':'<<die<<':'<<frag;}
};
class SuePacketNetwork:public Network {
 struct Message{int s,d;uint64_t bytes,received=0;int64_t start;Fn done;std::array<uint32_t,4> next{};};
 struct Pending{uint64_t id,left;uint32_t frag=0;};
 std::unordered_map<uint64_t,Message> messages;
 std::vector<Ptr<Node>> nodes;Ptr<SUESwitchNode> sw;
 std::vector<Ptr<SueNetDevice>> hosts,fabric;
 std::vector<std::deque<Pending>> pending;
 std::vector<bool> armed;
 std::map<Ptr<NetDevice>,std::pair<int,int>> ports;
 int64_t linkPs=20000,switchPs=300000,endpointPs=0;
 uint64_t nextId=0;uint32_t payload=2048,credits=512;uint64_t queueBytes=65536;
 std::vector<uint64_t> wireTx,wireRx,framesTx,framesRx,blocked;
 Ptr<SueNetDevice> device(Ptr<Node> node,bool isSwitch,uint64_t rate){
  auto d=CreateObject<SueNetDevice>();if(isSwitch)sw->AddPort(d);else node->AddDevice(d);
  d->SetDataRate(DataRate(rate));d->SetEgressQueue(CreateObject<SueEgressQueue>());
  auto cb=CreateObject<SueCbfcEngine>();cb->SetCreditSize(256);for(int vc=0;vc<8;vc++)cb->SetVcCreditLimit(vc,credits);cb->Init();
  auto fc=CreateObject<SueFcController>();fc->SetCbfc(cb);d->SetFcController(fc);
  auto llr=CreateObject<SueLlrEngine>();d->SetLlrEngine(llr);d->SetLlrEnabled(true);
  // Existing per-frame ACK mode: no idle periodic ACK events during long compute gaps.
  llr->SetAckPeriod(PicoSeconds(0));llr->SetReplayTimerMax(MicroSeconds(50));llr->SetInitTimerMax(MicroSeconds(50));llr->SetDataAgeTimerMax(MilliSeconds(100));
  d->SetCtlOsTargetSpacing(400);d->SetIfg(PicoSeconds((12000000000000ULL+rate/8-1)/(rate/8)));
  for(int vc=0;vc<8;vc++)d->SetEgressQueueVcLimit(vc,queueBytes);
  return d;
 }
 void wake(int port){if(armed[port]||pending[port].empty())return;armed[port]=true;later(notify,[this,port](){armed[port]=false;pump(port);});}
 void pump(int port){
  if(pending[port].empty())return;auto q=pending[port].front();auto&m=messages.at(q.id);
  uint32_t size=std::min<uint64_t>(payload,q.left),die=port%4;
  auto p=Create<Packet>(size);SueHeader h;h.SetSrc(SueMacFromXpuId(port));h.SetDst(SueMacFromXpuId(4*m.d+die));h.SetEtherType(SueConfig::EthType1());h.SetVlan(false,0,false,0);
  SueShimHeader shim;shim.f=true;shim.ttl=2;shim.dscp=0;shim.flowLabel=q.id&255;h.SetShim(shim);p->AddHeader(h);
  if(!hosts[port]->EgressCanAccept(0,p->GetSize())){blocked[port]++;return;} // parked until freed hook
  HmPacketTag tag;tag.id=q.id;tag.die=die;tag.frag=q.frag;tag.bytes=size;p->AddPacketTag(tag);
  pending[port].pop_front();q.left-=size;q.frag++;if(q.left)pending[port].push_back(q);
  hosts[port]->SwitchSend(0,p);wake(port);
 }
 void receive(Ptr<SueNetDevice> dev,Ptr<Packet> p,SueHeader h){
  HmPacketTag tag;NS_ABORT_MSG_IF(!p->PeekPacketTag(tag),"missing message tag");auto it=messages.find(tag.id);NS_ABORT_MSG_IF(it==messages.end(),"unknown message");auto&m=it->second;
  NS_ABORT_MSG_IF(dev!=hosts[4*m.d+tag.die]||h.GetXpuId()!=uint32_t(4*m.d+tag.die)||tag.bytes!=p->GetSize()-h.GetSerializedSize(),"invalid packet delivery");
  NS_ABORT_MSG_IF(tag.frag!=m.next[tag.die]++,"duplicate/reordered application fragment");m.received+=tag.bytes;rx[m.d]+=tag.bytes;tx[m.s]+=tag.bytes;
  if(m.received==m.bytes){auto done=m.done;service+=m.bytes/bw;prop+=alpha;slowdown+=std::max(0.,double(now()-m.start-alpha)-m.bytes/bw);messages.erase(it);later(notify,done);}
 }
 void trace(Ptr<const Packet> p,Ptr<NetDevice> src,Ptr<NetDevice>,Time,Time){
  auto [port,side]=ports.at(src);int chip=port/4;busy[chip]+=p->GetSize()/bw;
  if(side==0){wireTx[chip]+=p->GetSize();framesTx[chip]++;}else{wireRx[chip]+=p->GetSize();framesRx[chip]++;}
 }
public:
 SuePacketNetwork(int n_,bool duplex,double b,int64_t a,int64_t nt,bool switched):Network(n_,duplex,b,a,nt,switched),pending(4*n_),armed(4*n_),wireTx(n_),wireRx(n_),framesTx(n_),framesRx(n_),blocked(4*n_){
  NS_ABORT_MSG_IF(!switched,"SUE backend requires single_switch");
  if(const char*v=std::getenv("HM_SUE_PAYLOAD"))payload=std::stoul(v);
  if(const char*v=std::getenv("HM_SUE_CREDITS"))credits=std::stoul(v);
  NS_ABORT_MSG_IF(!payload||payload>8192||credits<(payload+64+255)/256,"invalid payload/credit profile");
  // Same 20 ns propagation / 300 ns forwarding-budget convention as v2/run.py.
  // Only XPU access overhead changes to meet the requested end-to-end fixed budget.
  if(const char*v=std::getenv("HM_SUE_LINK_PROPAGATION_NS"))linkPs=std::llround(std::stod(v)*1000);
  if(const char*v=std::getenv("HM_SUE_SWITCH_FORWARDING_NS"))switchPs=std::llround(std::stod(v)*1000);
  NS_ABORT_MSG_IF(a<2*linkPs+switchPs || (a-2*linkPs-switchPs)%2,"invalid fixed latency budget");
  endpointPs=(a-2*linkPs-switchPs)/2;
  sw=CreateObject<SUESwitchNode>();auto rt=CreateObject<SueRoutingTable>();
  uint64_t rate=uint64_t(b*8/(duplex?4:8)); // Per-die fixed full-duplex: main chip 64 GB/s per direction.
  for(int i=0;i<4*n;i++){
   auto node=CreateObject<Node>();nodes.push_back(node);auto h=device(node,false,rate),f=device(sw,true,rate);hosts.push_back(h);fabric.push_back(f);
   auto ch=CreateObject<SueChannel>();ch->SetAttribute("Delay",TimeValue(PicoSeconds(linkPs+endpointPs)));h->Attach(ch);f->Attach(ch);
   ports[h]={i,0};ports[f]={i,1};ch->TraceConnectWithoutContext("TxRxSue",MakeCallback(&SuePacketNetwork::trace,this));
   h->SetReceiveUpcall(MakeCallback(&SuePacketNetwork::receive,this));h->SetEgressFreedHook([this,i](){wake(i);});
   rt->AddPrefix(i,10,{uint32_t(i)});
  }
  sw->SetRoutingTable(rt);sw->SetForwardingBudget(PicoSeconds(switchPs));sw->ApplyCbfcBackpressure(credits*256);
  if(const char*v=std::getenv("HM_SUE_BAD_EVERY")){auto em=CreateObject<SueErrorModel>();em->SetBadEvery(std::stoul(v));fabric[0]->SetErrorModel(em);}
  for(auto d:hosts)d->GetLlrEngine()->Start();for(auto d:fabric)d->GetLlrEngine()->Start();
 }
 void send(int s,int d,double bytes,Fn done){
  NS_ABORT_MSG_IF(std::floor(bytes)!=bytes||bytes<=0,"fractional/zero payload");uint64_t id=nextId++,sz=bytes;hops++;
  messages.emplace(id,Message{s,d,sz,0,now(),done,{}});
  for(int die=0;die<4;die++){uint64_t part=sz/4+(uint64_t(die)<sz%4);if(part){pending[4*s+die].push_back({id,part,0});wake(4*s+die);}}
 }
 void output(const std::string&prefix){
  NS_ABORT_MSG_IF(!messages.empty(),"incomplete SUE messages");
  std::ofstream timing(prefix+"-sue-profile.json");
  timing<<"{\"backend\":\"sue_packets\",\"link_propagation_ps\":"<<linkPs
        <<",\"switch_forwarding_budget_ps\":"<<switchPs<<",\"xpu_extra_access_ps\":"<<endpointPs
        <<",\"access_channel_ps\":"<<linkPs+endpointPs<<",\"fixed_path_ps\":"<<alpha
        <<",\"per_die_direction_bps\":"<<hosts[0]->GetDataRate().GetBitRate()
        <<",\"payload\":"<<payload<<",\"credits\":"<<credits<<"}\n";
  std::ofstream f(prefix+"-sue-ports.csv");f<<"chip,die,side,tx_ok,replay_frames,nack_tx,discard,credit_exhausted,cf_tx,ack_tx,queue_blocked_wakeups\n";
  for(int i=0;i<4*n;i++)for(int side=0;side<2;side++){
   auto d=side?fabric[i]:hosts[i];auto l=d->GetLlrEngine()->GetCounters();auto c=d->GetCbfcEngine()->GetCounters();
   NS_ABORT_MSG_IF(l.tx_discard,"SUE discarded frames");
   f<<i/4<<','<<i%4<<','<<side<<','<<l.tx_ok<<','<<l.tx_replay_frames<<','<<l.tx_nack_ctl_os<<','<<l.tx_discard<<','<<c.credit_exhausted[0]<<','<<c.cf_update_tx<<','<<l.tx_ack_ctl_os<<','<<(side?0:blocked[i])<<'\n';
  }
  std::ofstream w(prefix+"-sue-wire.csv");w<<"chip,tx_wire_bytes,rx_wire_bytes,tx_wire_frames,rx_wire_frames\n";
  for(int i=0;i<n;i++)w<<i<<','<<wireTx[i]<<','<<wireRx[i]<<','<<framesTx[i]<<','<<framesRx[i]<<'\n';
 }
};

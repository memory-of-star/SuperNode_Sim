// Transaction-level, single-thread PCIe read model. No changes to SUE protocols.
// Rates serialize modeled bytes, not electrical lanes. No PCIe PHY/FLIT/replay.
#pragma once
#include <deque>
#include <memory>
#include <list>
#include <cmath>
#include <cassert>
#include <iomanip>
namespace pcie {
using Fn=std::function<void()>;
using Accept=std::function<void(Fn)>;
inline uint64_t Now(){return ns3::Simulator::Now().GetPicoSeconds();}
inline void After(uint64_t ps,Fn f){ns3::Simulator::Schedule(ns3::SuePicoSeconds(ps),[f](){f();});}
inline uint64_t Serial(uint64_t n,uint64_t rate){return ns3::SueRoundPs(std::max<uint64_t>(1,std::ceil((long double)n*1e12L/rate)));}
struct Meter {
 uint64_t bytes=0,wire=0,events=0,busy=0,queueWait=0,admitWait=0,used=0,items=0,peak=0,peakItems=0,stalls=0;
};
// Credits are occupied through transport and until downstream accepts. Waiting
// submissions remain owned by their upstream producer, not an infinite Rx FIFO.
class Buffer {
 struct W{uint64_t bytes,time;Accept fn;};std::deque<W> waiting;
 uint64_t cap,countCap;bool pumping=false;
public:
 Meter m;
 Buffer(uint64_t c,uint64_t n):cap(c),countCap(n){}
 void Push(uint64_t n,Accept f){if(n>cap)throw std::runtime_error("PCIe packet larger than buffer");waiting.push_back({n,Now(),f});Drain();}
 void Drain(){if(pumping)return;pumping=true;
  while(!waiting.empty()){
   auto w=waiting.front();if(m.used+w.bytes>cap||m.items>=countCap){++m.stalls;break;}
   waiting.pop_front();m.used+=w.bytes;++m.items;m.peak=std::max(m.peak,m.used);m.peakItems=std::max(m.peakItems,m.items);
   m.admitWait+=Now()-w.time;m.bytes+=w.bytes;++m.events;
   w.fn([this,w](){assert(m.used>=w.bytes&&m.items);m.used-=w.bytes;--m.items;Drain();});
  }pumping=false;
 }
 bool Empty()const{return waiting.empty()&&!m.used&&!m.items;}
};
class Link {
 uint64_t rate,delay,creditDelay,creditUnit,overhead,next=0;
public:
 Buffer slots;
 Link(uint64_t r,uint64_t d,uint64_t cd,uint64_t b,uint64_t h,uint64_t unit=0,uint64_t extra=0):rate(r),delay(d),creditDelay(cd),creditUnit(unit),overhead(extra),slots(b,h){}
 void Send(uint64_t n,Fn accepted,Accept delivered){
  uint64_t charge=creditUnit?((n-overhead+creditUnit-1)/creditUnit)*creditUnit:n;
  slots.Push(charge,[this,n,accepted,delivered](Fn release){
   slots.m.wire+=n;
   accepted();uint64_t start=std::max(Now(),next),duration=Serial(n,rate);
   slots.m.queueWait+=start-Now();slots.m.busy+=duration;next=start+duration;
   After(next-Now()+delay,[this,release,delivered](){delivered([this,release](){After(creditDelay,release);});});
  });
 }
};
struct Input{int mode;uint64_t address,row,stride;uint32_t socket;};
struct Piece{uint64_t address,bytes;};
struct Descriptor {
 uint32_t id,chip,die;uint64_t useful,begin,admit=0,start=0,end=0,wire=0,received=0,written=0,ddr=0,requests=0,completions=0,hits=0;
 uint64_t tagStalls=0,windowStalls=0,lookaheadStalls=0;uint32_t live=0;size_t cursor=0;
 bool ready=false;Input input;std::vector<Piece> pieces;std::set<uint64_t> pages;Fn done;
};
class Model {
 using Hbm=std::function<void(uint32_t,uint32_t,uint32_t,uint64_t,std::function<void(uint64_t)>)>;
 struct Chip {
  uint64_t submitNext=0,inflight=0,peakInflight=0,peakTags=0,peakResident=0,descriptorStalls=0;
  uint32_t tags=0,resident=0,engines=0;bool pumping=false;
  std::deque<uint32_t> external,ready;std::vector<uint32_t> active;size_t rr=0;
 };
 std::map<std::string,uint64_t> c;std::map<uint32_t,Input> inputs;
 std::map<uint32_t,Descriptor> desc;std::vector<Chip> chips;
 std::map<std::string,std::unique_ptr<Link>> links;
 std::map<uint32_t,std::unique_ptr<Buffer>> rx,ddrSlots;
 std::map<std::pair<uint32_t,uint32_t>,uint64_t> ddrNext;
 std::map<uint32_t,uint64_t> packNext;
 std::map<std::pair<uint32_t,uint32_t>,std::list<uint64_t>> cache;
 std::map<uint32_t,Meter> ddrMeters,packMeters,hbmMeters;
 Hbm hbm;bool enabled=false;
 uint64_t C(const std::string&k){return c.at(k);}
 uint32_t Socket(uint32_t chip){return chip/8*2+(chip%8)/4;}
 Link& GetLink(const std::string&key,uint64_t rate,bool completion){
  auto&x=links[key];if(!x)x.reset(new Link(rate,C("hop_delay_ns")*1000,C("credit_return_ns")*1000,
    completion?C("completion_data_credit_bytes"):C("request_header_credits")*32,
    completion?C("completion_header_credits"):C("request_header_credits"),completion?C("data_credit_unit_bytes"):0,completion?C("packet_overhead_bytes"):0));
  return *x;
 }
 void Path(std::shared_ptr<std::vector<Link*>> path,size_t i,uint64_t bytes,Fn accepted,Accept end){
  if(i==path->size()){end(accepted);return;}
  (*path)[i]->Send(bytes,accepted,[this,path,i,bytes,end](Fn release){Path(path,i+1,bytes,release,end);});
 }
 void Transport(Descriptor&d,bool response,uint64_t n,Fn accepted,Accept end){
  auto path=std::make_shared<std::vector<Link*>>();uint32_t local=Socket(d.chip);
  auto endpoint=&GetLink("endpoint-"+std::to_string(d.chip)+(response?"-down":"-up"),C("endpoint_bytes_s"),response);
  auto uplink=[&](){return &GetLink("uplink-"+std::to_string(local)+(response?"-down":"-up"),C("socket_uplink_bytes_s"),response);};
  auto numa=[&](){auto key="numa-"+std::to_string(response?d.input.socket:local)+"-to-"+std::to_string(response?local:d.input.socket);
   auto&x=links[key];if(!x)x.reset(new Link(C("numa_link_bytes_s"),C("numa_delay_ns")*1000,C("credit_return_ns")*1000,C("completion_data_credit_bytes"),C("completion_header_credits")));return x.get();};
  if(response){if(local!=d.input.socket)path->push_back(numa());if(C("topology"))path->push_back(uplink());path->push_back(endpoint);}
  else{path->push_back(endpoint);if(C("topology"))path->push_back(uplink());if(local!=d.input.socket)path->push_back(numa());}
  Path(path,0,n,accepted,end);
 }
 void Memory(Descriptor&d,uint64_t address,uint64_t n,Fn accepted,Fn done){
  uint32_t socket=d.input.socket;auto&slot=ddrSlots[socket];
  if(!slot)slot.reset(new Buffer(C("ddr_max_inflight_per_socket"),C("ddr_max_inflight_per_socket")));
  slot->Push(1,[this,&d,address,n,socket,accepted,done](Fn release){
   accepted();After(C("ddr_read_latency_ns")*1000,[this,&d,address,n,socket,done,release](){
    uint64_t line=C("ddr_line_bytes"),first=address/line,last=(address+n-1)/line,finish=Now();
    std::vector<uint64_t> counts(C("channels_per_socket"));
    for(uint64_t a=first;a<=last;++a)counts[a%counts.size()]+=line;
    for(uint32_t ch=0;ch<counts.size();++ch)if(counts[ch]){
     auto&next=ddrNext[{socket,ch}];uint64_t start=std::max(Now(),next),duration=Serial(counts[ch],C("ddr_channel_bytes_s"));
     next=start+duration;finish=std::max(finish,next);auto&m=ddrMeters[socket];m.bytes+=counts[ch];m.busy+=duration;m.queueWait+=start-Now();++m.events;d.ddr+=counts[ch];
    }
    After(finish-Now(),[release,done](){release();done();});
   });
  });
 }
 void Split(Descriptor&d,uint64_t address,uint64_t n){
  while(n){uint64_t b=std::min({n,C("mrrs_bytes"),4096-address%4096});d.pieces.push_back({address,b});d.wire+=b;address+=b;n-=b;}
 }
 void Prepare(Descriptor&d){
  auto in=d.input;
  if(in.mode==0||in.mode==3)Split(d,in.address,d.useful);
  else if(in.mode==1){
   uint64_t offset=0;for(uint64_t row=0;offset<d.useful;++row){uint64_t n=std::min(in.row,d.useful-offset);Split(d,in.address+row*in.stride,n);offset+=n;}
  }else{
   // A page fetch is full-page traffic; repeated pages within one descriptor
   // coalesce. Cache is per Die, populated only after HBM writes complete.
   auto&lru=cache[{d.chip,d.die}];uint64_t page=C("page_bytes");
   for(uint64_t offset=0,row=0;offset<d.useful;++row){
    uint64_t n=std::min(in.row,d.useful-offset),a=in.address+row*in.stride;
    for(uint64_t p=a/page;p<=(a+n-1)/page;++p){
     if(d.pages.count(p))continue;d.pages.insert(p);auto it=std::find(lru.begin(),lru.end(),p);
     if(it!=lru.end()){++d.hits;lru.erase(it);lru.push_front(p);}else Split(d,p*page,page);
    }offset+=n;
   }
  }
  d.ready=true;Pump(d.chip);TryFinish(d);
 }
 void Pack(Descriptor&d){
  // Explicit gather reads, packing CPU service, writes of staging bytes,
  // followed by ordinary DMA reads of that buffer (three DDR passes).
  auto left=std::make_shared<uint64_t>((d.useful+d.input.row-1)/d.input.row);
  for(uint64_t row=0,offset=0;offset<d.useful;++row){uint64_t n=std::min(d.input.row,d.useful-offset);offset+=n;
   Memory(d,d.input.address+row*d.input.stride,n,[](){},[this,&d,left](){if(--*left)return;
    auto&next=packNext[d.input.socket];uint64_t start=std::max(Now(),next),duration=Serial(d.useful,C("packing_bytes_s"));next=start+duration;
    auto&m=packMeters[d.input.socket];m.bytes+=d.useful;m.busy+=duration;m.queueWait+=start-Now();++m.events;
    After(next-Now(),[this,&d](){Memory(d,d.input.address,d.useful,[](){},[this,&d](){Prepare(d);});});
   });
  }
 }
 void Admit(uint32_t chip){
  auto&x=chips[chip];
  while(!x.external.empty()&&x.resident<C("descriptor_queue_depth")){
   uint32_t id=x.external.front();x.external.pop_front();auto&d=desc.at(id);d.admit=Now();++x.resident;x.peakResident=std::max<uint64_t>(x.peakResident,x.resident);
   uint64_t begin=std::max(Now(),x.submitNext);x.submitNext=begin+C("descriptor_submit_ns")*1000;
   After(x.submitNext-Now(),[this,chip,id](){chips[chip].ready.push_back(id);Start(chip);});
  }
  if(!x.external.empty())++x.descriptorStalls;
 }
 void Start(uint32_t chip){auto&x=chips[chip];
  while(x.engines<C("engines_per_chip")&&!x.ready.empty()){
   uint32_t id=x.ready.front();x.ready.pop_front();++x.engines;x.active.push_back(id);auto&d=desc.at(id);d.start=Now();
   After(C("engine_start_ns")*1000,[this,id](){auto&d=desc.at(id);if(d.input.mode==3)Pack(d);else Prepare(d);});
  }
 }
 void Pump(uint32_t chip){auto&x=chips[chip];if(x.pumping)return;x.pumping=true;
  bool progress=true;while(progress){progress=false;auto active=x.active;
   if(!active.empty())std::rotate(active.begin(),active.begin()+(x.rr++%active.size()),active.end());
   for(auto id:active){auto&d=desc.at(id);if(!d.ready||d.cursor>=d.pieces.size())continue;
    auto p=d.pieces[d.cursor];
    if(x.tags>=C("tags_per_chip")){++d.tagStalls;continue;}
    if(x.inflight+p.bytes>C("max_inflight_bytes_per_chip")){++d.windowStalls;continue;}
    if(d.live>=C("prefetch_requests_per_descriptor")){++d.lookaheadStalls;continue;}
    ++d.cursor;++d.live;++d.requests;++x.tags;x.inflight+=p.bytes;
    x.peakTags=std::max<uint64_t>(x.peakTags,x.tags);x.peakInflight=std::max(x.peakInflight,x.inflight);progress=true;
    Transport(d,false,32,[](){},[this,id,p](Fn accepted){auto&d=desc.at(id);Memory(d,p.address,p.bytes,accepted,[this,id,p](){CompleteRead(id,p);});});
   }
  }x.pumping=false;
 }
 void CompleteRead(uint32_t id,Piece p){
  auto&d=desc.at(id);auto remaining=std::make_shared<uint64_t>(p.bytes);
  uint64_t address=p.address,left=p.bytes;
  while(left){uint64_t n=std::min(left,C("mps_bytes"));if(address%C("rcb_bytes"))n=std::min(n,C("rcb_bytes")-address%C("rcb_bytes"));
   address+=n;left-=n;++d.completions;
   Transport(d,true,n+C("packet_overhead_bytes"),[](){},[this,id,p,n,remaining](Fn release){
    auto&d=desc.at(id);auto&b=rx[d.chip];if(!b)b.reset(new Buffer(C("buffer_bytes_per_chip"),C("buffer_bytes_per_chip")));
    b->Push(n,[this,id,p,n,remaining,release](Fn free){
     release();auto&d=desc.at(id);d.received+=n;*remaining-=n;
     if(!*remaining){auto&x=chips[d.chip];assert(x.tags&&x.inflight>=p.bytes&&d.live);--x.tags;x.inflight-=p.bytes;--d.live;Pump(d.chip);}
     auto&m=hbmMeters[d.chip*4+d.die];m.bytes+=n;++m.events;uint64_t begin=Now();
     hbm(id,d.chip,d.die,n,[this,id,n,free,begin](uint64_t serviceStart){auto&d=desc.at(id);hbmMeters[d.chip*4+d.die].queueWait+=serviceStart-begin;hbmMeters[d.chip*4+d.die].busy+=Now()-serviceStart;d.written+=n;free();TryFinish(d);});
    });
   });
  }
 }
 void TryFinish(Descriptor&d){
  if(d.end||!d.ready||d.cursor!=d.pieces.size()||d.live||d.written!=d.wire)return;
  d.end=Now();if(d.input.mode==2){auto&lru=cache[{d.chip,d.die}];for(auto p:d.pages){lru.remove(p);lru.push_front(p);}while(lru.size()>C("page_cache_bytes_per_die")/C("page_bytes"))lru.pop_back();}
  auto&x=chips[d.chip];assert(x.engines&&x.resident);--x.engines;--x.resident;x.active.erase(std::remove(x.active.begin(),x.active.end(),d.id),x.active.end());
  d.done();Admit(d.chip);Start(d.chip);Pump(d.chip);
 }
public:
 bool Enabled()const{return enabled;}
 uint64_t HbmRate(){return C("hbm_bytes_s_per_die");}
 void Load(const std::string&dir,uint32_t n,Hbm callback){
  std::ifstream f(dir+"/pcie.tsv");if(!f)return;std::string key;uint64_t value;
  while(f>>key>>value){if(!c.emplace(key,value).second)throw std::runtime_error("duplicate PCIe key");}
  if(!f.eof())throw std::runtime_error("malformed PCIe config");
  std::ifstream r(dir+"/pcie-requests.tsv");uint32_t id;Input in;
  while(r>>id>>in.mode>>in.address>>in.row>>in.stride>>in.socket){if(!inputs.emplace(id,in).second)throw std::runtime_error("duplicate PCIe request");}
  if(!r.eof())throw std::runtime_error("malformed PCIe requests");
  chips.resize(n);hbm=callback;enabled=true;
 }
 void Submit(uint32_t id,uint32_t chip,uint32_t die,uint64_t bytes,Fn done){
  if(!enabled||!inputs.count(id)||desc.count(id))throw std::runtime_error("missing/duplicate transactional PCIe request");
  Descriptor d;d.id=id;d.chip=chip;d.die=die;d.useful=bytes;d.begin=Now();d.input=inputs.at(id);d.done=done;desc.emplace(id,std::move(d));chips[chip].external.push_back(id);Admit(chip);
 }
 bool Save(const std::string&out){if(!enabled)return true;bool ok=true;
  std::ofstream f(out+"/pcie_descriptors.csv");
  f<<"id,chip,die,memory_socket,mode,useful_bytes,pcie_payload_bytes,received_bytes,hbm_written_bytes,ddr_bytes,read_requests,completions,page_hits,begin_ps,admit_ps,engine_start_ps,end_ps,tag_stall_checks,inflight_stall_checks,lookahead_stall_checks\n";
  for(auto&kv:desc){auto&d=kv.second;ok=ok&&d.end&&d.wire==d.received&&d.wire==d.written;
   f<<d.id<<','<<d.chip<<','<<d.die<<','<<d.input.socket<<','<<d.input.mode<<','<<d.useful<<','<<d.wire<<','<<d.received<<','<<d.written<<','<<d.ddr<<','<<d.requests<<','<<d.completions<<','<<d.hits<<','<<d.begin<<','<<d.admit<<','<<d.start<<','<<d.end<<','<<d.tagStalls<<','<<d.windowStalls<<','<<d.lookaheadStalls<<'\n';}
  std::ofstream q(out+"/pcie_resources.csv");q<<"resource,bytes,wire_bytes,events,busy_ps,queue_wait_ps,admission_wait_ps,peak_bytes,peak_items,stall_checks,residual_bytes,residual_items\n";
  auto row=[&](std::string name,const Meter&m){q<<name<<','<<m.bytes<<','<<m.wire<<','<<m.events<<','<<m.busy<<','<<m.queueWait<<','<<m.admitWait<<','<<m.peak<<','<<m.peakItems<<','<<m.stalls<<','<<m.used<<','<<m.items<<'\n';};
  for(auto&kv:links)row(kv.first,kv.second->slots.m);
  for(auto&kv:rx)row("rx-"+std::to_string(kv.first),kv.second->m);
  for(auto&kv:ddrSlots)row("ddr-slots-"+std::to_string(kv.first),kv.second->m);
  for(auto&kv:ddrMeters)row("ddr-"+std::to_string(kv.first),kv.second);
  for(auto&kv:packMeters)row("pack-"+std::to_string(kv.first),kv.second);
  for(auto&kv:hbmMeters)row("hbm-write-"+std::to_string(kv.first),kv.second);
  std::ofstream s(out+"/pcie_chips.csv");s<<"chip,peak_tags,peak_inflight_bytes,peak_descriptors,descriptor_stall_checks,remaining_tags,remaining_inflight_bytes,remaining_descriptors\n";
  for(uint32_t i=0;i<chips.size();++i){auto&x=chips[i];ok=ok&&!x.tags&&!x.inflight&&!x.resident&&x.external.empty();s<<i<<','<<x.peakTags<<','<<x.peakInflight<<','<<x.peakResident<<','<<x.descriptorStalls<<','<<x.tags<<','<<x.inflight<<','<<x.resident<<'\n';}
  return ok&&desc.size()==inputs.size();
 }
};
} // namespace pcie

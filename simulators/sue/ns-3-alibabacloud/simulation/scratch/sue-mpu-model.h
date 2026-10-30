// Concurrent MPU access latency, strictly FIFO exclusive bandwidth service.
#pragma once
#include "ns3/core-module.h"
#include "sue-clock-model.h"
#include <algorithm>
#include <map>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <stdexcept>
namespace mpu {
using Done=std::function<void(uint64_t)>;
inline uint64_t Now(){return ns3::Simulator::Now().GetPicoSeconds();}
struct Request {uint32_t id,chip;uint64_t bytes,submitted,admitted=0,ready=0,start=0,end=0;Done done;long double left=0;bool began=false;};
struct Node {std::deque<uint32_t> waiting,active;bool busy=false;uint64_t peak=0;};
struct Cycle {uint32_t id,chip;uint64_t start,end;long double bytes;};
class Model {
 bool cycles=false;std::vector<Cycle> ledger;
 uint64_t rate=0,limit=0;std::map<uint32_t,uint64_t> latency;
 std::map<uint32_t,Request> requests;std::map<uint32_t,Node> nodes;
 void Fill(uint32_t chip){
  auto&n=nodes[chip];
  while(!n.waiting.empty()&&n.active.size()<limit){
   auto id=n.waiting.front();n.waiting.pop_front();auto&r=requests.at(id);
   r.admitted=Now();r.ready=r.admitted+latency.at(id)*1000;n.active.push_back(id);
   n.peak=std::max<uint64_t>(n.peak,n.active.size());
  }
  Pump(chip);
 }
 void Pump(uint32_t chip){
  auto&n=nodes[chip];if(n.busy||n.active.empty())return;n.busy=true;
  auto id=n.active.front();auto&r=requests.at(id);
  if(cycles){
   uint64_t start=std::max(Now(),r.ready);
   ns3::Simulator::Schedule(ns3::SuePicoSeconds(start-Now()+1000),[this,chip](){
    auto&n=nodes[chip];uint64_t begin=Now()-1000;long double budget=(long double)rate/1000000000.0L;
    std::vector<std::pair<Done,uint64_t>> completed;
    while(!n.active.empty()&&budget>1e-9L){
     auto&r=requests.at(n.active.front());if(r.ready>begin)break;
     if(!r.began){r.began=true;r.start=begin;}
     long double work=std::min(r.left,budget);r.left-=work;budget-=work;ledger.push_back({r.id,chip,begin,Now(),work});
     if(r.left>1e-9L)break;
     r.end=Now();n.active.pop_front();completed.push_back({r.done,r.admitted});
    }
    n.busy=false;Fill(chip);for(auto&done:completed)done.first(done.second);
   });return;
  }
  r.start=std::max(Now(),r.ready);
  // Whole-ns rounding preserves the original single-request service budget.
  uint64_t serial=(((__uint128_t)r.bytes*1000000000ULL+rate-1)/rate)*1000;
  ns3::Simulator::Schedule(ns3::SuePicoSeconds(r.start-Now()+serial),[this,chip,id](){
   auto&n=nodes[chip];auto&r=requests.at(id);r.end=Now();n.active.pop_front();n.busy=false;
   auto done=r.done;auto admitted=r.admitted;Fill(chip);done(admitted);
  });
 }
public:
 void Configure(uint64_t bandwidth,uint64_t slots,bool perCycle=false){rate=bandwidth;limit=slots;cycles=perCycle;}
 void Skip(uint32_t id){latency.erase(id);}
 void SubmitAxi(uint32_t id,uint32_t chip,uint64_t bytes,uint64_t ns,Done done){
  if(!latency.emplace(id,ns).second)throw std::runtime_error("duplicate AXI memory request");Submit(id,chip,bytes,done);
 }
 bool Enabled()const{return rate!=0;}
 bool Has(uint32_t id)const{return latency.count(id);}
 void Load(const std::string&dir){
  std::ifstream f(dir+"/mpu.tsv");if(!f)return;
  if(!(f>>rate>>limit)||!rate||!limit||limit>65536)throw std::runtime_error("invalid MPU configuration");
  uint32_t id;uint64_t ns;while(f>>id>>ns)if(!latency.emplace(id,ns).second)throw std::runtime_error("duplicate MPU request");
  if(!f.eof())throw std::runtime_error("invalid MPU latency mapping");
 }
 void Submit(uint32_t id,uint32_t chip,uint64_t bytes,Done done){
  if(!latency.count(id)||!requests.emplace(id,Request{id,chip,bytes,Now(),0,0,0,0,done}).second)throw std::runtime_error("invalid MPU submit");
  requests.at(id).left=bytes;nodes[chip].waiting.push_back(id);Fill(chip);
 }
 bool Save(const std::string&out,const std::string&prefix="mpu"){
  if(!Enabled())return true;bool ok=true;
  std::ofstream f(out+"/"+prefix+"_requests.csv");f<<"id,chip,bytes,submitted_ps,admitted_ps,latency_ready_ps,bandwidth_start_ps,end_ps\n";
  for(auto&kv:requests){auto&r=kv.second;ok=ok&&r.end>0;f<<r.id<<','<<r.chip<<','<<r.bytes<<','<<r.submitted<<','<<r.admitted<<','<<r.ready<<','<<r.start<<','<<r.end<<'\n';}
  std::ofstream g(out+"/"+prefix+"_nodes.csv");g<<"chip,bandwidth_bytes_s,max_inflight,peak_inflight,pending\n";
  for(auto&kv:nodes){auto&n=kv.second;auto pending=n.waiting.size()+n.active.size();ok=ok&&!pending;g<<kv.first<<','<<rate<<','<<limit<<','<<n.peak<<','<<pending<<'\n';}
  if(cycles){std::ofstream c(out+"/"+prefix+"_cycles.csv");c<<"id,chip,start_ps,end_ps,bytes\n"<<std::setprecision(18);
   for(auto&r:ledger)c<<r.id<<','<<r.chip<<','<<r.start<<','<<r.end<<','<<r.bytes<<'\n';}
  return ok&&requests.size()==latency.size();
 }
};
}

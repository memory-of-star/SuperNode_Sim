// Capped max-min sharing of a per-Die HBM byte budget. No DRAM PHY model.
#pragma once
#include "ns3/core-module.h"
#include "sue-clock-model.h"
#include <map>
#include <deque>
#include <fstream>
#include <functional>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iomanip>
namespace hbm {
using Done=std::function<void(uint64_t)>;
inline uint64_t Now(){return ns3::Simulator::Now().GetPicoSeconds();}
struct Job {uint64_t bytes,ready,first;long double left;Done done;};
struct Source {long double cap=0,rate=0,integral=0;uint64_t bytes=0,requests=0,completed=0,wait=0,active=0;std::deque<Job> jobs;};
struct Die {uint64_t last=0,busy=0;long double integral=0;std::map<uint32_t,Source> sources;ns3::EventId event;};
class Model {
 uint64_t bandwidth=0;std::map<uint32_t,uint64_t> caps;std::map<uint32_t,uint32_t> mapping;
 std::map<uint32_t,Die> dies;
 void Advance(Die&d){
  uint64_t now=Now(),dt=now-d.last;long double sum=0;bool busy=false;
  for(auto&kv:d.sources){auto&s=kv.second;if(s.jobs.empty())continue;busy=true;sum+=s.rate;
   long double work=std::min(s.jobs.front().left,s.rate*dt/1e12L);s.jobs.front().left-=work;s.integral+=work;d.integral+=work;s.active+=dt;
  }
  if(sum>bandwidth*(1+1e-12L))throw std::runtime_error("HBM capacity exceeded");
  if(busy)d.busy+=dt;d.last=now;
  for(auto&kv:d.sources){auto&s=kv.second;
   if(!s.jobs.empty()&&s.jobs.front().left<1e-6L){auto j=s.jobs.front();s.jobs.pop_front();s.bytes+=j.bytes;++s.completed;s.wait+=j.first-j.ready;
    ns3::Simulator::ScheduleNow([j](){j.done(j.first);});
    if(!s.jobs.empty())s.jobs.front().first=now;
   }
  }
 }
 void Schedule(uint32_t key){
  auto&d=dies[key];if(d.event.IsRunning())d.event.Cancel();
  std::vector<Source*> active;for(auto&kv:d.sources){kv.second.rate=0;if(!kv.second.jobs.empty())active.push_back(&kv.second);}
  if(active.empty())return;
  std::sort(active.begin(),active.end(),[](Source*a,Source*b){return a->cap<b->cap;});
  long double remaining=bandwidth;size_t count=active.size();
  for(auto*s:active){s->rate=std::min(s->cap,remaining/count);remaining-=s->rate;--count;}
  long double next=1e30L;
  for(auto*s:active){if(s->rate<=0||s->rate>s->cap*(1+1e-12L))throw std::runtime_error("HBM invalid allocation");next=std::min(next,s->jobs.front().left/s->rate*1e12L);}
  uint64_t delay=std::max<uint64_t>(1,uint64_t(std::ceil(next)));
  d.event=ns3::Simulator::Schedule(ns3::SuePicoSeconds(delay),[this,key](){Advance(dies[key]);Schedule(key);});
 }
public:
 bool Enabled()const{return bandwidth!=0;}
 uint64_t Rate()const{return bandwidth;}
 bool Has(uint32_t id)const{return mapping.count(id);}
 void Load(const std::string&dir){
  std::ifstream f(dir+"/hbm.tsv");if(!f)return;
  if(!(f>>bandwidth)||!bandwidth)throw std::runtime_error("invalid HBM bandwidth");
  uint32_t source;uint64_t cap;
  while(f>>source>>cap){if(!caps.emplace(source,cap?cap:bandwidth).second)throw std::runtime_error("duplicate HBM source");}
  if(!f.eof()||caps.empty())throw std::runtime_error("invalid HBM sources");
  std::ifstream r(dir+"/hbm-requests.tsv");uint32_t id;
  while(r>>id>>source){if(!caps.count(source)||!mapping.emplace(id,source).second)throw std::runtime_error("invalid HBM task source");}
  if(!r.eof())throw std::runtime_error("invalid HBM requests");
 }
 void Submit(uint32_t task,uint32_t chip,uint32_t die,uint64_t bytes,Done done){
  uint32_t key=chip*4+die;auto&d=dies[key];Advance(d);auto&s=d.sources[mapping.at(task)];s.cap=std::min<uint64_t>(bandwidth,caps.at(mapping.at(task)));
  bool empty=s.jobs.empty();s.jobs.push_back({bytes,Now(),empty?Now():0,(long double)bytes,done});++s.requests;Schedule(key);
 }
 bool Save(const std::string&out){
  if(!Enabled())return true;bool ok=true;
  std::ofstream f(out+"/hbm_sources.csv");f<<"chip,die,source,cap_bytes_s,bytes,requests,completed,pending,source_fifo_wait_ps,active_ps,integrated_bytes\n";f<<std::setprecision(18);
  std::ofstream g(out+"/hbm_dies.csv");g<<"chip,die,bandwidth_bytes_s,bytes,busy_ps,integrated_bytes\n";g<<std::setprecision(18);
  for(auto&kv:dies){auto&d=kv.second;Advance(d);uint64_t bytes=0;
   for(auto&sv:d.sources){auto&s=sv.second;ok=ok&&s.jobs.empty()&&s.requests==s.completed;bytes+=s.bytes;
    f<<kv.first/4<<","<<kv.first%4<<","<<sv.first<<","<<s.cap<<","<<s.bytes<<","<<s.requests<<","<<s.completed<<","<<s.jobs.size()<<","<<s.wait<<","<<s.active<<","<<s.integral<<"\n";}
   g<<kv.first/4<<","<<kv.first%4<<","<<bandwidth<<","<<bytes<<","<<d.busy<<","<<d.integral<<"\n";
  }return ok;
 }
};
}

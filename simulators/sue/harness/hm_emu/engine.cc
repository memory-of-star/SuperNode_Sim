// Full decoder scheduling: HM_SUE selects original SUE packet devices; otherwise message reference.
#include <ns3/simulator.h>
#include <ns3/nstime.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
using namespace ns3;
using Fn=std::function<void()>;
static int64_t now(){return Simulator::Now().GetPicoSeconds();}
static void later(int64_t ps, Fn fn){Simulator::Schedule(PicoSeconds(std::max<int64_t>(0,ps)),fn);}
struct Flow {int s,d; double rem,rate,bytes; int64_t start; Fn done;};
struct Network {
 int n; bool independent, switched; double bw; int64_t alpha,notify,last=0; EventId next;
 std::vector<Flow> flows; std::vector<double> tx,rx,busy; double service=0,slowdown=0,prop=0; uint64_t hops=0;
 Network(int n_,bool duplex,double b,int64_t a,int64_t nt,bool sw=false):n(n_),independent(duplex),switched(sw),bw(b/1e12),alpha(a),notify(nt),tx(n),rx(n),busy(n){}
 std::vector<int> resources(const Flow& f){
  if(switched) return {independent?2*f.s:f.s,independent?2*f.d+1:f.d,2*n+2*f.s,2*n+2*f.d+1};
  return {independent?2*f.s:f.s, independent?2*f.d+1:f.d,2*n+f.s*n+f.d};}
 void progress(){double dt=now()-last; for(auto& f:flows){double bytes=std::min(f.rem,dt*f.rate);f.rem-=bytes;tx[f.s]+=bytes;rx[f.d]+=bytes;busy[f.s]+=bytes/bw;busy[f.d]+=bytes/bw;}last=now();}
 void rebalance(){
  if(next.IsRunning()) Simulator::Cancel(next); if(flows.empty())return;
  std::map<int,double> cap; std::vector<bool> open(flows.size(),true);
  for(auto& f:flows){f.rate=0;for(int r:resources(f))cap[r]=bw;}
  for(size_t round=0;round<=flows.size();round++){
   std::map<int,int> users; int left=0;
   for(size_t i=0;i<flows.size();i++)if(open[i]){left++;for(int r:resources(flows[i]))users[r]++;}
   if(!left)break;
   double delta=1e99;for(auto [r,c]:users)delta=std::min(delta,std::max(0.,cap[r])/c);
   for(size_t i=0;i<flows.size();i++)if(open[i]){flows[i].rate+=delta;for(int r:resources(flows[i]))cap[r]-=delta;}
   for(size_t i=0;i<flows.size();i++)if(open[i])for(int r:resources(flows[i]))if(cap[r]<1e-12)open[i]=false;
  }
  double dt=1e99;for(auto& f:flows){assert(f.rate>0);dt=std::min(dt,f.rem/f.rate);}
  next=Simulator::Schedule(PicoSeconds(std::max<int64_t>(1,std::ceil(dt))),&Network::finish,this);
 }
 void finish(){progress();std::vector<Fn> done;
  for(auto it=flows.begin();it!=flows.end();){if(it->rem<1e-6){
    double elapsed=now()-it->start;service+=it->bytes/bw;slowdown+=std::max(0.,elapsed-it->bytes/bw);prop+=alpha;
    done.push_back(it->done);it=flows.erase(it);
   }else ++it;}
  rebalance();for(auto& cb:done)later(alpha+notify,cb);
 }
 void hop(int s,int d,double bytes,Fn cb){progress();flows.push_back({s,d,bytes,0,bytes,now(),cb});hops++;rebalance();}
 void send(int s,int d,double bytes,Fn cb){
  if(s==d){later(0,cb);return;}
  if(switched){hop(s,d,bytes,cb);return;} // Nonblocking switch: source ingress + destination egress, no chip relay.
  int cw=(d-s+n)%n,ccw=(s-d+n)%n;int to=(s+(cw<=ccw?1:n-1))%n;
  hop(s,to,bytes,[=](){if(to==d)cb();else send(to,d,bytes,cb);});
 }
};
#ifdef HM_SUE
#include "sue_backend.h"
using EngineNetwork=SuePacketNetwork;
#else
using EngineNetwork=Network;
#endif
struct Job{int step,group;int64_t ready;};
int main(int argc,char**argv){
 if(argc!=3){std::cerr<<"engine input.txt output_prefix\n";return 2;}
 Time::SetResolution(Time::PS);std::ifstream f(argv[1]);std::string mode,arrfile;int n,B,m,steps,segments,duplex;double bw;int64_t alpha,notify;
 f>>mode>>n>>B>>m>>steps>>segments>>duplex>>bw>>alpha>>notify>>arrfile;
 if(!f){std::cerr<<"invalid header";return 2;}
 int groups=B/m;std::vector<double> payload(segments);for(auto& x:payload)f>>x;
 size_t count=size_t(steps)*groups*segments;std::vector<double> costs(count);
 std::ifstream bin(arrfile,std::ios::binary);bin.read((char*)costs.data(),count*sizeof(double));if(!bin){std::cerr<<"invalid service array";return 2;}
 std::string topology="ring";f>>topology;
 if(topology!="ring" && topology!="single_switch"){std::cerr<<"unknown topology";return 2;}
 EngineNetwork net(n,duplex,bw,alpha,notify,topology=="single_switch");
 std::vector<std::deque<Job>> queues(n);std::vector<bool> running(n,false);
 std::vector<double> stagebusy(n),stagewait(n);std::vector<int64_t> prev(groups,0);std::vector<double> intervals,requesttotal(groups),first(groups);
 std::ofstream tokens(std::string(argv[2])+"-tokens.csv");tokens<<"group,step,completed_ps,interval_ps\n";
 uint64_t finished=0,ar_steps=0;int64_t end=0;
 auto complete=[&](int g,int st){int64_t t=now();if(st>0)intervals.push_back(t-prev[g]);else first[g]=t;requesttotal[g]+=t-prev[g];tokens<<g<<','<<st<<','<<t<<','<<t-prev[g]<<'\n';prev[g]=t;end=t;finished+=m;};
 std::function<void(int)> start;
 std::function<void(int,Job)> enqueue=[&](int s,Job j){j.ready=now();queues[s].push_back(j);start(s);};
 start=[&](int s){if(running[s]||queues[s].empty())return;Job j=queues[s].front();queues[s].pop_front();running[s]=true;
  stagewait[s]+=now()-j.ready;double cost=costs[(size_t(j.step)*groups+j.group)*segments+s];stagebusy[s]+=cost;
  later((int64_t)std::ceil(cost),[&,s,j](){running[s]=false;start(s);
   if(s+1<n)net.send(s,s+1,payload[s],[&,s,j](){enqueue(s+1,j);});
   else {complete(j.group,j.step);if(j.step+1<steps)net.send(n-1,0,payload[s],[&,j](){enqueue(0,{j.step+1,j.group,0});});}
  });
 };
 std::function<void(int,int)> tp;
 std::function<void(int,int,int)> ar;
 ar=[&](int st,int seg,int k){
  if(k==2*(n-1)){tp(st,seg+1);return;}
  auto left=std::make_shared<int>(n);ar_steps++;
  for(int r=0;r<n;r++)net.send(r,(r+1)%n,payload[seg]/n,[&,st,seg,k,left](){if(--*left==0)ar(st,seg,k+1);});
 };
 tp=[&](int st,int seg){
  if(seg==segments){complete(0,st);if(st+1<steps)tp(st+1,0);return;}
  double cost=costs[size_t(st)*segments+seg];for(auto& b:stagebusy)b+=cost;
  later((int64_t)std::ceil(cost),[&,st,seg](){if(payload[seg]>0)ar(st,seg,0);else tp(st,seg+1);});
 };
 if(mode=="TP") {assert(groups==1);tp(0,0);}else for(int g=0;g<groups;g++)enqueue(0,{0,g,0});
 Simulator::Run();assert(finished==uint64_t(B)*steps);assert(net.flows.empty());
 std::sort(intervals.begin(),intervals.end());auto pct=[&](double q){return intervals.empty()?0:intervals[std::min(intervals.size()-1,size_t(q*(intervals.size()-1)))];};
 double mean=std::accumulate(intervals.begin(),intervals.end(),0.)/std::max<size_t>(1,intervals.size());
 #ifdef HM_SUE
 net.output(argv[2]);
#endif
 std::ofstream out(std::string(argv[2])+".json");out<<std::setprecision(17)<<"{\"finished_tokens\":"<<finished<<",\"makespan_ps\":"<<end<<",\"mean_tpot_ps\":"<<mean<<",\"p50_tpot_ps\":"<<pct(.5)<<",\"p95_tpot_ps\":"<<pct(.95)<<",\"mean_first_decode_ps\":"<<std::accumulate(first.begin(),first.end(),0.)/groups<<",\"ar_steps\":"<<ar_steps<<",\"hop_messages\":"<<net.hops<<",\"network_serial_ps_sum\":"<<net.service<<",\"network_competition_ps_sum\":"<<net.slowdown<<",\"network_propagation_ps_sum\":"<<net.prop;
 auto array=[&](std::string key,std::vector<double> v){out<<",\""<<key<<"\":[";for(size_t i=0;i<v.size();i++){if(i)out<<',';out<<v[i];}out<<']';};
 array("chip_tx_bytes",net.tx);array("chip_rx_bytes",net.rx);array("stage_busy_ps",stagebusy);array("stage_queue_ps",stagewait);array("network_budget_busy_ps",net.busy);
 out<<",\"topology\":\""<<topology<<"\",\"switch_forwarded_messages\":"<<(net.switched?net.hops:0)<<"}\n";Simulator::Destroy();return 0;
}

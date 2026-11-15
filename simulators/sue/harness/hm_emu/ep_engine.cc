#define HM_SUE
#define main hm_reference_entry
#include "engine.cc"
#undef main
#include <sstream>
struct DagTask{char kind;int src,dst,step;int64_t ps;uint64_t bytes;std::vector<int> children;int waiting=0;std::string phase;int64_t begin=0,end=0;};
int main(int argc,char**argv){
 if(argc!=3)return 2;Time::SetResolution(Time::PS);
 std::ifstream input(argv[1]);std::string mode,serviceFile,topology,graphFile;int n,B,m,steps,segments,duplex;double bw;int64_t alpha,notify;double ignored;
 input>>mode>>n>>B>>m>>steps>>segments>>duplex>>bw>>alpha>>notify>>serviceFile>>ignored>>topology>>graphFile;
 NS_ABORT_MSG_IF(mode!="EP"||n!=16||m!=B||!input,"bad EP input");
 SuePacketNetwork net(n,duplex,bw,alpha,notify,true);
 std::ifstream graph(graphFile);std::vector<DagTask> tasks;std::string line;
 while(std::getline(graph,line)){
  if(line.empty())continue;std::istringstream s(line);int id;DagTask t;std::string deps;
  s>>id>>t.kind>>t.src>>t.dst>>t.step>>t.ps>>t.bytes>>t.phase>>deps;
  NS_ABORT_MSG_IF(!s||id!=int(tasks.size()),"invalid DAG row");tasks.push_back(t);
  if(deps!="-"){std::replace(deps.begin(),deps.end(),',',' ');std::istringstream ds(deps);int d;while(ds>>d){NS_ABORT_MSG_IF(d<0||d>=id,"non-topological dependency");tasks[d].children.push_back(id);tasks[id].waiting++;}}
 }
 std::ofstream token(std::string(argv[2])+"-tokens.csv"),trace(std::string(argv[2])+"-ep-tasks.csv");
 token<<"group,step,completed_ps,interval_ps\n";trace<<"id,kind,step,phase,start_ps,end_ps\n";
 uint64_t finished=0,doneCount=0;int64_t end=0,previous=0;std::vector<double> intervals,stagebusy(n),zeros(n);double first=0;
 std::function<void(int)> launch,done;
 done=[&](int id){auto&t=tasks[id];t.end=now();doneCount++;trace<<id<<','<<t.kind<<','<<t.step<<','<<t.phase<<','<<t.begin<<','<<t.end<<'\n';
  if(t.kind=='T'){auto dt=now()-previous;token<<0<<','<<t.step<<','<<now()<<','<<dt<<'\n';if(t.step)intervals.push_back(dt);else first=dt;previous=end=now();finished+=B;}
  for(int child:t.children)if(--tasks[child].waiting==0)later(0,[&,child](){launch(child);});
 };
 launch=[&](int id){auto&t=tasks[id];t.begin=now();
  if(t.kind=='N'){net.send(t.src,t.dst,t.bytes,[&,id](){done(id);});}
  else{if(t.kind=='C')for(int chip=t.src*8;chip<(t.src+1)*8;chip++)stagebusy[chip]+=t.ps;later(t.ps,[&,id](){done(id);});}
 };
 for(int id=0;id<int(tasks.size());id++)if(!tasks[id].waiting)later(0,[&,id](){launch(id);});
 Simulator::Run();NS_ABORT_MSG_IF(doneCount!=tasks.size()||finished!=uint64_t(B)*steps,"incomplete EP DAG");net.output(argv[2]);
 std::sort(intervals.begin(),intervals.end());double mean=std::accumulate(intervals.begin(),intervals.end(),0.)/intervals.size();
 std::ofstream out(std::string(argv[2])+".json");out<<std::setprecision(17)<<"{\"finished_tokens\":"<<finished<<",\"makespan_ps\":"<<end<<",\"mean_tpot_ps\":"<<mean<<",\"p50_tpot_ps\":"<<intervals[(intervals.size()-1)/2]<<",\"p95_tpot_ps\":"<<intervals[size_t(.95*(intervals.size()-1))]<<",\"mean_first_decode_ps\":"<<first<<",\"ar_steps\":0,\"hop_messages\":"<<net.hops<<",\"network_serial_ps_sum\":"<<net.service<<",\"network_competition_ps_sum\":"<<net.slowdown<<",\"network_propagation_ps_sum\":"<<net.prop;
 auto arr=[&](std::string key,const std::vector<double>&v){out<<",\""<<key<<"\":[";for(size_t i=0;i<v.size();i++){if(i)out<<',';out<<v[i];}out<<']';};
 arr("chip_tx_bytes",net.tx);arr("chip_rx_bytes",net.rx);arr("stage_busy_ps",stagebusy);arr("stage_queue_ps",zeros);arr("network_budget_busy_ps",net.busy);
 out<<",\"topology\":\"single_switch\",\"switch_forwarded_messages\":"<<net.hops<<",\"task_count\":"<<tasks.size()<<"}\n";
 Simulator::Destroy();return 0;
}

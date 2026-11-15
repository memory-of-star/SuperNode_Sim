#include <ns3/simulator.h>
#include <ns3/nstime.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>
using namespace ns3;using Fn=std::function<void()>;
static int64_t now(){return Simulator::Now().GetPicoSeconds();}
static void later(int64_t ps,Fn f){Simulator::Schedule(PicoSeconds(ps),f);}
struct EngineNetwork{int n=8;int sends=0;double bytes=0;bool slow=false;int64_t unaffected=-1;void send(int s,int d,double b,Fn fn){sends++;bytes+=b;if(s==2&&d==0)unaffected=now();later(2000+(slow&&s==7&&d==3?10000:0),fn);}};
#include "tree_collective.h"
int main(int argc,char**argv){assert(argc==2);Time::SetResolution(Time::PS);std::ofstream trace(argv[1]);
 {LocalService l(0,&trace);int64_t a=0,b=0;l.submit(100,0,0,[&](){a=now();});l.submit(300,0,1,[&](){b=now();});Simulator::Run();assert(a==200&&b==400&&l.busy==400);Simulator::Destroy();}
 for(bool slow:{false,true}){EngineNetwork net;net.slow=slow;std::vector<std::unique_ptr<LocalService>> local;for(int r=0;r<8;r++)local.emplace_back(new LocalService(r,&trace));TreeCollective t(net,local,trace,1e30,1e30);bool done=false;int64_t end=0;t.start(0,0,1024,[&](){done=true;end=now();});Simulator::Run();assert(done&&net.sends==14&&net.bytes==14336);assert(net.unaffected==2001);assert(end==(slow?22003:12003));assert(t.calls==1&&t.edges==14);Simulator::Destroy();}
 // Two simultaneous collectives really overlap, but local reductions share service.
 {EngineNetwork net;std::vector<std::unique_ptr<LocalService>> local;for(int r=0;r<8;r++)local.emplace_back(new LocalService(r,&trace));TreeCollective t(net,local,trace,1e30,1e30);int done=0;t.start(0,0,1024,[&](){done++;});t.start(0,1,1024,[&](){done++;});Simulator::Run();assert(done==2&&net.sends==28&&now()==12006);Simulator::Destroy();}
}

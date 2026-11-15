#define main hm_emu_entry
#include "engine.cc"
#undef main
int main(){
 Time::SetResolution(Time::PS);
 // Nonadjacent transfers are store-and-forward, including per-hop card budgets.
 Network net(4,false,128e9,2000000,1000);int64_t end=0;
 net.send(0,2,128000,[&](){end=now();});Simulator::Run();
 assert(end==2*(1000000+2000000+1000));assert(net.hops==2);
 assert(net.tx[0]==128000&&net.tx[1]==128000&&net.rx[2]==128000);
 Simulator::Destroy();
 // Both neighbours share the card budget: two 128 kB sends need 2 us service.
 Network shared(4,false,128e9,2000000,1000);int count=0;end=0;
 shared.send(0,1,128000,[&](){count++;end=now();});
 shared.send(0,3,128000,[&](){count++;end=now();});Simulator::Run();
 assert(count==2&&end==4001000);assert(shared.tx[0]==256000);
 Simulator::Destroy();
 // Logical non-neighbours communicate through one switch without intermediate chips.
 Network sw(4,false,128e9,2000000,1000,true);end=0;
 sw.send(0,2,128000,[&](){end=now();});Simulator::Run();
 assert(end==3001000&&sw.hops==1);assert(sw.tx[1]==0&&sw.rx[1]==0);
 Simulator::Destroy();
 // Incast shares destination egress/card budget; disjoint transfers do not.
 Network incast(4,false,128e9,2000000,1000,true);end=0;count=0;
 incast.send(0,2,128000,[&](){end=now();count++;});
 incast.send(1,2,128000,[&](){end=now();count++;});Simulator::Run();
 assert(count==2&&end==4001000);assert(incast.rx[2]==256000);
 Simulator::Destroy();
 Network disjoint(4,false,128e9,2000000,1000,true);end=0;count=0;
 disjoint.send(0,2,128000,[&](){end=now();count++;});
 disjoint.send(1,3,128000,[&](){end=now();count++;});Simulator::Run();
 assert(count==2&&end==3001000);
 Simulator::Destroy();std::cout<<"multi-hop and shared-neighbour budgets PASS\n";return 0;
}

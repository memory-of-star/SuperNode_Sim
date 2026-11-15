#define HM_SUE
#define main hm_emu_entry
#include "engine.cc"
#undef main
int main(){
 Time::SetResolution(Time::PS);
 SuePacketNetwork sw(4,false,128e9,2000000,1000,true);bool done=false;
 sw.send(0,2,65536,[&](){done=true;});Simulator::Run();
 assert(done&&sw.tx[0]==65536&&sw.rx[2]==65536);
 assert(sw.tx[1]==0&&sw.rx[1]==0&&sw.tx[3]==0&&sw.rx[3]==0);
 sw.output("/tmp/hm-sue-star-test");Simulator::Destroy();
 std::cout<<"SUE non-neighbour delivery via switch, no chip relay: PASS\n";return 0;
}

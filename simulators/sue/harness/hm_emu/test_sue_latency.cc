#define HM_SUE
#define main hm_emu_entry
#include "engine.cc"
#undef main
// A settled, isolated one-frame stripe: ingress serialization + fixed budget
// + one injection notification + one completion notification. The switch budget
// already includes egress serialization, exactly as in the original SUE model.
int main(){
 Time::SetResolution(Time::PS);
 for(int64_t target:{2000000LL,1000000LL})for(bool duplex:{false,true})for(int size:{64,256,2048}){
  SuePacketNetwork net(2,duplex,128e9,target,1000,true);bool done=false;int64_t begin=0,elapsed=0;
  later(100000000,[&](){begin=now();net.send(0,1,size*4,[&](){done=true;elapsed=now()-begin;});});
  Simulator::Run();
  uint64_t rate=uint64_t(128e9*8/(duplex?4:8));
  auto wire=DataRate(rate).CalculateBytesTxTime(size+17+8).GetPicoSeconds();
  int64_t expected=target+wire+2000;
  if(!done||elapsed!=expected){std::cerr<<size<<" measured="<<elapsed<<" expected="<<expected<<"\n";return 1;}
  std::cout<<"target_ps="<<target<<" payload_per_die="<<size<<" direction="<<(duplex?128:64)<<"GB/s actual_ps="<<elapsed<<" PASS\n";
  Simulator::Destroy();
 }
}

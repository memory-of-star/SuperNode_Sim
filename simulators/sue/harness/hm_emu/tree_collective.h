// Tree AllReduce and bounded local service for hm_emu; no SUE protocol changes.
// Equal-rate processor sharing of measured/modelled local service work.
struct LocalService {
 struct Work { double remaining; int64_t start; int step,seg; Fn done; };
 std::vector<Work> jobs; EventId event; int64_t last=0; int rank;
 std::ofstream* trace; double busy=0;
 LocalService(int r,std::ofstream*t):rank(r),trace(t){}
 void advance(){if(!jobs.empty()){double dt=double(now()-last)/jobs.size();for(auto&j:jobs)j.remaining-=dt;}last=now();}
 void arm(){if(event.IsRunning())Simulator::Cancel(event);if(jobs.empty())return;double w=1e99;for(auto&j:jobs)w=std::min(w,j.remaining);event=Simulator::Schedule(PicoSeconds(std::max<int64_t>(1,std::ceil(w*jobs.size()))),&LocalService::finish,this);}
 void submit(double ps,int step,int seg,Fn fn){advance();busy+=ps;jobs.push_back({ps,now(),step,seg,fn});arm();}
 void finish(){advance();std::vector<Fn> done;for(auto it=jobs.begin();it!=jobs.end();){if(it->remaining<=1e-5){*trace<<"local,"<<it->step<<','<<it->seg<<','<<rank<<','<<it->start<<','<<now()<<'\n';done.push_back(it->done);it=jobs.erase(it);}else ++it;}arm();for(auto&f:done)f();}
};
struct TreeCollective {
 EngineNetwork& net; std::vector<std::unique_ptr<LocalService>>& local; std::ofstream& trace;
 double hbmBytesPerPs,reduceElementsPerPs; uint64_t calls=0,edges=0;
 struct Op { int step,seg;double bytes;int64_t start;Fn done;std::vector<int> waiting;int finished=0; };
 TreeCollective(EngineNetwork&n,std::vector<std::unique_ptr<LocalService>>&l,std::ofstream&t,double b,double f):net(n),local(l),trace(t),hbmBytesPerPs(b),reduceElementsPerPs(f){}
 int children(int r){return int(2*r+1<net.n)+int(2*r+2<net.n);}
 void send(int s,int d,const std::shared_ptr<Op>&o,Fn fn){edges++;net.send(s,d,o->bytes,std::move(fn));}
 void down(int r,const std::shared_ptr<Op>&o){
  for(int c=2*r+1;c<=2*r+2 && c<net.n;c++)send(r,c,o,[this,c,o](){down(c,o);});
  if(++o->finished==net.n){trace<<"collective,"<<o->step<<','<<o->seg<<",-1,"<<o->start<<','<<now()<<'\n';o->done();}
 }
 void up(int r,const std::shared_ptr<Op>&o){
  auto forward=[this,r,o](){if(r==0)down(0,o);else{int parent=(r-1)/2;send(r,parent,o,[this,parent,o](){if(--o->waiting[parent]==0)up(parent,o);});}};
  int c=children(r);if(!c){forward();return;}
  // Each received vector: two reads + one write, BF16 elementwise addition.
  double perDie=o->bytes/4;double ps=std::max(3*c*perDie/hbmBytesPerPs,c*perDie/2/reduceElementsPerPs);
  local[r]->submit(std::ceil(ps),o->step,o->seg,forward);
 }
 void start(int step,int seg,double bytes,Fn done){
  calls++;auto o=std::make_shared<Op>();o->step=step;o->seg=seg;o->bytes=bytes;o->done=done;o->start=now();o->waiting.resize(net.n);
  for(int r=0;r<net.n;r++)o->waiting[r]=children(r);
  for(int r=0;r<net.n;r++)if(!o->waiting[r])up(r,o);
 }
};

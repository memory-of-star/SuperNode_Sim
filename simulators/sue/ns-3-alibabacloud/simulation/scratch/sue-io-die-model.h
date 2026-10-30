// Included by the external adapter after task routing declarations. No protocol core changes.
// Packet tags carry simulator identity only; modeled metadata bytes are charged to wire payload.
struct IoSpec {uint32_t kind,count;int parent;};
struct IoRecord {uint32_t task,seq,kind,sc,sd,dc,dd,port,proxy,vc,logical;int64_t ready,received=-1;};
struct IoPacket {std::vector<uint32_t> records;uint32_t bytes,unpadded,mac;int64_t sent;};
static bool ioEnabled=false,ioPack=false;
static bool ioEventDriven=true;
static uint32_t ioCap,ioInterval,ioLimit,ioHeader,ioMinMac;static std::array<uint32_t,4> ioSizes;
static std::map<uint32_t,IoSpec> ioSpecs;
struct IoWire {int64_t packet,time;uint32_t port,side,bytes;int64_t busy;};
static std::vector<IoWire> ioWires;
static void IoWireTrace(Ptr<const Packet> p,Ptr<NetDevice> src,Time tx){
 if(!ioEnabled)return;auto loc=wirePorts.at(src);V3Tag tag;int64_t id=-1;
 if(p->PeekPacketTag(tag)&&tag.reply==2)id=tag.id;
 ioWires.push_back({id,Simulator::Now().GetPicoSeconds(),loc.first,uint32_t(loc.second),p->GetSize(),tx.GetPicoSeconds()});
}
static std::vector<IoRecord> ioRecords;static std::vector<IoPacket> ioPackets;
using IoPort=std::tuple<uint32_t,uint32_t,uint32_t>;
static std::map<IoPort,std::deque<uint32_t>> ioQueues;
static std::set<IoPort> ioArmed;
// Only blocked producers are parked. Keep their original retry-clock phase;
// one wake event per port replaces independent periodic failed attempts.
struct IoWaiter {uint64_t eligible;Fn retry;};
struct IoWaitQueue {std::map<uint64_t,std::deque<IoWaiter>> phases;EventId event;bool armed=false;uint64_t due=0;};
static std::map<IoPort,IoWaitQueue> ioWaiters;
static std::map<IoPort,uint64_t> ioEgressWaiting;
static uint64_t ioParked=0,ioResumed=0,ioWakeEvents=0,ioPending=0,ioPeakPending=0,ioEgressChecks=0,ioEgressBlocks=0,ioEgressNotifications=0;
static std::map<uint32_t,uint32_t> ioIssued,ioReceived,ioResponses;
struct IoRead {uint32_t request,service,response,count,completed=0;};
static mpu::Model ioMemory;
static uint64_t ioMemoryLatency=0;
static std::map<uint32_t,IoRead> ioReads;
static std::set<uint32_t> ioMemoryWrites;
static std::set<std::pair<uint32_t,uint32_t>> ioManagedEdges;
static void IoMemorySubmit(uint32_t record);
static bool IoManagedEdge(uint32_t child,uint32_t parent){return ioManagedEdges.count({child,parent});}
static void IoFlush(IoPort key);
static void IoArm(IoPort key,Time wait){if(ioArmed.insert(key).second)Later(wait,[key](){ioArmed.erase(key);IoFlush(key);});}
static bool IoEnqueue(IoRecord r){
 IoPort key={r.sc,r.sd,r.port};auto&q=ioQueues[key];if(q.size()>=ioLimit)return false;
 r.ready=Simulator::Now().GetPicoSeconds();q.push_back(ioRecords.size());ioRecords.push_back(r);
 if(!ioEgressWaiting.count(key))IoArm(key,SuePicoSeconds(0));return true;
}
static uint64_t IoRetryAt(uint64_t eligible,uint64_t now){
 uint64_t period=uint64_t(interval)*1000;
 return now<=eligible?eligible:eligible+((now-eligible+period-1)/period)*period;
}
static void IoWakeArm(IoPort key);
static void IoWake(IoPort key){
 auto&w=ioWaiters.at(key);w.armed=false;++ioWakeEvents;
 uint64_t now=Simulator::Now().GetPicoSeconds(),phase=now%(uint64_t(interval)*1000);
 auto bucket=w.phases.find(phase);
 while(bucket!=w.phases.end()&&!bucket->second.empty()&&bucket->second.front().eligible<=now&&ioQueues[key].size()<ioLimit){
  auto retry=std::move(bucket->second.front().retry);bucket->second.pop_front();--ioPending;++ioResumed;retry();
 }
 if(bucket!=w.phases.end()&&bucket->second.empty())w.phases.erase(bucket);
 IoWakeArm(key);
}
static void IoWakeArm(IoPort key){
 auto found=ioWaiters.find(key);if(found==ioWaiters.end()||ioQueues[key].size()>=ioLimit)return;
 auto&w=found->second;if(w.phases.empty())return;
 uint64_t now=Simulator::Now().GetPicoSeconds(),due=UINT64_MAX;
 for(auto&b:w.phases)due=std::min(due,IoRetryAt(b.second.front().eligible,now));
 if(w.armed&&w.due<=due)return;
 if(w.armed)Simulator::Cancel(w.event);
 w.armed=true;w.due=due;w.event=Simulator::Schedule(SuePicoSeconds(due-now),[key](){IoWake(key);});
}
static void IoPark(const IoRecord&r,Fn retry){
 IoPort key={r.sc,r.sd,r.port};uint64_t eligible=Simulator::Now().GetPicoSeconds()+uint64_t(interval)*1000;
 ioWaiters[key].phases[eligible%(uint64_t(interval)*1000)].push_back({eligible,std::move(retry)});
 ++ioParked;++ioPending;ioPeakPending=std::max(ioPeakPending,ioPending);IoWakeArm(key);
}
static void IoEgressFreed(IoPort key){
 auto it=ioEgressWaiting.find(key);if(it==ioEgressWaiting.end())return;
 ++ioEgressNotifications;uint64_t now=Simulator::Now().GetPicoSeconds();
 IoArm(key,SuePicoSeconds(IoRetryAt(it->second,now)-now));
}
static bool IoMatch(const IoRecord&a,const IoRecord&b){return a.sc==b.sc&&a.sd==b.sd&&a.port==b.port&&a.dc==b.dc&&a.dd==b.dd&&a.proxy==b.proxy&&a.vc==b.vc&&a.kind==b.kind;}
static void IoFlush(IoPort key){
 auto&q=ioQueues[key];if(q.empty())return;
 const auto first=ioRecords[q.front()];std::vector<uint32_t> selected;uint32_t bytes=ioHeader;
 for(auto id:q){auto&r=ioRecords[id];if(!IoMatch(first,r))continue;if(bytes+ioSizes[r.kind]>ioCap)break;
  selected.push_back(id);bytes+=ioSizes[r.kind];if(!ioPack)break;}
 SueHeader h;h.SetSrc(SaMac(first.sc,first.sd));h.SetDst(SaMac(first.dc,first.dd));h.SetEtherType(SueConfig::EthType1());h.SetVlan(false,0,false,0);
 SueShimHeader shim;shim.f=true;shim.ttl=2;shim.dscp=first.vc;shim.flowLabel=first.task&255;h.SetShim(shim);
 uint32_t unpadded=bytes;bytes=std::max(bytes,ioMinMac>h.GetSerializedSize()?ioMinMac-h.GetSerializedSize():0u);
 auto packet=Create<Packet>(bytes);packet->AddHeader(h);auto dev=P(first.sc,first.sd,first.port).host;
 ++ioEgressChecks;
 if(!dev->EgressCanAccept(first.vc,packet->GetSize())){
  ++tasks[first.task].blocked;++ioEgressBlocks;
  if(ioEventDriven)ioEgressWaiting.emplace(key,Simulator::Now().GetPicoSeconds()+uint64_t(interval)*1000);
  else IoArm(key,NanoSeconds(interval));return;
 }
 ioEgressWaiting.erase(key);
 V3Tag tag;tag.reply=2;tag.id=ioPackets.size();tag.bytes=bytes;packet->AddPacketTag(tag);
 ioPackets.push_back({selected,bytes,unpadded,packet->GetSize(),Simulator::Now().GetPicoSeconds()});
 std::set<uint32_t> ids(selected.begin(),selected.end());q.erase(std::remove_if(q.begin(),q.end(),[&](uint32_t i){return ids.count(i);}),q.end());
 // One packet formation opportunity per endpoint wire slot; no hold-to-fill timer.
 // Actual SUE arbitration/control frames/credits can add delay downstream.
 uint64_t bps=dev->GetDataRate().GetBitRate();
 auto ns=(uint64_t(packet->GetSize()+8)*8000000000ULL+bps-1)/bps;
 dev->SwitchSend(first.vc,packet);
 // Keep the opportunity reserved even when q empties; subsequent arrivals cannot bypass it.
 IoArm(key,NanoSeconds(std::max<uint64_t>(1,ns)));
 if(ioEventDriven)IoWakeArm(key);
}
static void IoReply(uint32_t record){
 const auto r=ioRecords.at(record);
 IoRecord b{r.task,r.seq,2,r.dc,r.proxy,r.sc,r.sd,r.port,r.sd,r.vc,0,0};
 if(!IoEnqueue(b)){
  if(ioEventDriven)IoPark(b,[record](){IoReply(record);});
  else Later(NanoSeconds(interval),[record](){IoReply(record);});
 }
}
static void IoDelivered(uint32_t record){
 auto&r=ioRecords.at(record);auto&t=tasks[r.task];if(r.received>=0){++t.duplicates;return;}
 r.received=Simulator::Now().GetPicoSeconds();uint32_t task=r.task,kind=r.kind;
 if(kind==2){if(++ioResponses[task]==ioSpecs.at(task).count)Complete(task);return;}
 t.received+=r.logical;if(++ioReceived[task]==ioSpecs.at(task).count){
  if(t.received!=t.bytes)throw std::runtime_error("IO logical conservation");
  t.dataEnd=Simulator::Now().GetPicoSeconds();Notify(t.receiveNext);
  if(kind!=0)Complete(task);
 }
 if(kind==1&&ioReads.count(task))IoMemorySubmit(record);
 if(kind==0&&ioMemoryWrites.count(task)){
  auto a=ioRecords.at(record);ioMemory.SubmitAxi(record,a.dc,256,ioMemoryLatency,[record](uint64_t){IoReply(record);});return;
 }
 if(kind==0){auto chip=r.dc;auto from=r.dd;auto to=r.proxy;RingTransfer(chip,from,to,ioSizes[2],[](){},[record](){IoReply(record);});}
}
static bool IoReceive(Ptr<SueNetDevice> dev,Ptr<Packet> p,SueHeader h){
 V3Tag tag;if(!p->PeekPacketTag(tag)||tag.reply!=2)return false;
 uint32_t wire=p->GetSize();if(tag.id>=ioPackets.size()){++unknownFrames;dev->RefundCredit(h.GetShim().dscp&7,wire);return true;}
 auto&f=ioPackets[tag.id];auto r=ioRecords[f.records.front()];auto&pp=ports.at(devPort.at(dev));SueFrameBadTag bad;
 bool valid=!p->PeekPacketTag(bad)&&tag.bytes==f.bytes&&wire==f.bytes+h.GetSerializedSize()&&h.GetShim().dscp==r.vc&&pp.chip==r.dc&&pp.die==r.proxy&&pp.i==r.port&&h.GetSrc()==SaMac(r.sc,r.sd)&&h.GetDst()==SaMac(r.dc,r.dd);
 if(!valid){++pathErrors;dev->RefundCredit(r.vc,wire);return true;}
 auto ids=f.records;
 RingTransfer(r.dc,r.proxy,r.dd,f.bytes,[=](){dev->RefundCredit(r.vc,wire);},[ids](){for(auto id:ids)IoDelivered(id);});return true;
}
static void IoIssue(uint32_t id){
 auto&t=tasks[id];auto spec=ioSpecs.at(id);uint32_t seq=ioIssued[id];
 uint32_t logical=spec.kind==1?uint32_t((uint64_t(seq+1)*t.bytes)/spec.count-(uint64_t(seq)*t.bytes)/spec.count):std::min<uint64_t>(256,t.bytes-uint64_t(seq)*256);
 IoRecord r{id,seq,spec.kind,t.sc,t.sd,t.dc,t.dd,uint32_t(t.port),uint32_t(t.proxy),t.vc,logical,0};
 if(!IoEnqueue(r)){++t.blocked;
  if(ioEventDriven)IoPark(r,[id](){IoIssue(id);});
  else Later(NanoSeconds(interval),[id](){IoIssue(id);});return;
 }
 ++ioIssued[id];t.sent+=logical;if(ioIssued[id]<spec.count)Later(NanoSeconds(ioInterval),[id](){IoIssue(id);});
}
static void IoReadResponse(uint32_t request,uint32_t seq){
 auto&read=ioReads.at(request);auto&t=tasks[read.response];auto&a=tasks[request];
 if(t.start<0){t.start=t.ready=Simulator::Now().GetPicoSeconds();t.status="running";t.port=a.port;t.group=a.group;
  t.proxy=Proxy(t.sc,t.sd,t.dc,t.dd,t.port);if(t.proxy<0)throw std::runtime_error("streaming AXI response route");t.hops=Distance(t.proxy,t.dd);}
 uint32_t useful=std::min<uint64_t>(256,t.bytes-uint64_t(seq)*256);
 IoRecord r{read.response,seq,3,t.sc,t.sd,t.dc,t.dd,uint32_t(t.port),uint32_t(t.proxy),t.vc,useful,0};
 if(!IoEnqueue(r)){++t.blocked;
  if(ioEventDriven)IoPark(r,[request,seq](){IoReadResponse(request,seq);});
  else Later(NanoSeconds(interval),[request,seq](){IoReadResponse(request,seq);});return;
 }
 t.sent+=useful;++ioIssued[read.response];
}
static void IoMemorySubmit(uint32_t record){
 auto r=ioRecords.at(record);auto&read=ioReads.at(r.task);auto&t=tasks[read.service];
 if(t.start<0){t.start=t.ready=Simulator::Now().GetPicoSeconds();t.status="running";}
 ioMemory.SubmitAxi(record,r.dc,256,ioMemoryLatency,[request=r.task,seq=r.seq](uint64_t admitted){
  auto&read=ioReads.at(request);auto&t=tasks[read.service];
  if(t.serviceStart<0||int64_t(admitted)<t.serviceStart)t.serviceStart=admitted;
  if(++read.completed==read.count){t.dataEnd=Simulator::Now().GetPicoSeconds();Complete(read.service);}
  IoReadResponse(request,seq);
 });
}
static void IoActivateMpu(const std::string&dir){
 std::ifstream f(dir+"/io-mpu.tsv");if(!f)return;uint64_t rate,slots;
 if(!ioEnabled||!(f>>rate>>slots>>ioMemoryLatency)||!rate||!slots)throw std::runtime_error("invalid AXI MPU configuration");
 ioMemory.Configure(rate,slots,true);IoRead r;
 while(f>>r.request>>r.service>>r.response>>r.count){
  if(!ioSpecs.count(r.request)||!ioSpecs.count(r.response)||ioSpecs.at(r.request).kind!=1||ioSpecs.at(r.response).kind!=3||tasks.at(r.service).resource!=2||!ioReads.emplace(r.request,r).second)throw std::runtime_error("invalid AXI MPU graph");
  for(auto pair:{std::make_pair(r.service,r.request),std::make_pair(r.response,r.service)}){
   ioManagedEdges.insert(pair);auto&parent=tasks[pair.second];
   for(auto v:{&parent.next,&parent.receiveNext})v->erase(std::remove(v->begin(),v->end(),pair.first),v->end());
  }
  mpuModel.Skip(r.service);
 }
 if(!f.eof())throw std::runtime_error("malformed AXI MPU mapping");
 std::ifstream w(dir+"/io-mpu-writes.tsv");uint32_t id;
 while(w>>id){if(!ioSpecs.count(id)||ioSpecs.at(id).kind!=0||tasks[id].dc<nx||!ioMemoryWrites.insert(id).second)throw std::runtime_error("invalid AXI MPU write");}
 if(!w.eof())throw std::runtime_error("malformed AXI MPU writes");
}
static void IoLoad(const std::string&dir){
 std::ifstream f(dir+"/io-die.tsv");if(!f)return;ioEnabled=true;
 if(!(f>>ioPack>>ioCap>>ioInterval>>ioLimit>>ioHeader>>ioMinMac>>ioSizes[0]>>ioSizes[1]>>ioSizes[2]>>ioSizes[3])||!ioLimit||!ioInterval||ioCap>payload)throw std::runtime_error("invalid IO config");
 uint32_t id;IoSpec s;while(f>>id>>s.kind>>s.count>>s.parent){if(id>=tasks.size()||s.kind>3||s.kind==2||!s.count||!ioSpecs.emplace(id,s).second)throw std::runtime_error("invalid IO task");}
 if(!f.eof())throw std::runtime_error("malformed IO mapping");
 for(uint32_t i=0;i<tasks.size();++i)if(bool(tasks[i].bytes)!=bool(ioSpecs.count(i)))throw std::runtime_error("IO coverage");
}
static bool IoSave(const std::string&out){
 if(!ioEnabled)return true;bool ok=ioMemory.Save(out,"io_mpu");const char*names[]={"AW_W","AR","B","R"};
 std::ofstream f(out+"/io_records.csv");f<<"record,task,seq,kind,sc,sd,dc,dd,port,vc,logical_bytes,ready_ps,received_ps\n";
 for(uint32_t i=0;i<ioRecords.size();++i){auto&r=ioRecords[i];ok=ok&&r.received>=0;f<<i<<','<<r.task<<','<<r.seq<<','<<names[r.kind]<<','<<r.sc<<','<<r.sd<<','<<r.dc<<','<<r.dd<<','<<r.port<<','<<r.vc<<','<<r.logical<<','<<r.ready<<','<<r.received<<'\n';}
 std::ofstream g(out+"/io_packets.csv");g<<"packet,payload_bytes,unpadded_payload_bytes,mac_frame_bytes,enqueued_ps,records\n";
 for(uint32_t i=0;i<ioPackets.size();++i){auto&p=ioPackets[i];g<<i<<','<<p.bytes<<','<<p.unpadded<<','<<p.mac<<','<<p.sent<<',';for(uint32_t j=0;j<p.records.size();++j){if(j)g<<';';g<<p.records[j];}g<<'\n';}
 std::ofstream w(out+"/io_wire.csv");w<<"packet,time_ps,chip,die,port,switch,physical_port,side,wire_bytes,tx_busy_ps\n";
 for(auto&e:ioWires){auto&p=ports[e.port];w<<e.packet<<','<<e.time<<','<<p.chip<<','<<p.die<<','<<p.i<<','<<p.sw<<','<<p.physical<<','<<e.side<<','<<e.bytes<<','<<e.busy<<'\n';}
 for(auto&q:ioQueues)ok=ok&&q.second.empty();
 for(auto&w:ioWaiters)ok=ok&&w.second.phases.empty();
 ok=ok&&!ioPending&&ioEgressWaiting.empty();
 std::ofstream schedule(out+"/io_scheduler.csv");
 schedule<<"event_driven,parked,resumed,wake_events,pending,peak_pending,egress_checks,egress_blocks,egress_notifications\n"
  <<ioEventDriven<<','<<ioParked<<','<<ioResumed<<','<<ioWakeEvents<<','<<ioPending<<','<<ioPeakPending<<','<<ioEgressChecks<<','<<ioEgressBlocks<<','<<ioEgressNotifications<<'\n';
 return ok;
}

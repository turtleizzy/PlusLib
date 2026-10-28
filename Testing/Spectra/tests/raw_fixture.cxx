// OFFLINE ONLY. The supplied connected FD must be a private AF_UNIX socketpair.
#include "PlusNDIRaw.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>
int main(int argc,char** argv){
 if(argc<5)return 3;std::string mode=argv[1],parent=argv[2];int fd=std::stoi(argv[3]);
 bool tcp=mode.rfind("tcp",0)==0;
 struct stat st;if(!tcp&&(fstat(fd,&st)||!S_ISSOCK(st.st_mode)))return 4;
 if(mode=="pack"){
  std::ifstream input(argv[4],std::ios::binary);std::string payload((std::istreambuf_iterator<char>(input)),{});
  try {std::ofstream out(parent+"/packed.igtl",std::ios::binary);auto wire=PlusNDIRawWire::Pack(payload);out.write(wire.data(),wire.size());std::cout<<"PACK_OK\n";}
  catch(const std::length_error&){std::cout<<"PACK_SIZE_REJECTED\n";}return 0;
 }
 PlusNDIRawConfig c;c.SourceId="95dd92b1-5d16-4974-ab90-84d17f35f4c4";c.StatusDirectory=parent;c.SerialEndpoint="SYNTHETIC_SOCKETPAIR_NO_DEVICE";c.WriteTimeoutMs=50;c.DrainTimeoutMs=100;
 if(tcp){c.Port=unsigned(fd);c.SerialEndpoint="SYNTHETIC_TCP_NO_DEVICE";}
 if(mode=="byte_budget")c.QueueBytes=sizeof(PlusNDISnapshot);
 if(mode=="overflow"){c.QueueCapacity=1;c.QueueBytes=sizeof(PlusNDISnapshot)*2;}
 if(mode=="model_check"){
  PlusNDIRawModel model;model.NewAcquisition(1);std::string clock=model.ClockDomain();
  std::vector<PlusNDIMarkerBinding> bs;PlusNDIMarkerBinding b;b.Handle=1;b.TransformName="ExampleToTracker";b.BodyFrame="Example";b.PlusStatus="OK";bs.push_back(b);model.PrepareBindings(bs,1);
  std::ifstream input(argv[4],std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});
  std::unique_ptr<ndiBXCapture> r(new ndiBXCapture);ndiDecodeBXRawCapture(bytes.data(),bytes.size(),NDI_BX_CAPTURE_FLAGS,r.get(),1);r->transport_state=NDI_BX_ORDERED_UNVERIFIED;
  r->tools[0].frame_number=5;auto first=model.Seal(*r,bs,1,1);
  r->tools[0].frame_number=4;auto backward=model.Seal(*r,bs,1,2);
  bs[0].Handle=2;r->tools[0].handle=2;model.PrepareBindings(bs,1);auto rebound=model.Seal(*r,bs,1,3);
  if(!rebound->ToolStates[0].Quarantined||rebound->ToolStates[0].BindingEpoch==backward->ToolStates[0].BindingEpoch)return 12;
  model.NewAcquisition(1);r->tools[0].frame_number=6;auto restarted=model.Seal(*r,bs,1,1);
  model.NewAcquisition(2);r->tools[0].frame_number=7;model.PrepareBindings(bs,2);auto recovered=model.Seal(*r,bs,2,1);
  if(first->Quarantined||!backward->ToolStates[0].Quarantined||!restarted->Quarantined||recovered->Quarantined||recovered->ToolStates[0].Classification!="FIRST"||model.ClockDomain()!=clock||restarted->AcquisitionId==recovered->AcquisitionId)return 9;
  r->receive_monotonic_ns=1000000;r->receive_wall_ns=5000000;model.Seal(*r,bs,2,2);
  r->receive_monotonic_ns=2000000;r->receive_wall_ns=4000000;auto jump=model.Seal(*r,bs,2,3);if(!(jump->Diagnostics&8))return 10;
  std::cout<<"MODEL_RECOVERY_AND_CLOCK_CHECK_OK\n";return 0;
 }
 PlusNDIRawSender sender;std::string error;
 if(mode=="start_retry"){
  setenv("NDI_TEST_ARMED","1",1);
  bool first=sender.StartConnectedForTest(c,dup(fd),error);
  std::cout<<"first="<<first<<" stop="<<sender.Stop()<<","<<sender.Stop()<<" error="<<sender.Error()<<"\n";
  if(first)return 8;
 }
 if(!(tcp?sender.Start(c,error):sender.StartConnectedForTest(c,fd,error))){std::cerr<<error;return 5;}
 PlusNDIRawModel model;model.NewAcquisition(1);PlusNDIMarkerRecorder sidecar;
 if(!sidecar.Start(parent,"Tracker",64,20,115200,error,"synthetic_fixture"))return 6;
 if(mode!="overflow")std::this_thread::sleep_for(std::chrono::milliseconds(30));
 uint64_t seq=0;double maxProducer=0;
 for(int file=4;file<argc;++file){
  std::ifstream input(argv[file],std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),{});
  std::unique_ptr<ndiBXCapture> r(new ndiBXCapture);
  ndiDecodeBXRawCapture(bytes.data(),bytes.size(),NDI_BX_CAPTURE_FLAGS,r.get(),1);
  r->command_wire_bytes=12;r->command_write_count_known=1;r->request_sent=1;r->transport_state=r->valid?NDI_BX_ORDERED_UNVERIFIED:NDI_BX_RECONNECT_REQUIRED;
  r->command_start_monotonic_ns=1000000000ULL+seq*50000000;r->receive_monotonic_ns=r->command_start_monotonic_ns+20000000;r->decode_done_monotonic_ns=r->receive_monotonic_ns+100;r->receive_wall_ns=1700000000000000000ULL+seq*50000000;
  std::vector<PlusNDIMarkerBinding> bs;PlusNDIMarkerBinding b;b.Handle=1;b.TransformName="ExampleToTracker";b.BodyFrame="Example";
  if(r->valid&&r->tool_count){const auto& t=r->tools[0];b.PlusStatus=(t.port_status&0x31)!=0x31?"MISSING":(t.port_status&NDI_OUT_OF_VOLUME)?"OUT_OF_VOLUME":!t.pose_present?"OUT_OF_VIEW":"OK";}
  bs.push_back(b);model.PrepareBindings(bs,1);
  unsigned n=mode=="overflow"?2000:1;
  for(unsigned k=0;k<n;++k){
   auto start=std::chrono::steady_clock::now();
   sender.BeginAttempt(++seq,bs,model);const auto snapshot=model.Seal(*r,bs,1,seq);sender.Enqueue(snapshot);sidecar.Enqueue(snapshot);
   maxProducer=std::max(maxProducer,std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
   if(mode!="overflow")std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
 }
 bool a=sender.Stop(),b=sender.Stop(),d=sidecar.Stop();
 std::cout<<"raw="<<a<<","<<b<<" sidecar="<<d<<" max_producer_s="<<maxProducer<<" state="<<sender.State()<<" error="<<sender.Error()<<"\n";
 return a==b?0:7;
}

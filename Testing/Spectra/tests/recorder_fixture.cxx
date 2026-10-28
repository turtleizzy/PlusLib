/* SYNTHETIC fixture harness, never opens a tracker device. */
#include "PlusNDIMarkerRecorder.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <signal.h>
#include <sys/resource.h>
#include <thread>
#include <chrono>
int main(int argc,char** argv)
{
  if(argc<4)return 3;
  std::string parent=argv[1],mode=argv[2],error;
  PlusNDIMarkerRecorder recorder;
  if(mode=="hash"){
    std::string digest;uint64_t n=0;
    if(!PlusNDIMarkerRecorder::IdentifyRom(argv[3],digest,n))return 1;
    std::cout<<digest<<" "<<n<<std::endl;return 0;
  }
  if(!recorder.Start(parent,"Tracker",mode=="overflow"?1:64,20,115200,error,"synthetic_fixture")){
    std::cerr<<error<<std::endl;return 2;
  }
  std::cout<<recorder.Directory()<<std::endl;
  // Normal state/format fixtures run at the configured 20 Hz, after worker startup.
  // The separate overflow case intentionally saturates the nonblocking queue.
  if(mode!="overflow")std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if(mode=="write_failure"){
    signal(SIGXFSZ,SIG_IGN);rlimit limit={2048,2048};setrlimit(RLIMIT_FSIZE,&limit);
  }
  uint64_t seq=0;
  for(int file=3;file<argc;++file){
    std::ifstream input(argv[file],std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    std::unique_ptr<ndiBXCapture> reply(new ndiBXCapture);
    ndiDecodeBXCapture(bytes.data(),bytes.size(),NDI_BX_CAPTURE_FLAGS,reply.get());
    reply->command_wire_bytes=12;reply->command_write_count_known=1;reply->request_sent=1;reply->transport_state=NDI_BX_ORDERED_UNVERIFIED;
    reply->command_start_monotonic_ns=1000000000ULL+seq*50000000;
    reply->receive_monotonic_ns=reply->command_start_monotonic_ns+20000000;
    reply->receive_wall_ns=1700000000000000000ULL+seq*50000000;
    reply->decode_done_monotonic_ns=reply->receive_monotonic_ns+100;
    std::vector<PlusNDIMarkerBinding> bindings;
    unsigned count=reply->tool_count?reply->tool_count:1;
    for(unsigned i=0;i<count;++i){
      PlusNDIMarkerBinding b;b.Handle=reply->tool_count?reply->tools[i].handle:1;
      b.BodyFrame="Test"+std::to_string(i+1);b.TransformName=b.BodyFrame+"ToTracker";
      b.RomPath="synthetic-fixture-only.rom";b.RomMD5="00000000000000000000000000000000";b.RomBytes=752;
      bindings.push_back(b);
    }
    unsigned repeats=mode=="overflow"?3000:1;
    for(unsigned i=0;i<repeats;++i){
      recorder.Enqueue(*reply,bindings,std::string(argv[file]).find("epoch2")!=std::string::npos?2:1,++seq);
      if(mode!="overflow")std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  bool complete=recorder.Stop();
  std::cerr<<"complete="<<complete<<" healthy="<<recorder.Healthy()<<" error="<<recorder.Error()<<std::endl;
  return complete?0:1;
}

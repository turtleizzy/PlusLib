// SYNTHETIC ONLY: no device open, no serial commands, no loopback sockets.
#include "PlusNDIMarkerRecorder.h"
#include "vtkPlusNDITracker.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <thread>

unsigned fds(){unsigned n=0;DIR* d=opendir("/proc/self/fd");while(readdir(d))++n;closedir(d);return n;}
std::string contents(const std::string& name){std::ifstream f(name);return std::string(std::istreambuf_iterator<char>(f),{});}
class TestTracker:public vtkPlusNDITracker {
public:
  static TestTracker* New(){return new TestTracker;}
  void Attach(PlusNDIMarkerRecorder* recorder){MarkerRecorder=recorder;EnableMeasuredMarkers=true;}
  PlusStatus StopInternal(){return InternalStopRecording();}
  PlusStatus DisconnectInternal(){return InternalDisconnect();}
};
int main(int argc,char** argv){
  if(argc!=4)return 3;
  std::string parent=argv[1],mode=argv[2],error;
  std::ifstream in(argv[3],std::ios::binary);std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});
  ndiBXCapture reply;ndiDecodeBXCapture(bytes.data(),bytes.size(),NDI_BX_CAPTURE_FLAGS,&reply);
  std::vector<PlusNDIMarkerBinding> bindings(1);bindings[0].Handle=1;bindings[0].TransformName="Test1ToTracker";
  auto* recorder=new PlusNDIMarkerRecorder;
  unsigned before=fds();bool virginStop=recorder->Stop();std::string virgin=recorder->StorageState();
  if(mode=="validation_failure"){
    bool invalid=recorder->Start(parent+"/nonexistent","Tracker",64,20,115200,error,"synthetic_fixture");
    std::cout<<"{\"started\":"<<invalid<<",\"state\":\""<<recorder->StorageState()<<"\",\"stop\":"<<recorder->Stop()<<"}"<<std::endl;
    delete recorder;return 0;
  }
  setenv("NDI_TEST_ARMED","1",1);
  bool started=recorder->Start(parent,"Tracker",mode=="drop"?1:64,20,115200,error,"synthetic_fixture");
  std::string first=recorder->Directory();std::string startState=recorder->StorageState();
  if(mode=="start_failure"){
    bool stopped=recorder->Stop(),again=recorder->Stop();unsigned afterFailure=fds();
    unsetenv("NDI_TEST_STORAGE_FAULT");
    bool retry=recorder->Start(parent,"Tracker",64,20,115200,error,"synthetic_fixture");
    std::string second=recorder->Directory();bool retryStop=recorder->Stop();unsigned after=fds();
    std::cout<<"{\"started\":"<<started<<",\"state\":\""<<startState<<"\",\"stop\":"<<stopped<<",\"stop_again\":"<<again<<",\"retry\":"<<retry<<",\"retry_stop\":"<<retryStop<<",\"fds_before\":"<<before<<",\"fds_failed\":"<<afterFailure<<",\"fds_after\":"<<after<<",\"first\":\""<<first<<"\",\"second\":\""<<second<<"\"}"<<std::endl;
    delete recorder;return 0;
  }
  if(!started){std::cerr<<error<<std::endl;delete recorder;return 2;}
  if(mode=="driver"||mode=="driver_drop"){
    for(unsigned i=0;i<(mode=="driver_drop"?3000:1);++i)recorder->Enqueue(reply,bindings,1,i+1);std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto* tracker=TestTracker::New();tracker->Attach(recorder);
    PlusStatus internal=tracker->StopInternal(),internalDisconnect=tracker->DisconnectInternal();
    PlusStatus stop=tracker->StopRecording(),again=tracker->StopRecording(),disconnect=tracker->Disconnect(),disconnectAgain=tracker->Disconnect();
    std::cout<<"{\"path\":\""<<first<<"\",\"state\":\""<<tracker->GetMeasuredMarkerStorageState()<<"\",\"internal\":"<<internal<<",\"internal_disconnect\":"<<internalDisconnect<<",\"stop\":"<<stop<<",\"again\":"<<again<<",\"disconnect\":"<<disconnect<<",\"disconnect_again\":"<<disconnectAgain<<"}"<<std::endl;
    tracker->Delete();return 0;
  }
  std::atomic<bool> done{false};std::thread producer;
  if(mode=="race_stop")producer=std::thread([&]{uint64_t i=0;while(!done){recorder->Enqueue(reply,bindings,1,++i);}});
  else for(unsigned i=0;i<(mode=="drop"?3000:1);++i)recorder->Enqueue(reply,bindings,1,i+1);
  if(mode=="race_stop")std::this_thread::sleep_for(std::chrono::milliseconds(30));
  bool stopped=recorder->Stop();done=true;if(producer.joinable())producer.join();
  std::string original=contents(first+"/summary.json");bool postSeal=recorder->Enqueue(reply,bindings,1,99999);
  bool again=recorder->Stop();bool same=original==contents(first+"/summary.json");
  std::cout<<"{\"virgin\":\""<<virgin<<"\",\"virgin_stop\":"<<virginStop<<",\"path\":\""<<first<<"\",\"stop\":"<<stopped<<",\"again\":"<<again<<",\"post_seal\":"<<postSeal<<",\"outside_session\":"<<recorder->RejectedOutsideSession()<<",\"same_summary\":"<<same<<",\"state\":\""<<recorder->StorageState()<<"\",\"fds_before\":"<<before<<",\"fds_after\":"<<fds()<<"}"<<std::endl;
  delete recorder;return 0;
}

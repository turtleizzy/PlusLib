// OFFLINE ONLY: deterministic faults for test PTYs / private ndi-marker files.
// Never preload into a real-device capture process.
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

namespace {
std::atomic<bool> injected{false};
std::atomic<unsigned> dataWrites{0}, serialReads{0};
bool mode(const char* key,const char* expected){const char* p=getenv(key);return p&&!strcmp(p,expected);}
bool path(int fd,char* p){ssize_t n=readlink((std::string("/proc/self/fd/")+std::to_string(fd)).c_str(),p,1023);if(n<0)return false;p[n]=0;return true;}
bool serial(int fd){char p[1024];const char* wanted=getenv("NDI_TEST_SERIAL_PATH");return wanted&&strstr(wanted,"/dev/pts/")==wanted&&path(fd,p)&&!strcmp(p,wanted);}
bool sink(int fd,const char* suffix){char p[1024];return path(fd,p)&&strstr(p,"/ndi-marker-")&&strstr(p,suffix);}
bool once(const char* value){return mode("NDI_TEST_STORAGE_FAULT",value)&&!injected.exchange(true);}
}
extern "C" {
ssize_t read(int fd,void* data,size_t n){
  static auto real=reinterpret_cast<ssize_t(*)(int,void*,size_t)>(dlsym(RTLD_NEXT,"read"));
  if(serial(fd)){
    unsigned count=++serialReads;
    if(mode("NDI_TEST_SERIAL_FAULT","read_eagain")||(count>1&&mode("NDI_TEST_SERIAL_FAULT","reset_eagain"))){errno=EAGAIN;return -1;}
    if(mode("NDI_TEST_SERIAL_FAULT","read_eintr")||(count>1&&mode("NDI_TEST_SERIAL_FAULT","reset_eintr"))){errno=EINTR;return -1;}
  }
  return real(fd,data,n);
}
ssize_t write(int fd,const void* data,size_t n){
  static auto real=reinterpret_cast<ssize_t(*)(int,const void*,size_t)>(dlsym(RTLD_NEXT,"write"));
  if(serial(fd)){
    if(mode("NDI_TEST_SERIAL_FAULT","write_eagain")){errno=EAGAIN;return -1;}
    if(mode("NDI_TEST_SERIAL_FAULT","write_eintr")){errno=EINTR;return -1;}
    if(mode("NDI_TEST_SERIAL_FAULT","zero_write"))return 0;
    if(mode("NDI_TEST_SERIAL_FAULT","short_write")&&n>3)n=3;
  }
  if(sink(fd,"/measurements.jsonl")){
    unsigned count=++dataWrites;
    if((count==1&&once("header_write"))||(count>1&&once("measurement_write"))){errno=EIO;return -1;}
  }
  if(sink(fd,"/summary.pending.json")&&once("summary_write")){errno=EIO;return -1;}
  return real(fd,data,n);
}
int poll(pollfd* fds,nfds_t n,int timeout){
  static auto real=reinterpret_cast<int(*)(pollfd*,nfds_t,int)>(dlsym(RTLD_NEXT,"poll"));
  if(mode("NDI_TEST_SERIAL_FAULT","poll_eintr"))for(nfds_t i=0;i<n;++i)if(serial(fds[i].fd)){errno=EINTR;return -1;}
  return real(fds,n,timeout);
}
int fsync(int fd){
  static auto real=reinterpret_cast<int(*)(int)>(dlsym(RTLD_NEXT,"fsync"));
  if((sink(fd,"/measurements.jsonl")&&once("data_fsync"))||(sink(fd,"/summary.pending.json")&&once("summary_fsync"))){errno=EIO;return -1;}
  return real(fd);
}
int close(int fd){
  static auto real=reinterpret_cast<int(*)(int)>(dlsym(RTLD_NEXT,"close"));
  bool fail=(sink(fd,"/measurements.jsonl")&&once("data_close"))||(sink(fd,"/summary.pending.json")&&once("summary_close"));
  int result=real(fd);if(fail){errno=EIO;return -1;}return result;
}
int link(const char* from,const char* to){
  static auto real=reinterpret_cast<int(*)(const char*,const char*)>(dlsym(RTLD_NEXT,"link"));
  if(strstr(from,"/ndi-marker-")&&once("summary_link")){errno=EPERM;return -1;}
  return real(from,to);
}
int pthread_create(pthread_t* thread,const pthread_attr_t* attr,void*(*fn)(void*),void* value){
  static auto real=reinterpret_cast<int(*)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*)>(dlsym(RTLD_NEXT,"pthread_create"));
  if(getenv("NDI_TEST_ARMED")&&once("thread_create"))return EAGAIN;
  return real(thread,attr,fn,value);
}
}

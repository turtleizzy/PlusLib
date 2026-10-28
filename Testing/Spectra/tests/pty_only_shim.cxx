/* OFFLINE TEST ONLY. Refuses all physical /dev paths and exposes only our PTY. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
static bool blocked(const char* p){
  if(!p || std::strncmp(p,"/dev/",5))return false;
  return std::strncmp(p,"/dev/pts/",9) && std::strcmp(p,"/dev/null") &&
    std::strcmp(p,"/dev/zero") && std::strcmp(p,"/dev/urandom") && std::strcmp(p,"/dev/random");
}
__attribute__((constructor)) static void guard(){
  const char* p=getenv("NDI_MARKER_PTY");
  if(!p || std::strncmp(p,"/dev/pts/",9))_exit(90);
}
extern "C" const char* ndiSerialDeviceName(int index){
  return index==0?getenv("NDI_MARKER_PTY"):nullptr;
}
static mode_t mode(int flags,va_list& ap){
  return (flags&O_CREAT)||((flags&O_TMPFILE)==O_TMPFILE)?va_arg(ap,int):0;
}
extern "C" int open(const char* p,int flags,...){
  if(blocked(p)){errno=EACCES;return -1;}
  va_list a;va_start(a,flags);mode_t m=mode(flags,a);va_end(a);
  using F=int(*)(const char*,int,...);return reinterpret_cast<F>(dlsym(RTLD_NEXT,"open"))(p,flags,m);
}
extern "C" int open64(const char* p,int flags,...){
  if(blocked(p)){errno=EACCES;return -1;}
  va_list a;va_start(a,flags);mode_t m=mode(flags,a);va_end(a);
  using F=int(*)(const char*,int,...);return reinterpret_cast<F>(dlsym(RTLD_NEXT,"open64"))(p,flags,m);
}
extern "C" int openat(int fd,const char* p,int flags,...){
  if(blocked(p)){errno=EACCES;return -1;}
  va_list a;va_start(a,flags);mode_t m=mode(flags,a);va_end(a);
  using F=int(*)(int,const char*,int,...);return reinterpret_cast<F>(dlsym(RTLD_NEXT,"openat"))(fd,p,flags,m);
}
extern "C" int openat64(int fd,const char* p,int flags,...){
  if(blocked(p)){errno=EACCES;return -1;}
  va_list a;va_start(a,flags);mode_t m=mode(flags,a);va_end(a);
  using F=int(*)(int,const char*,int,...);return reinterpret_cast<F>(dlsym(RTLD_NEXT,"openat64"))(fd,p,flags,m);
}

// A private pipe lets the deterministic PTY peer emulate a hardware reset reply.
// The constructor above still prohibits physical serial paths.
extern "C" int tcsendbreak(int fd,int duration){
  using F=int(*)(int,int);int result=reinterpret_cast<F>(dlsym(RTLD_NEXT,"tcsendbreak"))(fd,duration);
  const char* notify=getenv("NDI_RESET_TEST_FD");
  if(notify){char byte='R';::write(std::atoi(notify),&byte,1);}
  return result;
}

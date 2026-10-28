// OFFLINE ONLY. Preload exclusively into raw_fixture with private socketpairs.
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
std::atomic<unsigned> calls{0};std::atomic<bool> used{false};
bool mode(const char* s){const char* p=getenv("RAW_TEST_FAULT");return p&&!strcmp(p,s);}
bool rawfd(int fd){char p[2048];auto n=readlink(("/proc/self/fd/"+std::to_string(fd)).c_str(),p,sizeof(p)-1);if(n<0)return false;p[n]=0;const char* base=strrchr(p,'/');return base&&!strncmp(base+1,"raw-",4);}
}
extern "C" {
ssize_t sendmsg(int fd,const msghdr* msg,int flags){
 static auto real=reinterpret_cast<ssize_t(*)(int,const msghdr*,int)>(dlsym(RTLD_NEXT,"sendmsg"));unsigned n=++calls;
 if(mode("timeout")||(mode("partial_timeout")&&n>1)){errno=EAGAIN;return -1;}
 if(mode("eintr")&&n<8){errno=EINTR;return -1;}
 if(mode("eagain")&&n<8){errno=EAGAIN;return -1;}
 if(mode("zero"))return 0;
 if((mode("short")||mode("partial_timeout"))&&msg->msg_iovlen){auto copy=*msg;auto part=msg->msg_iov[0];part.iov_len=std::min<size_t>(part.iov_len,7);copy.msg_iov=&part;copy.msg_iovlen=1;return real(fd,&copy,flags);}
 return real(fd,msg,flags);
}
ssize_t write(int fd,const void* b,size_t n){static auto real=reinterpret_cast<ssize_t(*)(int,const void*,size_t)>(dlsym(RTLD_NEXT,"write"));if(rawfd(fd)&&mode("summary_write")&&!used.exchange(true)){errno=EIO;return -1;}return real(fd,b,n);}
int fsync(int fd){static auto real=reinterpret_cast<int(*)(int)>(dlsym(RTLD_NEXT,"fsync"));if(rawfd(fd)&&mode("summary_fsync")&&!used.exchange(true)){errno=EIO;return -1;}return real(fd);}
int close(int fd){static auto real=reinterpret_cast<int(*)(int)>(dlsym(RTLD_NEXT,"close"));bool fail=rawfd(fd)&&mode("summary_close")&&!used.exchange(true);int code=real(fd);if(fail){errno=EIO;return -1;}return code;}
int link(const char* a,const char* b){static auto real=reinterpret_cast<int(*)(const char*,const char*)>(dlsym(RTLD_NEXT,"link"));const char* base=strrchr(a,'/');if(base&&!strncmp(base+1,"raw-",4)&&mode("summary_link")&&!used.exchange(true)){errno=EIO;return -1;}return real(a,b);}
}

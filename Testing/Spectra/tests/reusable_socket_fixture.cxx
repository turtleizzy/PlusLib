// Loopback-only test. Never accesses a tracker or a production endpoint.
#include "PlusReusableServerSocket.h"
#include <thread>
#include <iostream>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
int main(int argc,char** argv){
 int port=argc>1?std::atoi(argv[1]):0;
#ifdef BASELINE_SOCKET
 using Socket=igtl::ServerSocket;
#else
 using Socket=PlusReusableServerSocket;
#endif
 const int rounds=argc>2?std::atoi(argv[2]):1;
 for(int i=0;i<rounds;++i){
  auto server=Socket::New();if(server->CreateServer(port)){std::cout<<"BIND_FAILED round="<<i<<" port="<<port<<" errno="<<errno<<"\n";return 2;}
  port=server->GetServerPort();
  std::thread peer([&](){int fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)))_exit(7);char byte;recv(fd,&byte,1,0);recv(fd,&byte,1,0);close(fd);});
  auto client=server->WaitForConnection(1000);if(!client){peer.join();return 3;}
  client->Send("x",1);client->CloseSocket();server->CloseSocket();peer.join();
 }
 std::cout<<"RESTART_PASS rounds="<<rounds<<" port="<<port<<"\n";
}

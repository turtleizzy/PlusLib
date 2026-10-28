// Local compatibility for OpenIGTLink builds whose Linux BindSocket lacks
// SO_REUSEADDR. No changes to the shared dependency or the wire protocol.
#ifndef PlusReusableServerSocket_h
#define PlusReusableServerSocket_h
#include <igtlServerSocket.h>
#if defined(__linux__)
#include <sys/socket.h>
#include <cerrno>
class PlusReusableServerSocket : public igtl::ServerSocket
{
public:
  igtlTypeMacro(PlusReusableServerSocket, igtl::ServerSocket);
  igtlNewMacro(PlusReusableServerSocket);
  int CreateServer(int port)
  {
    this->igtl::Socket::CloseSocket();
    this->m_SocketDescriptor = this->CreateSocket();
    if (this->m_SocketDescriptor < 0) { return -1; }
    const int enabled = 1;
    if (::setsockopt(this->m_SocketDescriptor, SOL_SOCKET, SO_REUSEADDR,
                     &enabled, sizeof(enabled)) != 0 ||
        this->BindSocket(this->m_SocketDescriptor, port) != 0 ||
        this->Listen(this->m_SocketDescriptor) != 0)
    {
      const int error = errno;
      this->igtl::Socket::CloseSocket();
      errno = error;
      return -1;
    }
    return 0;
  }
protected:
  PlusReusableServerSocket() = default;
  ~PlusReusableServerSocket() override = default;
};
#else
using PlusReusableServerSocket = igtl::ServerSocket;
#endif
#endif

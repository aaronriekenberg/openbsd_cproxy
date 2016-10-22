#include "socketutil.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

static void setSockAddrInfoSize(
  struct SockAddrInfo* sockAddrInfo)
{
  sockAddrInfo->saSize = sizeof(sockAddrInfo->saStorage);
}

static bool sockAddrToNameAndPort(
  const struct sockaddr* address,
  const socklen_t addressSize,
  struct AddrPortStrings* addrPortStrings)
{
  int retVal;
  if ((retVal = getnameinfo(address,
                            addressSize,
                            addrPortStrings->addrString,
                            NI_MAXHOST,
                            addrPortStrings->portString,
                            NI_MAXSERV,
                            (NI_NUMERICHOST | NI_NUMERICSERV))) != 0)
  {
    printf("getnameinfo error: %s\n", gai_strerror(retVal));
    return false;
  }
  return true;
}

bool addrInfoToNameAndPort(
  const struct addrinfo* addrinfo,
  struct AddrPortStrings* addrPortStrings)
{
  return sockAddrToNameAndPort(addrinfo->ai_addr,
                               addrinfo->ai_addrlen,
                               addrPortStrings);
}

bool sockAddrInfoToNameAndPort(
  const struct SockAddrInfo* sockAddrInfo,
  struct AddrPortStrings* addrPortStrings)
{
  return sockAddrToNameAndPort(&(sockAddrInfo->sa),
                               sockAddrInfo->saSize,
                               addrPortStrings);
}

bool createNonBlockingSocket(
  const struct addrinfo* addrinfo,
  int* socketFD)
{
  *socketFD = socket(addrinfo->ai_family,
                     addrinfo->ai_socktype | SOCK_NONBLOCK,
                     addrinfo->ai_protocol);
  return ((*socketFD) != -1);
}

bool setSocketListening(
  const int socket)
{
  return (listen(socket, SOMAXCONN) != -1);
}

bool setSocketReuseAddress(
  const int socket)
{
  int optval = 1;
  return (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                     &optval, sizeof(optval)) != -1);
}

bool setSocketSplice(
  const int fromSocket,
  const int toSocket)
{
  return (setsockopt(fromSocket, SOL_SOCKET, SO_SPLICE,
                     &toSocket, sizeof(toSocket)) != -1);
}

bool setBidirectionalSplice(
  const int socket1,
  const int socket2)
{
  bool retVal = setSocketSplice(socket1, socket2);

  if (retVal)
  {
    retVal = setSocketSplice(socket2, socket1);
  }

  return retVal;
}

off_t getSpliceBytesTransferred(
  const int socket)
{
  off_t bytesTransferred;
  socklen_t optlen = sizeof(bytesTransferred);
  int retVal =
    getsockopt(socket, SOL_SOCKET, SO_SPLICE, &bytesTransferred, &optlen);
  if (retVal == -1)
  {
    bytesTransferred = 0;
  }
  return bytesTransferred;
}

int getSocketError(
  const int socket)
{
  int optval = 0;
  socklen_t optlen = sizeof(optval);
  int retVal =
    getsockopt(socket, SOL_SOCKET, SO_ERROR, &optval, &optlen);
  if (retVal == -1)
  {
    return retVal;
  }
  return optval;
}

bool signalSafeAccept(
  const int sockfd,
  int* acceptFD,
  struct SockAddrInfo* sockAddrInfo)
{
  bool interrupted;
  int retVal;
  do
  {
    struct sockaddr* addr = NULL;
    socklen_t* addrlen = NULL;
    if (sockAddrInfo != NULL)
    {
      setSockAddrInfoSize(sockAddrInfo);
      addr = &(sockAddrInfo->sa);
      addrlen = &(sockAddrInfo->saSize);
    }
    retVal = accept(sockfd, addr, addrlen);
    interrupted =
      ((retVal == -1) &&
       (errno == EINTR));
  } while (interrupted);

  *acceptFD = retVal;
  return (retVal != -1);
}

bool getSocketName(
  const int socketFD,
  struct SockAddrInfo* sockAddrInfo)
{
  int retVal;

  setSockAddrInfoSize(sockAddrInfo);

  retVal = getsockname(socketFD, &(sockAddrInfo->sa), &(sockAddrInfo->saSize));

  return (retVal != -1);
}

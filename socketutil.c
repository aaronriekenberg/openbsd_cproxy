#include "socketutil.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

static void setSockAddrInfoSize(struct SockAddrInfo* sockAddrInfo)
{
  sockAddrInfo->saSize = sizeof(struct sockaddr_storage);
}

static bool sockAddrToNameAndPort(
  const struct sockaddr* address,
  const socklen_t addressSize,
  struct AddrPortStrings* addrPortStrings)
{
  const int retVal = getnameinfo(address,
                                 addressSize,
                                 addrPortStrings->addrString,
                                 MAX_ADDR_STRING_LENGTH,
                                 addrPortStrings->portString,
                                 MAX_PORT_STRING_LENGTH,
                                 (NI_NUMERICHOST | NI_NUMERICSERV));
  if (retVal != 0)
  {
    printf("getnameinfo error: %s\n", gai_strerror(retVal));
  }
  return (retVal == 0);
}

bool addrInfoToNameAndPort(
  const struct addrinfo* addrinfo,
  struct AddrPortStrings* addrPortStrings)
{
  assert(addrinfo != NULL);
  assert(addrPortStrings != NULL);

  return sockAddrToNameAndPort(addrinfo->ai_addr,
                               addrinfo->ai_addrlen,
                               addrPortStrings);
}

bool sockAddrInfoToNameAndPort(
  const struct SockAddrInfo* sockAddrInfo,
  struct AddrPortStrings* addrPortStrings)
{
  assert(sockAddrInfo != NULL);
  assert(addrPortStrings != NULL);

  return sockAddrToNameAndPort(&(sockAddrInfo->sa),
                               sockAddrInfo->saSize,
                               addrPortStrings);
}

bool createNonBlockingSocket(
  const struct addrinfo* addrinfo,
  int* socketFD)
{
  assert(addrinfo != NULL);
  assert(socketFD != NULL);

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

bool bindSocket(
  const int socket,
  const struct addrinfo* addrinfo)
{
  assert(addrinfo != NULL);

  return (bind(socket, addrinfo->ai_addr, addrinfo->ai_addrlen) != -1);
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

enum AcceptSocketResult acceptSocket(
  const int socketFD,
  int* acceptFD,
  struct SockAddrInfo* sockAddrInfo)
{
  enum AcceptSocketResult acceptSocketResult;
  bool interrupted;
  int acceptRetVal;

  assert(acceptFD != NULL);

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
    acceptRetVal = accept(socketFD, addr, addrlen);
    interrupted =
      ((acceptRetVal == -1) &&
       (errno == EINTR));
  } while (interrupted);

  if (acceptRetVal == -1)
  {
    if (errno == EWOULDBLOCK)
    {
      acceptSocketResult = ACCEPT_SOCKET_RESULT_WOULD_BLOCK;
    }
    else
    {
      acceptSocketResult = ACCEPT_SOCKET_RESULT_ERROR;
    }
  }
  else
  {
    *acceptFD = acceptRetVal;
    acceptSocketResult = ACCEPT_SOCKET_RESULT_SUCCESS;
  }

  return acceptSocketResult;
}

bool getSocketName(
  const int socketFD,
  struct SockAddrInfo* sockAddrInfo)
{
  assert(sockAddrInfo != NULL);

  setSockAddrInfoSize(sockAddrInfo);

  return (getsockname(socketFD,
                      &(sockAddrInfo->sa),
                      &(sockAddrInfo->saSize)) != -1);
}

enum ConnectSocketResult connectSocket(
  const int socket,
  const struct addrinfo* addrinfo)
{
  enum ConnectSocketResult connectSocketResult;

  assert(addrinfo != NULL);

  if (connect(socket,
              addrinfo->ai_addr,
              addrinfo->ai_addrlen) == -1)
  {
    if ((errno == EINPROGRESS) || (errno == EINTR))
    {
      connectSocketResult = CONNECT_SOCKET_RESULT_IN_PROGRESS;
    }
    else
    {
      connectSocketResult = CONNECT_SOCKET_RESULT_ERROR;
    }
  }
  else
  {
    connectSocketResult = CONNECT_SOCKET_RESULT_CONNECTED;
  }

  return connectSocketResult;
}

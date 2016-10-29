#ifndef SOCKETUTIL_H
#define SOCKETUTIL_H

#include <netdb.h>
#include <stdbool.h>
#include <sys/socket.h>

struct SockAddrInfo
{
  union
  {
    struct sockaddr sa;
    struct sockaddr_storage saStorage;
  };
  socklen_t saSize;
};

#define MAX_ADDR_STRING_LENGTH (50)
#define MAX_PORT_STRING_LENGTH (6)

struct AddrPortStrings
{
  char addrString[MAX_ADDR_STRING_LENGTH];
  char portString[MAX_PORT_STRING_LENGTH];
};

extern bool addrInfoToNameAndPort(
  const struct addrinfo* addrinfo,
  struct AddrPortStrings* addrPortStrings);

extern bool sockAddrInfoToNameAndPort(
  const struct SockAddrInfo* sockAddrInfo,
  struct AddrPortStrings* addrPortStrings);

extern bool createNonBlockingSocket(
  const struct addrinfo* addrinfo,
  int* socketFD);

extern bool setSocketListening(
  const int socket);

extern bool setSocketReuseAddress(
  const int socket);

extern bool bindSocket(
  const int socket,
  const struct addrinfo* addrinfo);

extern bool setSocketSplice(
  const int fromSocket,
  const int toSocket);

extern bool setBidirectionalSplice(
  const int socket1,
  const int socket2);

extern off_t getSpliceBytesTransferred(
  const int socket);

extern int getSocketError(
  const int socket);

enum AcceptSocketResult
{
  ACCEPT_SOCKET_RESULT_ERROR,
  ACCEPT_SOCKET_RESULT_SUCCESS,
  ACCEPT_SOCKET_RESULT_WOULD_BLOCK
};

extern enum AcceptSocketResult acceptSocket(
  const int socketFD,
  int* acceptFD,
  struct SockAddrInfo* sockAddrInfo);

extern bool getSocketName(
  const int socketFD,
  struct SockAddrInfo* sockAddrInfo);

enum ConnectSocketResult
{
  CONNECT_SOCKET_RESULT_ERROR,
  CONNECT_SOCKET_RESULT_CONNECTED,
  CONNECT_SOCKET_RESULT_IN_PROGRESS
};

extern enum ConnectSocketResult connectSocket(
  const int socket,
  const struct addrinfo* addrinfo);

#endif

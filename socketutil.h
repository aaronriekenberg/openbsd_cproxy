#ifndef SOCKETUTIL_H
#define SOCKETUTIL_H

#include <netdb.h>
#include <stdbool.h>
#include <sys/socket.h>

struct AddrPortStrings
{
  char addrString[NI_MAXHOST];
  char portString[NI_MAXSERV];
};

extern bool addressToNameAndPort(
  const struct sockaddr* address,
  const socklen_t addressSize,
  struct AddrPortStrings* addrPortStrings);

extern bool setSocketListening(
  const int socket);

extern bool setSocketReuseAddress(
  const int socket);

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

extern bool signalSafeAccept(
  const int sockfd,
  int* acceptFD,
  struct sockaddr* addr,
  socklen_t* addrlen);

#endif

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
  int socket);

extern bool setSocketReuseAddress(
  int socket);

extern bool setSocketSplice(
  int fromSocket,
  int toSocket);

extern bool setBidirectionalSplice(
  int socket1,
  int socket2);

extern off_t getSpliceBytesTransferred(
  int socket);

extern int getSocketError(
  int socket);

extern bool signalSafeAccept(
  int sockfd,
  int* acceptFD,
  struct sockaddr* addr,
  socklen_t* addrlen);

#endif

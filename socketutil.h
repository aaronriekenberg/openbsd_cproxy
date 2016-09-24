#ifndef SOCKETUTIL_H
#define SOCKETUTIL_H

#include <netdb.h>
#include <sys/socket.h>

struct AddrPortStrings
{
  char addrString[NI_MAXHOST];
  char portString[NI_MAXSERV];
};

extern int addressToNameAndPort(
  const struct sockaddr* address,
  const socklen_t addressSize,
  struct AddrPortStrings* addrPortStrings);

extern int setSocketListening(
  int socket);

extern int setSocketReuseAddress(
  int socket);

extern int setSocketNoDelay(
  int socket);

extern int setSocketSplice(
  int fromSocket,
  int toSocket);

extern int setBidirectionalSplice(
  int socket1,
  int socket2);

extern off_t getSpliceBytesTransferred(
  int socket);

extern int getSocketError(
  int socket);

extern int signalSafeAccept(
  int sockfd,
  struct sockaddr* addr,
  socklen_t* addrlen);

#endif

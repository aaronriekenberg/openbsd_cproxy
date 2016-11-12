#ifndef PROXYSETTINGS_H
#define PROXYSETTINGS_H

#include "socketutil.h"
#include <sys/queue.h>

struct ServerAddrInfo
{
  struct addrinfo* addrinfo;
  SIMPLEQ_ENTRY(ServerAddrInfo) entry;
};

struct RemoteAddrInfo
{
  struct addrinfo* addrinfo;
  struct AddrPortStrings addrPortStrings;
};

struct ProxySettings
{
  SIMPLEQ_HEAD(,ServerAddrInfo) serverAddrInfoList;
  struct RemoteAddrInfo* remoteAddrInfoArray;
  size_t remoteAddrInfoArrayLength;
  uint32_t connectTimeoutMS;
};

extern const struct ProxySettings* processArgs(
  int argc,
  char** argv);

#endif

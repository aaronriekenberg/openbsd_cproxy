#ifndef PROXYSETTINGS_H
#define PROXYSETTINGS_H

#include "socketutil.h"
#include <sys/queue.h>

struct ServerAddrInfo
{
  struct addrinfo* addrinfo;
  SIMPLEQ_ENTRY(ServerAddrInfo) entry;
};

struct ProxySettings
{
  SIMPLEQ_HEAD(,ServerAddrInfo) serverAddrInfoList;
  struct addrinfo* remoteAddrInfo;
  struct AddrPortStrings remoteAddrPortStrings;
  uint32_t connectTimeoutMS;
};

extern const struct ProxySettings* processArgs(
  int argc,
  char** argv);

#endif

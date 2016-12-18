#ifndef PROXYSETTINGS_H
#define PROXYSETTINGS_H

#include "socketutil.h"
#include <stdbool.h>
#include <sys/queue.h>

struct ListenAddrInfo
{
  struct addrinfo* addrinfo;
  SIMPLEQ_ENTRY(ListenAddrInfo) entry;
};

struct RemoteAddrInfo
{
  struct addrinfo* addrinfo;
  struct AddrPortStrings addrPortStrings;
};

struct ProxySettings
{
  SIMPLEQ_HEAD(,ListenAddrInfo) listenAddrInfoList;
  struct RemoteAddrInfo* remoteAddrInfoArray;
  size_t remoteAddrInfoArrayLength;
  uint32_t connectTimeoutMS;
  uint32_t periodicLogMS;
  bool flushAfterLog;
};

const struct ProxySettings* processArgs(
  int argc,
  char** argv);

#endif

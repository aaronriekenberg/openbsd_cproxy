#include "log.h"
#include "memutil.h"
#include "proxysettings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_CONNECT_TIMEOUT_MS (5000)

static void printUsageAndExit()
{
  printf("Usage:\n"
         "  cproxy -l <local addr>:<local port>\n"
         "         [-l <local addr>:<local port>...]\n"
         "         -r <remote addr>:<remote port>\n"
         "         [-r <remote addr>:<remote port>...]\n"
         "         [-c <connect timeout milliseconds>]\n"
         "Arguments:\n"
         "  -c <connect timeout milliseconds>: specify connection timeout\n"
         "  -l <local addr>:<local port>: specify listen address and port\n"
         "  -r <remote addr>:<remote port>: specify remote address and port\n");
  exit(1);
}

static struct addrinfo* parseAddrPort(
  const char* optarg)
{
  struct addrinfo hints;
  struct addrinfo* addressInfo = NULL;
  char addressString[NI_MAXHOST];
  char portString[NI_MAXSERV];
  const size_t optargLen = strlen(optarg);
  size_t colonIndex;
  size_t hostLength;
  size_t portLength;
  int retVal;

  colonIndex = optargLen;
  while (colonIndex > 0)
  {
    --colonIndex;
    if (optarg[colonIndex] == ':')
    {
      break;
    }
  }

  if ((colonIndex <= 0) ||
      (colonIndex >= (optargLen - 1)))
  {
    proxyLog("invalid address:port argument: '%s'", optarg);
    goto fail;
  }

  hostLength = colonIndex;
  portLength = optargLen - colonIndex - 1;

  if ((hostLength >= NI_MAXHOST) ||
      (portLength >= NI_MAXSERV))
  {
    proxyLog("invalid address:port argument: '%s'", optarg);
    goto fail;
  }

  strncpy(addressString, optarg, hostLength);
  addressString[hostLength] = 0;

  strncpy(portString, &(optarg[colonIndex + 1]), portLength);
  portString[portLength] = 0;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_ADDRCONFIG;

  if (((retVal = getaddrinfo(addressString, portString,
                             &hints, &addressInfo)) != 0) ||
      (addressInfo == NULL))
  {
    proxyLog("error resolving address %s %s",
             optarg, gai_strerror(retVal));
    goto fail;
  }

  return addressInfo;

fail:
  exit(1);
}

static struct addrinfo* parseRemoteAddrPort(
  const char* optarg,
  struct AddrPortStrings* addrPortStrings)
{
  struct addrinfo* addressInfo = parseAddrPort(optarg);

  if (!addrInfoToNameAndPort(
        addressInfo,
        addrPortStrings))
  {
    exit(1);
  }

  return addressInfo;
}

static uint32_t parseConnectTimeoutMS(char* optarg)
{
  const char* errstr;
  const long long connectTimeoutMS = strtonum(optarg, 1, 60 * 1000, &errstr);
  if (errstr != NULL)
  {
    proxyLog("invalid connect timeout argument '%s': %s", optarg, errstr);
    exit(1);
  }
  return connectTimeoutMS;
}

const struct ProxySettings* processArgs(
  int argc,
  char** argv)
{
  int retVal;
  struct ServerAddrInfo* pServerAddrInfo;
  struct RemoteAddrInfo* pRemoteAddrInfo;
  size_t remoteAddrInfoArrayCapacity = 0;
  struct ProxySettings* proxySettings =
    checkedCallocOne(sizeof(struct ProxySettings));

  proxySettings->connectTimeoutMS = DEFAULT_CONNECT_TIMEOUT_MS;
  SIMPLEQ_INIT(&(proxySettings->serverAddrInfoList));
  do
  {
    retVal = getopt(argc, argv, "c:l:r:");
    switch (retVal)
    {
    case 'c':
      proxySettings->connectTimeoutMS = parseConnectTimeoutMS(optarg);
      break;

    case 'l':
      pServerAddrInfo = checkedCallocOne(sizeof(struct ServerAddrInfo));
      pServerAddrInfo->addrinfo = parseAddrPort(optarg);
      SIMPLEQ_INSERT_TAIL(
        &(proxySettings->serverAddrInfoList),
        pServerAddrInfo, entry);
      break;

    case 'r':
      ++(proxySettings->remoteAddrInfoArrayLength);

      if (proxySettings->remoteAddrInfoArrayLength > remoteAddrInfoArrayCapacity)
      {
        if (remoteAddrInfoArrayCapacity == 0)
        {
          remoteAddrInfoArrayCapacity = 2;
        }
        else
        {
          remoteAddrInfoArrayCapacity *= 2;
        }
        proxySettings->remoteAddrInfoArray = checkedReallocarray(
          proxySettings->remoteAddrInfoArray,
          remoteAddrInfoArrayCapacity,
          sizeof(struct RemoteAddrInfo));
      }

      pRemoteAddrInfo =
        proxySettings->remoteAddrInfoArray +
        proxySettings->remoteAddrInfoArrayLength - 1;
      pRemoteAddrInfo->addrinfo =
        parseRemoteAddrPort(
          optarg,
          &(pRemoteAddrInfo->addrPortStrings));
      break;

    case '?':
      goto fail;
      break;
    }
  }
  while (retVal != -1);

  if (SIMPLEQ_EMPTY(&(proxySettings->serverAddrInfoList)) ||
      (proxySettings->remoteAddrInfoArrayLength == 0))
  {
    goto fail;
  }

  return proxySettings;

fail:
  printUsageAndExit();
  return NULL;
}

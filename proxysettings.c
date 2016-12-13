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
         "         [-p <periodic log milliseconds>] [-f]\n"
         "Arguments:\n"
         "  -l <local addr>:<local port>: specify listen address and port\n"
         "  -r <remote addr>:<remote port>: specify remote address and port\n"
         "  -c <connect timeout milliseconds>: specify connection timeout\n"
         "  -p <periodic log milliseconds>: specify periodic log interval\n"
         "  -f: flush stdout on each log\n");
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

  memcpy(addressString, optarg, hostLength);
  addressString[hostLength] = 0;

  memcpy(portString, optarg + colonIndex + 1, portLength);
  portString[portLength] = 0;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_ADDRCONFIG;

  if ((retVal = getaddrinfo(addressString, portString,
                            &hints, &addressInfo)) != 0)
  {
    proxyLog("error resolving address %s %s",
             optarg, gai_strerror(retVal));
    goto fail;
  }

  return addressInfo;

fail:
  exit(1);
}

static void parseServerAddrPort(
  const char* optarg,
  struct ProxySettings* proxySettings)
{
  struct ServerAddrInfo* serverAddrInfo =
    checkedCallocOne(sizeof(struct ServerAddrInfo));

  serverAddrInfo->addrinfo = parseAddrPort(optarg);

  SIMPLEQ_INSERT_TAIL(
    &(proxySettings->serverAddrInfoList),
    serverAddrInfo, entry);
}

static void parseRemoteAddrPort(
  const char* optarg,
  struct ProxySettings* proxySettings,
  size_t* remoteAddrInfoArrayCapacity)
{
  struct addrinfo* addressInfo = parseAddrPort(optarg);

  while (addressInfo != NULL)
  {
    struct RemoteAddrInfo* remoteAddrInfo;

    ++(proxySettings->remoteAddrInfoArrayLength);

    proxySettings->remoteAddrInfoArray =
      resizeDynamicArray(
        proxySettings->remoteAddrInfoArray,
        proxySettings->remoteAddrInfoArrayLength,
        sizeof(struct RemoteAddrInfo),
        remoteAddrInfoArrayCapacity);

    remoteAddrInfo =
      proxySettings->remoteAddrInfoArray +
      proxySettings->remoteAddrInfoArrayLength - 1;

    remoteAddrInfo->addrinfo = addressInfo;

    if (!addrInfoToNameAndPort(
          addressInfo,
          &(remoteAddrInfo->addrPortStrings)))
    {
      goto fail;
    }

    addressInfo = addressInfo->ai_next;
  }

  return;

fail:
  exit(1);
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

static uint32_t parsePeriodicLogMS(char* optarg)
{
  const char* errstr;
  const long long periodicLogMS = strtonum(optarg, 1, 3600 * 1000, &errstr);
  if (errstr != NULL)
  {
    proxyLog("invalid periodic log timeout argument '%s': %s", optarg, errstr);
    exit(1);
  }
  return periodicLogMS;
}

const struct ProxySettings* processArgs(
  int argc,
  char** argv)
{
  int retVal;
  size_t remoteAddrInfoArrayCapacity = 0;
  struct ProxySettings* proxySettings =
    checkedCallocOne(sizeof(struct ProxySettings));

  proxySettings->connectTimeoutMS = DEFAULT_CONNECT_TIMEOUT_MS;
  SIMPLEQ_INIT(&(proxySettings->serverAddrInfoList));

  while ((retVal = getopt(argc, argv, "c:fl:p:r:")) != -1)
  {
    switch (retVal)
    {
    case 'c':
      proxySettings->connectTimeoutMS = parseConnectTimeoutMS(optarg);
      break;

    case 'f':
      proxySettings->flushAfterLog = true;
      break;

    case 'l':
      parseServerAddrPort(optarg, proxySettings);
      break;

    case 'p':
      proxySettings->periodicLogMS = parsePeriodicLogMS(optarg);
      break;

    case 'r':
      parseRemoteAddrPort(optarg, proxySettings, &remoteAddrInfoArrayCapacity);
      break;

    default:
      goto fail;
      break;
    }
  }

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

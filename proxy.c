/* cproxy - Copyright (C) 2012 Aaron Riekenberg (aaron.riekenberg@gmail.com).

   cproxy is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   cproxy is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with cproxy.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "errutil.h"
#include "fdutil.h"
#include "kqueue_pollutil.h"
#include "linkedlist.h"
#include "log.h"
#include "memutil.h"
#include "socketutil.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#define DEFAULT_NO_DELAY_SETTING (false)
#define MAX_OPERATIONS_FOR_ONE_FD (100)

static void printUsageAndExit()
{
  printf("Usage:\n");
  printf("  cproxy -l <local addr>:<local port>\n");
  printf("         [-l <local addr>:<local port>...]\n");
  printf("         -r <remote addr>:<remote port>\n");
  printf("         [-n]\n");
  printf("Arguments:\n");
  printf("  -l <local addr>:<local port>: specify listen address and port\n");
  printf("  -r <remote addr>:<remote port>: specify remote address and port\n");
  printf("  -n: enable TCP no delay\n");
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
    exit(1);
  }

  hostLength = colonIndex;
  portLength = optargLen - colonIndex - 1;

  if ((hostLength >= NI_MAXHOST) ||
      (portLength >= NI_MAXSERV))
  {
    proxyLog("invalid address:port argument: '%s'", optarg);
    exit(1);
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
    exit(1);
  }

  return addressInfo;
}

static struct addrinfo* parseRemoteAddrPort(
  const char* optarg,
  struct AddrPortStrings* addrPortStrings)
{
  struct addrinfo* addressInfo = parseAddrPort(optarg);
  
  if (addressToNameAndPort(
        addressInfo->ai_addr,
        addressInfo->ai_addrlen,
        addrPortStrings) < 0)
  {
    exit(1);
  }

  return addressInfo;
}

struct ProxySettings
{
  bool noDelay;
  struct LinkedList serverAddrInfoList;
  struct addrinfo* remoteAddrInfo;
  struct AddrPortStrings remoteAddrPortStrings;
};

static const struct ProxySettings* processArgs(
  int argc,
  char** argv)
{
  int retVal;
  bool foundLocalAddress = false;
  bool foundRemoteAddress = false;
  struct ProxySettings* proxySettings = 
    checkedCalloc(1, sizeof(struct ProxySettings));
  proxySettings->noDelay = DEFAULT_NO_DELAY_SETTING;
  initializeLinkedList(&(proxySettings->serverAddrInfoList));

  do
  {
    retVal = getopt(argc, argv, "l:nr:");
    switch (retVal)
    {
    case 'l':
      addToLinkedList(&(proxySettings->serverAddrInfoList),
                      parseAddrPort(optarg));
      foundLocalAddress = true;
      break;

    case 'n':
      proxySettings->noDelay = true;
      break;

    case 'r':
      if (foundRemoteAddress)
      {
        printUsageAndExit();
      }
      proxySettings->remoteAddrInfo =
        parseRemoteAddrPort(
          optarg,
          &(proxySettings->remoteAddrPortStrings));
      foundRemoteAddress = true;
      break;

    case '?':
      printUsageAndExit();
      break;
    }
  }
  while (retVal != -1);

  if ((!foundLocalAddress) || (!foundRemoteAddress))
  {
    printUsageAndExit();
  }

  return proxySettings;
}

static void setupSignals()
{
  struct sigaction newAction;
  memset(&newAction, 0, sizeof(struct sigaction));
  newAction.sa_handler = SIG_IGN;
  if (sigaction(SIGPIPE, &newAction, NULL) < 0)
  {
    proxyLog("sigaction error errno = %d", errno);
    exit(1);
  }
}

enum SocketInfoType
{
  SERVER_SOCKET_INFO_TYPE,
  CONNECTION_SOCKET_INFO_TYPE
};

struct AbstractSocketInfo
{
  enum SocketInfoType socketInfoType;
};

struct ServerSocketInfo
{
  enum SocketInfoType socketInfoType;
  int socket;
};

enum ConnectionSocketInfoType
{
  CLIENT_TO_PROXY,
  PROXY_TO_REMOTE
};

struct ConnectionSocketInfo
{
  enum SocketInfoType socketInfoType;
  int socket;
  enum ConnectionSocketInfoType type;
  bool waitingForConnect;
  bool waitingForRead;
  struct ConnectionSocketInfo* relatedConnectionSocketInfo;
  struct AddrPortStrings clientAddrPortStrings;
  struct AddrPortStrings serverAddrPortStrings;
};

static void setupServerSockets(
  const struct LinkedList* serverAddrInfoList,
  struct PollState* pollState)
{
  struct LinkedListNode* nodePtr;

  for (nodePtr = serverAddrInfoList->head;
       nodePtr;
       nodePtr = nodePtr->next)
  {
    const struct addrinfo* listenAddrInfo = nodePtr->data;
    struct AddrPortStrings serverAddrPortStrings;
    struct ServerSocketInfo* serverSocketInfo =
      checkedCalloc(1, sizeof(struct ServerSocketInfo));
    serverSocketInfo->socketInfoType = SERVER_SOCKET_INFO_TYPE;

    if (addressToNameAndPort(listenAddrInfo->ai_addr,
                             listenAddrInfo->ai_addrlen,
                             &serverAddrPortStrings) < 0)
    {
      proxyLog("error resolving server listen address");
      exit(1);
    }

    serverSocketInfo->socket = socket(listenAddrInfo->ai_family,
                                      listenAddrInfo->ai_socktype,
                                      listenAddrInfo->ai_protocol);
    if (serverSocketInfo->socket < 0)
    {
      proxyLog("error creating server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      exit(1);
    }

    if (setSocketReuseAddress(serverSocketInfo->socket) < 0)
    {
      proxyLog("setSocketReuseAddress error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      exit(1);
    }

    if (bind(serverSocketInfo->socket,
             listenAddrInfo->ai_addr,
             listenAddrInfo->ai_addrlen) < 0)
    {
      proxyLog("bind error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      exit(1);
    }

    if (setSocketListening(serverSocketInfo->socket) < 0)
    {
      proxyLog("listen error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      exit(1);
    }

    if (setFDNonBlocking(serverSocketInfo->socket) < 0)
    {
      proxyLog("error setting non-blocking on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      exit(1);
    }

    proxyLog("listening on %s:%s (fd=%d)", 
             serverAddrPortStrings.addrString,
             serverAddrPortStrings.portString,
             serverSocketInfo->socket);

    addPollFDForRead(
      pollState,
      serverSocketInfo->socket,
      serverSocketInfo);
  }
}

static void addConnectionSocketInfoToPollState(
  struct PollState* pollState,
  struct ConnectionSocketInfo* connectionSocketInfo)
{
  if (connectionSocketInfo->waitingForConnect)
  {
    addPollFDForWrite(
      pollState,
      connectionSocketInfo->socket,
      connectionSocketInfo);
  }
  if (connectionSocketInfo->waitingForRead)
  {
    addPollFDForRead(
      pollState,
      connectionSocketInfo->socket,
      connectionSocketInfo);
  }
}

static void removeConnectionSocketInfoFromPollState(
  struct PollState* pollState,
  struct ConnectionSocketInfo* connectionSocketInfo)
{
  if (connectionSocketInfo->waitingForConnect)
  {
    removePollFDForWrite(
      pollState,
      connectionSocketInfo->socket);
  }
  if (connectionSocketInfo->waitingForRead)
  {
    removePollFDForRead(
      pollState,
      connectionSocketInfo->socket);
  }
}

static bool setupClientSocket(
  int clientSocket,
  const struct ProxySettings* proxySettings,
  struct AddrPortStrings* clientAddrPortStrings,
  struct AddrPortStrings* proxyServerAddrPortStrings)
{
  struct sockaddr_storage clientAddress;
  socklen_t clientAddressSize;
  struct sockaddr_storage proxyServerAddress;
  socklen_t proxyServerAddressSize;

  if ((proxySettings->noDelay) && (setSocketNoDelay(clientSocket) < 0))
  {
    proxyLog("error setting no delay on accepted socket");
    return false;
  }

  clientAddressSize = sizeof(clientAddress);
  if (getpeername(
        clientSocket, 
        (struct sockaddr*)&clientAddress,
        &clientAddressSize) < 0)
  {
    proxyLog("getpeername error errno = %d", errno);
    return false;
  }

  if (addressToNameAndPort((struct sockaddr*)&clientAddress,
                           clientAddressSize,
                           clientAddrPortStrings) < 0)
  {
    proxyLog("error getting client address name and port");
    return false;
  }

  proxyServerAddressSize = sizeof(proxyServerAddress);
  if (getsockname(
        clientSocket, 
        (struct sockaddr*)&proxyServerAddress,
        &proxyServerAddressSize) < 0)
  {
    proxyLog("getsockname error errno = %d", errno);
    return false;
  }

  if (addressToNameAndPort((struct sockaddr*)&proxyServerAddress,
                           proxyServerAddressSize,
                           proxyServerAddrPortStrings) < 0)
  {
    proxyLog("error getting proxy server address name and port");
    return false;
  }

  proxyLog("connect client to proxy %s:%s -> %s:%s (fd=%d)",
           clientAddrPortStrings->addrString,
           clientAddrPortStrings->portString,
           proxyServerAddrPortStrings->addrString,
           proxyServerAddrPortStrings->portString,
           clientSocket);

  return true;
}

enum RemoteSocketStatus
{
  REMOTE_SOCKET_ERROR,
  REMOTE_SOCKET_CONNECTED,
  REMOTE_SOCKET_IN_PROGRESS
};

struct RemoteSocketResult
{
  enum RemoteSocketStatus status;
  int remoteSocket;
};

static struct RemoteSocketResult createRemoteSocket(
  int clientSocket,
  const struct ProxySettings* proxySettings,
  struct AddrPortStrings* proxyClientAddrPortStrings)
{
  int connectRetVal;
  struct sockaddr_storage proxyClientAddress;
  socklen_t proxyClientAddressSize;
  struct RemoteSocketResult result =
  {
    .status = REMOTE_SOCKET_ERROR,
    .remoteSocket =
       socket(proxySettings->remoteAddrInfo->ai_family,
              proxySettings->remoteAddrInfo->ai_socktype,
              proxySettings->remoteAddrInfo->ai_protocol)
  };
  if (result.remoteSocket < 0)
  {
    proxyLog("error creating remote socket errno = %d", errno);
    result.status = REMOTE_SOCKET_ERROR;
    result.remoteSocket = -1;
    return result;
  }

  if (setFDNonBlocking(result.remoteSocket) < 0)
  {
    proxyLog("error setting non-blocking on remote socket");
    signalSafeClose(result.remoteSocket);
    result.status = REMOTE_SOCKET_ERROR;
    result.remoteSocket = -1;
    return result;
  }

  connectRetVal = connect(
    result.remoteSocket,
    proxySettings->remoteAddrInfo->ai_addr,
    proxySettings->remoteAddrInfo->ai_addrlen);
  if ((connectRetVal < 0) &&
      ((errno == EINPROGRESS) ||
       (errno == EINTR)))
  {
    result.status = REMOTE_SOCKET_IN_PROGRESS;
  }
  else if (connectRetVal < 0)
  {
    char* socketErrorString = errnoToString(errno);
    proxyLog("remote socket connect error errno = %d: %s",
             errno, socketErrorString);
    free(socketErrorString);
    signalSafeClose(result.remoteSocket);
    result.status = REMOTE_SOCKET_ERROR;
    result.remoteSocket = -1;
    return result;
  }
  else
  {
    result.status = REMOTE_SOCKET_CONNECTED;
    if (setBidirectionalSplice(clientSocket, result.remoteSocket) < 0)
    {
      proxyLog("splice setup error");
      signalSafeClose(result.remoteSocket);
      result.status = REMOTE_SOCKET_ERROR;
      result.remoteSocket = -1;
      return result;
    }
  }

  if ((proxySettings->noDelay) && (setSocketNoDelay(result.remoteSocket) < 0))
  {
    proxyLog("error setting no delay on remote socket");
    signalSafeClose(result.remoteSocket);
    result.status = REMOTE_SOCKET_ERROR;
    result.remoteSocket = -1;
    return result;
  }

  proxyClientAddressSize = sizeof(proxyClientAddress);
  if (getsockname(
        result.remoteSocket,
        (struct sockaddr*)&proxyClientAddress,
        &proxyClientAddressSize) < 0)
  {
    proxyLog("getsockname error errno = %d", errno);
    signalSafeClose(result.remoteSocket);
    result.status = REMOTE_SOCKET_ERROR;
    result.remoteSocket = -1;
    return result;
  }

  if (addressToNameAndPort((struct sockaddr*)&proxyClientAddress,
                           proxyClientAddressSize,
                           proxyClientAddrPortStrings) < 0)
  {
    proxyLog("error getting proxy client address name and port");
    signalSafeClose(result.remoteSocket);
    result.status = REMOTE_SOCKET_ERROR;
    result.remoteSocket = -1;
    return result;
  }

  if (result.status != REMOTE_SOCKET_ERROR)
  {
    proxyLog("connect %s proxy to remote %s:%s -> %s:%s (fd=%d)",
             ((result.status == REMOTE_SOCKET_CONNECTED) ? "complete" : "starting"),
             proxyClientAddrPortStrings->addrString,
             proxyClientAddrPortStrings->portString,
             proxySettings->remoteAddrPortStrings.addrString,
             proxySettings->remoteAddrPortStrings.portString,
             result.remoteSocket);
  }

  return result;
}

static void handleNewClientSocket(
  int clientSocket,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  struct AddrPortStrings clientAddrPortStrings;
  struct AddrPortStrings proxyServerAddrPortStrings;
  struct AddrPortStrings proxyClientAddrPortStrings;
  if (!setupClientSocket(
        clientSocket,
        proxySettings,
        &clientAddrPortStrings,
        &proxyServerAddrPortStrings))
  {
    signalSafeClose(clientSocket);
  }
  else
  {
    const struct RemoteSocketResult remoteSocketResult =
      createRemoteSocket(clientSocket,
                         proxySettings,
                         &proxyClientAddrPortStrings);
    if (remoteSocketResult.status == REMOTE_SOCKET_ERROR)
    {
      signalSafeClose(clientSocket);
    }
    else
    {
      struct ConnectionSocketInfo* connInfo1;
      struct ConnectionSocketInfo* connInfo2;

      connInfo1 = checkedCalloc(1, sizeof(struct ConnectionSocketInfo));
      connInfo1->socketInfoType = CONNECTION_SOCKET_INFO_TYPE;
      connInfo1->socket = clientSocket;
      connInfo1->type = CLIENT_TO_PROXY;
      if (remoteSocketResult.status == REMOTE_SOCKET_CONNECTED)
      {
        connInfo1->waitingForConnect = false;
        connInfo1->waitingForRead = true;
      }
      else if (remoteSocketResult.status == REMOTE_SOCKET_IN_PROGRESS)
      {
        connInfo1->waitingForConnect = false;
        connInfo1->waitingForRead = false;
      }
      memcpy(&(connInfo1->clientAddrPortStrings),
             &clientAddrPortStrings,
             sizeof(struct AddrPortStrings));
      memcpy(&(connInfo1->serverAddrPortStrings),
             &proxyServerAddrPortStrings,
             sizeof(struct AddrPortStrings));

      connInfo2 = checkedCalloc(1, sizeof(struct ConnectionSocketInfo));
      connInfo2->socketInfoType = CONNECTION_SOCKET_INFO_TYPE;
      connInfo2->socket = remoteSocketResult.remoteSocket;
      connInfo2->type = PROXY_TO_REMOTE;
      if (remoteSocketResult.status == REMOTE_SOCKET_CONNECTED)
      {
        connInfo2->waitingForConnect = false;
        connInfo2->waitingForRead = true;
      }
      else if (remoteSocketResult.status == REMOTE_SOCKET_IN_PROGRESS)
      {
        connInfo2->waitingForConnect = true;
        connInfo2->waitingForRead = false;
      }
      memcpy(&(connInfo2->clientAddrPortStrings),
             &proxyClientAddrPortStrings,
             sizeof(struct AddrPortStrings));
      memcpy(&(connInfo2->serverAddrPortStrings),
             &(proxySettings->remoteAddrPortStrings),
             sizeof(struct AddrPortStrings));

      connInfo1->relatedConnectionSocketInfo = connInfo2;
      connInfo2->relatedConnectionSocketInfo = connInfo1;

      addConnectionSocketInfoToPollState(pollState, connInfo1);
      addConnectionSocketInfoToPollState(pollState, connInfo2);
    }
  }
}

static void printDisconnectMessage(
  const struct ConnectionSocketInfo* connectionSocketInfo)
{
  proxyLog("disconnect %s %s:%s -> %s:%s (fd=%d)",
           ((connectionSocketInfo->type == CLIENT_TO_PROXY) ?
            "client to proxy" :
            "proxy to remote"),
           connectionSocketInfo->clientAddrPortStrings.addrString,
           connectionSocketInfo->clientAddrPortStrings.portString,
           connectionSocketInfo->serverAddrPortStrings.addrString,
           connectionSocketInfo->serverAddrPortStrings.portString,
           connectionSocketInfo->socket);
}

static void destroyConnection(
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* relatedConnectionSocketInfo =
    connectionSocketInfo->relatedConnectionSocketInfo;

  printDisconnectMessage(connectionSocketInfo);
  removeConnectionSocketInfoFromPollState(pollState, connectionSocketInfo);
  signalSafeClose(connectionSocketInfo->socket);
  free(connectionSocketInfo);
  connectionSocketInfo = NULL;

  if (relatedConnectionSocketInfo)
  {
    relatedConnectionSocketInfo->relatedConnectionSocketInfo = NULL;
    destroyConnection(
      relatedConnectionSocketInfo,
      pollState);
  }
}

static struct ConnectionSocketInfo* handleConnectionReadyForError(
  struct ConnectionSocketInfo* connectionSocketInfo)
{
  struct ConnectionSocketInfo* pDisconnectSocketInfo = NULL;
  const int socketError = getSocketError(connectionSocketInfo->socket);
  if (socketError != 0)
  {
    char* socketErrorString = errnoToString(socketError);

    pDisconnectSocketInfo = connectionSocketInfo;

    proxyLog("fd %d errno %d: %s",
             connectionSocketInfo->socket,
             socketError,
             socketErrorString);

    free(socketErrorString);
  }

  return pDisconnectSocketInfo;
}

static struct ConnectionSocketInfo* handleConnectionReadyForRead(
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* pDisconnectSocketInfo = NULL;

  if (connectionSocketInfo->waitingForRead)
  {
    proxyLog("splice read error fd %d", connectionSocketInfo->socket);
    pDisconnectSocketInfo = connectionSocketInfo;
  }

  return pDisconnectSocketInfo;
}

static struct ConnectionSocketInfo* handleConnectionReadyForWrite(
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* pDisconnectSocketInfo = NULL;
  struct ConnectionSocketInfo* relatedConnectionSocketInfo =
    connectionSocketInfo->relatedConnectionSocketInfo;

  if (connectionSocketInfo->waitingForConnect)
  {
    int socketError;

    assert(relatedConnectionSocketInfo != NULL);

    socketError = getSocketError(connectionSocketInfo->socket);
    if (socketError == 0)
    {
      proxyLog("connect complete proxy to remote %s:%s -> %s:%s (fd=%d)",
               connectionSocketInfo->clientAddrPortStrings.addrString,
               connectionSocketInfo->clientAddrPortStrings.portString,
               connectionSocketInfo->serverAddrPortStrings.addrString,
               connectionSocketInfo->serverAddrPortStrings.portString,
               connectionSocketInfo->socket);
      if (setBidirectionalSplice(
            connectionSocketInfo->socket,
            relatedConnectionSocketInfo->socket) < 0)
      {
        proxyLog("splice setup error");
        pDisconnectSocketInfo = connectionSocketInfo;
      }
      else
      {
        removeConnectionSocketInfoFromPollState(pollState, connectionSocketInfo);
        removeConnectionSocketInfoFromPollState(pollState, relatedConnectionSocketInfo);

        connectionSocketInfo->waitingForConnect = false;
        connectionSocketInfo->waitingForRead = true;
        addConnectionSocketInfoToPollState(pollState, connectionSocketInfo);

        relatedConnectionSocketInfo->waitingForConnect = false;
        relatedConnectionSocketInfo->waitingForRead = true;
        addConnectionSocketInfoToPollState(pollState, relatedConnectionSocketInfo);
      }
    }
    else if (socketError == EINPROGRESS)
    {
      /* do nothing, still in progress */
    }
    else
    {
      char* socketErrorString = errnoToString(socketError);
      proxyLog("async remote connect fd %d errno %d: %s",
               connectionSocketInfo->socket,
               socketError,
               socketErrorString);
      free(socketErrorString);
      pDisconnectSocketInfo = connectionSocketInfo;
    }
  }

  return pDisconnectSocketInfo;
}

enum HandleConnectionReadyResult
{
  POLL_STATE_INVALIDATED_RESULT,
  POLL_STATE_NOT_INVALIDATED_RESULT
};

static enum HandleConnectionReadyResult handleConnectionReady(
  const struct ReadyFDInfo* readyFDInfo,
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState)
{
  enum HandleConnectionReadyResult handleConnectionReadyResult =
    POLL_STATE_NOT_INVALIDATED_RESULT;
  struct ConnectionSocketInfo* pDisconnectSocketInfo = NULL;

/*#ifdef DEBUG_PROXY*/
  proxyLog("fd %d readyForRead %d readyForWrite %d readyForError %d",
           connectionSocketInfo->socket,
           readyFDInfo->readyForRead,
           readyFDInfo->readyForWrite,
           readyFDInfo->readyForError);
/*#endif*/

  if (readyFDInfo->readyForError &&
      (!pDisconnectSocketInfo))
  {
    pDisconnectSocketInfo = 
      handleConnectionReadyForError(
        connectionSocketInfo);
  }

  if (readyFDInfo->readyForRead &&
      (!pDisconnectSocketInfo))
  {
    pDisconnectSocketInfo =
      handleConnectionReadyForRead(
        connectionSocketInfo,
        pollState);
  }

  if (readyFDInfo->readyForWrite &&
      (!pDisconnectSocketInfo))
  {
    pDisconnectSocketInfo =
      handleConnectionReadyForWrite(
        connectionSocketInfo,
        pollState);
  }

  if (pDisconnectSocketInfo)
  {
    destroyConnection(
      pDisconnectSocketInfo,
      pollState);
    handleConnectionReadyResult = POLL_STATE_INVALIDATED_RESULT;
  }

  return handleConnectionReadyResult;
}

static void handleServerSocketReady(
  const struct ServerSocketInfo* serverSocketInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  bool acceptError = false;
  int numAccepts = 0;
  while ((!acceptError) &&
         (numAccepts < MAX_OPERATIONS_FOR_ONE_FD))
  {
    const int acceptedFD = signalSafeAccept(serverSocketInfo->socket, NULL, NULL);
    ++numAccepts;
    if (acceptedFD < 0)
    {
      if ((errno != EAGAIN) && (errno != EWOULDBLOCK))
      {
        proxyLog("accept error errno %d", errno);
      }
      acceptError = true;
    }
    else
    {
      proxyLog("accepted fd %d", acceptedFD);
      handleNewClientSocket(
        acceptedFD,
        proxySettings,
        pollState);
    }
  }
}

static void runProxy(
  const struct ProxySettings* proxySettings)
{
  struct PollState pollState;

  proxyLog("remote address = %s:%s",
           proxySettings->remoteAddrPortStrings.addrString,
           proxySettings->remoteAddrPortStrings.portString);
  proxyLog("no delay = %d",
           (unsigned int)(proxySettings->noDelay));

  setupSignals();

  initializePollState(&pollState);

  setupServerSockets(
    &(proxySettings->serverAddrInfoList),
    &pollState);

  while (true)
  {
    size_t i;
    bool pollStateInvalidated = false;
    const struct PollResult* pollResult = blockingPoll(&pollState);
    if (!pollResult)
    {
      proxyLog("blockingPoll failed");
      abort();
    }

    for (i = 0; 
         (!pollStateInvalidated) &&
         (i < pollResult->numReadyFDs);
         ++i)
    {
      struct ReadyFDInfo* readyFDInfo =
        &(pollResult->readyFDInfoArray[i]);
      struct AbstractSocketInfo* pAbstractSocketInfo = readyFDInfo->data;
      if (pAbstractSocketInfo->socketInfoType == SERVER_SOCKET_INFO_TYPE)
      {
        handleServerSocketReady(
          (struct ServerSocketInfo*)pAbstractSocketInfo,
          proxySettings,
          &pollState);
      }
      else if (pAbstractSocketInfo->socketInfoType == CONNECTION_SOCKET_INFO_TYPE)
      {
        if (handleConnectionReady(
              readyFDInfo,
              (struct ConnectionSocketInfo*)pAbstractSocketInfo,
              &pollState) == POLL_STATE_INVALIDATED_RESULT)
        {
          pollStateInvalidated = true;
        }
      }
    }
  }
}

int main( 
  int argc,
  char** argv)
{
  const struct ProxySettings* proxySettings;

  proxyLogSetThreadName("main");

  proxySettings = processArgs(argc, argv);
  runProxy(proxySettings);

  return 0;
}

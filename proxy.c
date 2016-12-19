#include "errutil.h"
#include "fdutil.h"
#include "log.h"
#include "memutil.h"
#include "pollutil.h"
#include "proxysettings.h"
#include "socketutil.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/queue.h>

#define MAX_OPERATIONS_FOR_ONE_FD (100)

#define PERIODIC_TIMER_ID (UINTPTR_MAX)

struct AbstractSocketInfo;

typedef void (*HandleConnectionReadyFunction)(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyEventInfo* readyEventInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState);

struct AbstractSocketInfo
{
  HandleConnectionReadyFunction handleConnectionReadyFunction;
};

struct ServerSocketInfo
{
  HandleConnectionReadyFunction handleConnectionReadyFunction;
  int socket;
};

static void handleServerSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyEventInfo* readyEventInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState);

enum ConnectionSocketInfoType
{
  CLIENT_TO_PROXY,
  PROXY_TO_REMOTE
};

struct ConnectionSocketInfo
{
  HandleConnectionReadyFunction handleConnectionReadyFunction;
  int socket;
  enum ConnectionSocketInfoType type;
  bool markedForDestruction;
  bool waitingForConnect;
  bool waitingForRead;
  struct ConnectionSocketInfo* relatedConnectionSocketInfo;
  struct AddrPortStrings clientAddrPortStrings;
  struct AddrPortStrings serverAddrPortStrings;
  TAILQ_ENTRY(ConnectionSocketInfo) entry;
};

static TAILQ_HEAD(,ConnectionSocketInfo) connectionSocketInfoList =
  TAILQ_HEAD_INITIALIZER(connectionSocketInfoList);

static TAILQ_HEAD(,ConnectionSocketInfo) destroyConnectionSocketInfoList =
  TAILQ_HEAD_INITIALIZER(destroyConnectionSocketInfoList);

static void handleConnectionSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyEventInfo* readyEventInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState);

static void setupServerSockets(
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  const struct ListenAddrInfo* listenAddrInfo;

  SIMPLEQ_FOREACH(listenAddrInfo, &(proxySettings->listenAddrInfoList), entry)
  {
    struct AddrPortStrings serverAddrPortStrings;
    struct ServerSocketInfo* serverSocketInfo =
      checkedCallocOne(sizeof(struct ServerSocketInfo));
    serverSocketInfo->handleConnectionReadyFunction = handleServerSocketReady;

    if (!addrInfoToNameAndPort(listenAddrInfo->addrinfo,
                               &serverAddrPortStrings))
    {
      proxyLog("error resolving server listen address");
      goto fail;
    }

    if (!createNonBlockingSocket(
           listenAddrInfo->addrinfo,
           &(serverSocketInfo->socket)))
    {
      proxyLog("error creating server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
    }

    if (!setSocketReuseAddress(serverSocketInfo->socket))
    {
      proxyLog("setSocketReuseAddress error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
    }

    if (!bindSocket(serverSocketInfo->socket, listenAddrInfo->addrinfo))
    {
      proxyLog("bind error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
    }

    if (!setSocketListening(serverSocketInfo->socket))
    {
      proxyLog("listen error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
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

  return;

fail:
  exit(1);
}

static void addConnectionSocketInfoToPollState(
  struct PollState* pollState,
  struct ConnectionSocketInfo* connectionSocketInfo,
  const struct ProxySettings* proxySettings)
{
  if (connectionSocketInfo->waitingForConnect)
  {
    addPollFDForWriteAndTimeout(
      pollState,
      connectionSocketInfo->socket,
      connectionSocketInfo,
      proxySettings->connectTimeoutMS);
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
  const struct ConnectionSocketInfo* connectionSocketInfo)
{
  if (connectionSocketInfo->waitingForConnect)
  {
    removePollFDForWriteAndTimeout(
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

static bool getClientSocketAddresses(
  const int clientSocket,
  const struct SockAddrInfo* clientSockAddrInfo,
  struct AddrPortStrings* clientAddrPortStrings,
  struct AddrPortStrings* serverAddrPortStrings)
{
  struct SockAddrInfo serverSockAddrInfo;

  if (!sockAddrInfoToNameAndPort(clientSockAddrInfo,
                                 clientAddrPortStrings))
  {
    proxyLog("error getting client adddress port strings");
    goto fail;
  }

  if (!getSocketName(clientSocket,
                     &serverSockAddrInfo))
  {
    proxyLog("client getsockname error errno = %d: %s",
             errno, errnoToString(errno));
    goto fail;
  }

  if (!sockAddrInfoToNameAndPort(&serverSockAddrInfo,
                                 serverAddrPortStrings))
  {
    proxyLog("error getting proxy server address port strings");
    goto fail;
  }

  proxyLog("connect client to proxy %s:%s -> %s:%s (fd=%d)",
           clientAddrPortStrings->addrString,
           clientAddrPortStrings->portString,
           serverAddrPortStrings->addrString,
           serverAddrPortStrings->portString,
           clientSocket);

  return true;

fail:
  return false;
}

static const struct RemoteAddrInfo* chooseRemoteAddrInfo(
  const struct ProxySettings* proxySettings)
{
  const size_t remoteAddrInfoIndex =
    arc4random_uniform(
      proxySettings->remoteAddrInfoArrayLength);
  const struct RemoteAddrInfo* remoteAddrInfo =
    proxySettings->remoteAddrInfoArray + remoteAddrInfoIndex;

  proxyLog("remote address %s:%s (index=%zu)",
           remoteAddrInfo->addrPortStrings.addrString,
           remoteAddrInfo->addrPortStrings.portString,
           remoteAddrInfoIndex);

  return remoteAddrInfo;
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
  const int clientSocket,
  const struct RemoteAddrInfo* remoteAddrInfo,
  struct AddrPortStrings* proxyClientAddrPortStrings)
{
  enum ConnectSocketResult connectSocketResult;
  struct SockAddrInfo proxyClientSockAddrInfo;
  struct RemoteSocketResult result;
  result.status = REMOTE_SOCKET_ERROR;

  if (!createNonBlockingSocket(
         remoteAddrInfo->addrinfo,
         &(result.remoteSocket)))
  {
    proxyLog("error creating remote socket errno = %d", errno);
    goto fail;
  }

  connectSocketResult = connectSocket(result.remoteSocket,
                                      remoteAddrInfo->addrinfo);
  if (connectSocketResult == CONNECT_SOCKET_RESULT_IN_PROGRESS)
  {
    result.status = REMOTE_SOCKET_IN_PROGRESS;
  }
  else if (connectSocketResult == CONNECT_SOCKET_RESULT_ERROR)
  {
    proxyLog("remote socket connect error errno = %d: %s",
             errno, errnoToString(errno));
    goto failWithSocket;
  }
  else
  {
    result.status = REMOTE_SOCKET_CONNECTED;
    if (!setBidirectionalSplice(clientSocket, result.remoteSocket))
    {
      proxyLog("splice setup error");
      goto failWithSocket;
    }
  }

  if (!getSocketName(result.remoteSocket, 
                     &proxyClientSockAddrInfo))
  {
    proxyLog("remote getsockname error errno = %d: %s",
             errno, errnoToString(errno));
    goto failWithSocket;
  }

  if (!sockAddrInfoToNameAndPort(&proxyClientSockAddrInfo,
                                 proxyClientAddrPortStrings))
  {
    proxyLog("error getting proxy client address name and port");
    goto failWithSocket;
  }

  proxyLog("connect %s proxy to remote %s:%s -> %s:%s (fd=%d)",
           ((result.status == REMOTE_SOCKET_CONNECTED) ?
            "complete" :
            "starting"),
           proxyClientAddrPortStrings->addrString,
           proxyClientAddrPortStrings->portString,
           remoteAddrInfo->addrPortStrings.addrString,
           remoteAddrInfo->addrPortStrings.portString,
           result.remoteSocket);

  return result;

failWithSocket:
  signalSafeClose(result.remoteSocket);

fail:
  result.status = REMOTE_SOCKET_ERROR;
  result.remoteSocket = -1;
  return result;
}

static void handleNewClientSocket(
  const int clientSocket,
  const struct SockAddrInfo* clientSockAddrInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  struct RemoteSocketResult remoteSocketResult;
  struct ConnectionSocketInfo* connInfo1 =
    checkedCallocOne(sizeof(struct ConnectionSocketInfo));
  struct ConnectionSocketInfo* connInfo2 = NULL;
  const struct RemoteAddrInfo* remoteAddrInfo;

  connInfo1->handleConnectionReadyFunction = handleConnectionSocketReady;
  connInfo1->type = CLIENT_TO_PROXY;
  connInfo1->socket = clientSocket;

  if (!getClientSocketAddresses(
         clientSocket,
         clientSockAddrInfo,
         &(connInfo1->clientAddrPortStrings),
         &(connInfo1->serverAddrPortStrings)))
  {
    goto fail;
  }

  connInfo2 = checkedCallocOne(sizeof(struct ConnectionSocketInfo));
  connInfo2->handleConnectionReadyFunction = handleConnectionSocketReady;
  connInfo2->type = PROXY_TO_REMOTE;

  remoteAddrInfo = chooseRemoteAddrInfo(proxySettings);

  remoteSocketResult =
    createRemoteSocket(clientSocket,
                       remoteAddrInfo,
                       &(connInfo2->clientAddrPortStrings));
  if (remoteSocketResult.status == REMOTE_SOCKET_ERROR)
  {
    goto fail;
  }
  connInfo2->socket = remoteSocketResult.remoteSocket;

  memcpy(&(connInfo2->serverAddrPortStrings),
         &(remoteAddrInfo->addrPortStrings),
         sizeof(struct AddrPortStrings));

  if (remoteSocketResult.status == REMOTE_SOCKET_CONNECTED)
  {
    connInfo1->waitingForRead = true;
    connInfo2->waitingForRead = true;
  }
  else if (remoteSocketResult.status == REMOTE_SOCKET_IN_PROGRESS)
  {
    connInfo2->waitingForConnect = true;
  }

  connInfo1->relatedConnectionSocketInfo = connInfo2;
  connInfo2->relatedConnectionSocketInfo = connInfo1;

  addConnectionSocketInfoToPollState(pollState, connInfo1, proxySettings);
  addConnectionSocketInfoToPollState(pollState, connInfo2, proxySettings);

  TAILQ_INSERT_TAIL(
    &connectionSocketInfoList,
    connInfo1,
    entry);

  TAILQ_INSERT_TAIL(
    &connectionSocketInfoList,
    connInfo2,
    entry);

  return;

fail:
  free(connInfo1);
  free(connInfo2);
  signalSafeClose(clientSocket);
}

static void markForDestruction(
  struct ConnectionSocketInfo* connectionSocketInfo)
{
  struct ConnectionSocketInfo* relatedConnectionSocketInfo = 
    connectionSocketInfo->relatedConnectionSocketInfo;

  if (!connectionSocketInfo->markedForDestruction)
  {
    connectionSocketInfo->markedForDestruction = true;
    TAILQ_REMOVE(
      &connectionSocketInfoList,
      connectionSocketInfo,
      entry);
    TAILQ_INSERT_TAIL(
      &destroyConnectionSocketInfoList,
      connectionSocketInfo,
      entry);
  }

  if (!relatedConnectionSocketInfo->markedForDestruction)
  {
    relatedConnectionSocketInfo->markedForDestruction = true;
    TAILQ_REMOVE(
      &connectionSocketInfoList,
      relatedConnectionSocketInfo,
      entry);
    TAILQ_INSERT_TAIL(
      &destroyConnectionSocketInfoList,
      relatedConnectionSocketInfo,
      entry);
  }
}

static void printDisconnectMessage(
  const struct ConnectionSocketInfo* connectionSocketInfo)
{
  const char* typeString =
    ((connectionSocketInfo->type == CLIENT_TO_PROXY) ?
     "client to proxy" :
     "proxy to remote");
  proxyLog("disconnect %s %s:%s -> %s:%s (fd=%d,bytes=%jd)",
           typeString,
           connectionSocketInfo->clientAddrPortStrings.addrString,
           connectionSocketInfo->clientAddrPortStrings.portString,
           connectionSocketInfo->serverAddrPortStrings.addrString,
           connectionSocketInfo->serverAddrPortStrings.portString,
           connectionSocketInfo->socket,
           (intmax_t)getSpliceBytesTransferred(connectionSocketInfo->socket));
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

  if (relatedConnectionSocketInfo != NULL)
  {
    relatedConnectionSocketInfo->relatedConnectionSocketInfo = NULL;
  }
}

static void destroyMarkedConnections(
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* connectionSocketInfo;

  while ((connectionSocketInfo =
          TAILQ_FIRST(&destroyConnectionSocketInfoList)) != NULL)
  {
    TAILQ_REMOVE(&destroyConnectionSocketInfoList, connectionSocketInfo, entry);
    destroyConnection(connectionSocketInfo, pollState);
  }
}

static struct ConnectionSocketInfo* handleConnectionReadyForRead(
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* disconnectSocketInfo = NULL;

  if (connectionSocketInfo->waitingForRead)
  {
    proxyLog("splice read error fd %d", connectionSocketInfo->socket);
    disconnectSocketInfo = connectionSocketInfo;
  }

  return disconnectSocketInfo;
}

static struct ConnectionSocketInfo* handleConnectionReadyForWrite(
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState,
  const struct ProxySettings* proxySettings)
{
  struct ConnectionSocketInfo* relatedConnectionSocketInfo =
    connectionSocketInfo->relatedConnectionSocketInfo;

  if (connectionSocketInfo->waitingForConnect)
  {
    const int socketError = getSocketError(connectionSocketInfo->socket);
    if (socketError == EINPROGRESS)
    {
      /* do nothing, still in progress */
    }
    else if (socketError != 0)
    {
      proxyLog("async remote connect fd %d errno %d: %s",
               connectionSocketInfo->socket,
               socketError,
               errnoToString(socketError));
      goto fail;
    }
    else
    {
      proxyLog("connect complete proxy to remote %s:%s -> %s:%s (fd=%d)",
               connectionSocketInfo->clientAddrPortStrings.addrString,
               connectionSocketInfo->clientAddrPortStrings.portString,
               connectionSocketInfo->serverAddrPortStrings.addrString,
               connectionSocketInfo->serverAddrPortStrings.portString,
               connectionSocketInfo->socket);

      if (!setBidirectionalSplice(
             connectionSocketInfo->socket,
             relatedConnectionSocketInfo->socket))
      {
        proxyLog("splice setup error");
        goto fail;
      }

      removeConnectionSocketInfoFromPollState(
        pollState, connectionSocketInfo);
      removeConnectionSocketInfoFromPollState(
        pollState, relatedConnectionSocketInfo);

      connectionSocketInfo->waitingForConnect = false;
      connectionSocketInfo->waitingForRead = true;
      addConnectionSocketInfoToPollState(
        pollState, connectionSocketInfo, proxySettings);

      relatedConnectionSocketInfo->waitingForConnect = false;
      relatedConnectionSocketInfo->waitingForRead = true;
      addConnectionSocketInfoToPollState(
        pollState, relatedConnectionSocketInfo, proxySettings);
    }
  }

  return NULL;

fail:
  return connectionSocketInfo;
}

static struct ConnectionSocketInfo* handleConnectionReadyForTimeout(
  struct ConnectionSocketInfo* connectionSocketInfo,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* disconnectSocketInfo = NULL;

  if (connectionSocketInfo->waitingForConnect)
  {
    proxyLog("connect timeout fd %d", connectionSocketInfo->socket);
    disconnectSocketInfo = connectionSocketInfo;
  }

  return disconnectSocketInfo;
}

static void handleConnectionSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyEventInfo* readyEventInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* connectionSocketInfo =
    (struct ConnectionSocketInfo*) abstractSocketInfo;
  struct ConnectionSocketInfo* disconnectSocketInfo = NULL;

#ifdef DEBUG_PROXY
  proxyLog("fd %d readyForRead %d readyForWrite %d readyForTimeout %d markedForDestruction %d",
           connectionSocketInfo->socket,
           readyEventInfo->readyForRead,
           readyEventInfo->readyForWrite,
           readyEventInfo->readyForTimeout,
           connectionSocketInfo->markedForDestruction);
#endif

  if (connectionSocketInfo->markedForDestruction)
  {
    disconnectSocketInfo = connectionSocketInfo;
  }

  if (readyEventInfo->readyForRead &&
      (disconnectSocketInfo == NULL))
  {
    disconnectSocketInfo =
      handleConnectionReadyForRead(
        connectionSocketInfo,
        pollState);
  }

  if (readyEventInfo->readyForWrite &&
      (disconnectSocketInfo == NULL))
  {
    disconnectSocketInfo =
      handleConnectionReadyForWrite(
        connectionSocketInfo,
        pollState,
        proxySettings);
  }

  if (readyEventInfo->readyForTimeout &&
      (disconnectSocketInfo == NULL))
  {
    disconnectSocketInfo =
      handleConnectionReadyForTimeout(
        connectionSocketInfo,
        pollState);
  }

  if (disconnectSocketInfo != NULL)
  {
    markForDestruction(disconnectSocketInfo);
  }
}

static void handleServerSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyEventInfo* readyEventInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  struct ServerSocketInfo* serverSocketInfo =
    (struct ServerSocketInfo*) abstractSocketInfo;
  enum AcceptSocketResult acceptSocketResult = ACCEPT_SOCKET_RESULT_SUCCESS;
  int i;

  for (i = 0;
       (acceptSocketResult == ACCEPT_SOCKET_RESULT_SUCCESS) &&
       (i < MAX_OPERATIONS_FOR_ONE_FD);
       ++i)
  {
    int acceptedFD;
    struct SockAddrInfo clientSockAddrInfo;

    acceptSocketResult = acceptSocket(
      serverSocketInfo->socket,
      &acceptedFD,
      &clientSockAddrInfo);

    if (acceptSocketResult == ACCEPT_SOCKET_RESULT_ERROR)
    {
      proxyLog("accept error errno %d: %s", errno, errnoToString(errno));
    }
    else if (acceptSocketResult == ACCEPT_SOCKET_RESULT_SUCCESS)
    {
      proxyLog("accept fd %d", acceptedFD);
      handleNewClientSocket(
        acceptedFD,
        &clientSockAddrInfo,
        proxySettings,
        pollState);
    }
  }
}

static void periodicTimerPop()
{
  const struct ConnectionSocketInfo* connectionSocketInfo;
  bool foundConnection = false;

  TAILQ_FOREACH(connectionSocketInfo, &connectionSocketInfoList, entry)
  {
    if (!foundConnection)
    {
      proxyLog("Active connections: [");
    }

    proxyLogNoTime("  fd=%d rfd=%d cw=%d rw=%d %s:%s -> %s:%s bytes=%jd",
                   connectionSocketInfo->socket,
                   connectionSocketInfo->relatedConnectionSocketInfo->socket,
                   connectionSocketInfo->waitingForConnect,
                   connectionSocketInfo->waitingForRead,
                   connectionSocketInfo->clientAddrPortStrings.addrString,
                   connectionSocketInfo->clientAddrPortStrings.portString,
                   connectionSocketInfo->serverAddrPortStrings.addrString,
                   connectionSocketInfo->serverAddrPortStrings.portString,
                   (intmax_t)getSpliceBytesTransferred(
                               connectionSocketInfo->socket));

    foundConnection = true;
  }

  if (foundConnection)
  {
    proxyLogNoTime("]");
  }
}

static void logSettings(
  const struct ProxySettings* proxySettings)
{
  size_t i;

  proxyLog("log flush stdout = %s",
           (proxySettings->flushAfterLog ? "true" : "false"));

  proxyLog("num remote addresses = %zu",
           proxySettings->remoteAddrInfoArrayLength);
  for (i = 0; i < proxySettings->remoteAddrInfoArrayLength; ++i)
  {
    proxyLog("remote address [%zu] = %s:%s", i,
             proxySettings->remoteAddrInfoArray[i].addrPortStrings.addrString,
             proxySettings->remoteAddrInfoArray[i].addrPortStrings.portString);
  }
  proxyLog("connect timeout milliseconds = %d",
           proxySettings->connectTimeoutMS);
  proxyLog("periodic log milliseconds = %d",
           proxySettings->periodicLogMS);
}

static void runProxy(
  const struct ProxySettings* proxySettings)
{
  struct PollState* pollState;

  proxyLogSetFlush(proxySettings->flushAfterLog);

  logSettings(proxySettings);

  pollState = newPollState();

  setupServerSockets(
    proxySettings,
    pollState);

  if (proxySettings->periodicLogMS > 0)
  {
    addPollIDForPeriodicTimer(
      pollState,
      PERIODIC_TIMER_ID,
      NULL,
      proxySettings->periodicLogMS);
  }

  while (true)
  {
    const struct PollResult* pollResult = blockingPoll(pollState);
    const struct ReadyEventInfo* readyEventInfo =
      pollResult->readyEventInfoArray;
    const struct ReadyEventInfo* endReadyEventInfo =
      readyEventInfo + pollResult->numReadyEvents;

    for (; readyEventInfo != endReadyEventInfo;
         ++readyEventInfo)
    {
      if (readyEventInfo->id == PERIODIC_TIMER_ID)
      {
        periodicTimerPop();
      }
      else
      {
        struct AbstractSocketInfo* abstractSocketInfo = readyEventInfo->data;
        (*(abstractSocketInfo->handleConnectionReadyFunction))(
          abstractSocketInfo, readyEventInfo, proxySettings, pollState);
      }
    }

    destroyMarkedConnections(pollState);
  }
}

static void setupInitialPledge()
{
  if (pledge("stdio inet dns", NULL) == -1)
  {
    proxyLog("initial pledge failed");
    abort();
  }
}

static void setupSignals()
{
  signal(SIGPIPE, SIG_IGN);
}

static void setupRunLoopPledge()
{
  if (pledge("stdio inet", NULL) == -1)
  {
    proxyLog("run loop pledge failed");
    abort();
  }
}

int main(
  int argc,
  char** argv)
{
  const struct ProxySettings* proxySettings;

  setupInitialPledge();

  setupSignals();

  proxySettings = processArgs(argc, argv);

  setupRunLoopPledge();

  runProxy(proxySettings);

  return 0;
}

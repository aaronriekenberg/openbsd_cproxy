#include "errutil.h"
#include "fdutil.h"
#include "log.h"
#include "memutil.h"
#include "pollutil.h"
#include "proxysettings.h"
#include "socketutil.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define MAX_OPERATIONS_FOR_ONE_FD (100)

enum HandleConnectionReadyResult
{
  POLL_STATE_INVALIDATED_RESULT,
  POLL_STATE_NOT_INVALIDATED_RESULT
};

struct AbstractSocketInfo;

typedef enum HandleConnectionReadyResult (*HandleConnectionReadyFunction)(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyFDInfo* readyFDInfo,
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

static enum HandleConnectionReadyResult handleServerSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyFDInfo* readyFDInfo,
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
  bool waitingForConnect;
  bool waitingForRead;
  struct ConnectionSocketInfo* relatedConnectionSocketInfo;
  struct AddrPortStrings clientAddrPortStrings;
  struct AddrPortStrings serverAddrPortStrings;
};

static enum HandleConnectionReadyResult handleConnectionSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyFDInfo* readyFDInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState);

static void setupServerSockets(
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  const struct ServerAddrInfo* pServerAddrInfo;

  SIMPLEQ_FOREACH(pServerAddrInfo, &(proxySettings->serverAddrInfoList), entry)
  {
    const struct addrinfo* listenAddrInfo = pServerAddrInfo->addrinfo;
    struct AddrPortStrings serverAddrPortStrings;
    struct ServerSocketInfo* serverSocketInfo =
      checkedCallocOne(sizeof(struct ServerSocketInfo));
    serverSocketInfo->handleConnectionReadyFunction = handleServerSocketReady;

    if (!addrInfoToNameAndPort(listenAddrInfo,
                               &serverAddrPortStrings))
    {
      proxyLog("error resolving server listen address");
      goto fail;
    }

    if (!createNonBlockingSocket(
           listenAddrInfo,
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

    if (!bindSocket(serverSocketInfo->socket, listenAddrInfo))
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
  struct ConnectionSocketInfo* connectionSocketInfo)
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
    &(proxySettings->remoteAddrInfoArray[remoteAddrInfoIndex]);

  proxyLog("remote address %s:%s (index=%ld)",
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

  return;

fail:
  free(connInfo1);
  free(connInfo2);
  signalSafeClose(clientSocket);
}

static void printDisconnectMessage(
  const struct ConnectionSocketInfo* connectionSocketInfo)
{
  const char* typeString =
    ((connectionSocketInfo->type == CLIENT_TO_PROXY) ?
     "client to proxy" :
     "proxy to remote");
  proxyLog("disconnect %s %s:%s -> %s:%s (fd=%d,bytes=%ld)",
           typeString,
           connectionSocketInfo->clientAddrPortStrings.addrString,
           connectionSocketInfo->clientAddrPortStrings.portString,
           connectionSocketInfo->serverAddrPortStrings.addrString,
           connectionSocketInfo->serverAddrPortStrings.portString,
           connectionSocketInfo->socket,
           getSpliceBytesTransferred(connectionSocketInfo->socket));
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
    destroyConnection(
      relatedConnectionSocketInfo,
      pollState);
  }
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
  struct PollState* pollState,
  const struct ProxySettings* proxySettings)
{
  struct ConnectionSocketInfo* relatedConnectionSocketInfo =
    connectionSocketInfo->relatedConnectionSocketInfo;

  if (connectionSocketInfo->waitingForConnect)
  {
    int socketError;

    assert(relatedConnectionSocketInfo != NULL);

    socketError = getSocketError(connectionSocketInfo->socket);
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
  struct ConnectionSocketInfo* pDisconnectSocketInfo = NULL;

  if (connectionSocketInfo->waitingForConnect)
  {
    proxyLog("connect timeout fd %d", connectionSocketInfo->socket);
    pDisconnectSocketInfo = connectionSocketInfo;
  }

  return pDisconnectSocketInfo;
}

static enum HandleConnectionReadyResult handleConnectionSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyFDInfo* readyFDInfo,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  struct ConnectionSocketInfo* connectionSocketInfo =
    (struct ConnectionSocketInfo*) abstractSocketInfo;
  enum HandleConnectionReadyResult handleConnectionReadyResult =
    POLL_STATE_NOT_INVALIDATED_RESULT;
  struct ConnectionSocketInfo* pDisconnectSocketInfo = NULL;

#ifdef DEBUG_PROXY
  proxyLog("fd %d readyForRead %d readyForWrite %d readyForTimeout %d",
           connectionSocketInfo->socket,
           readyFDInfo->readyForRead,
           readyFDInfo->readyForWrite,
           readyFDInfo->readyForTimeout);
#endif

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
        pollState,
        proxySettings);
  }

  if (readyFDInfo->readyForTimeout &&
      (!pDisconnectSocketInfo))
  {
    pDisconnectSocketInfo =
      handleConnectionReadyForTimeout(
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

static enum HandleConnectionReadyResult handleServerSocketReady(
  struct AbstractSocketInfo* abstractSocketInfo,
  const struct ReadyFDInfo* readyFDInfo,
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
  return POLL_STATE_NOT_INVALIDATED_RESULT;
}

static void runProxy(
  const struct ProxySettings* proxySettings)
{
  struct PollState* pollState;
  size_t i;

  proxyLog("num remote addresses = %ld", proxySettings->remoteAddrInfoArrayLength);
  for (i = 0; i < proxySettings->remoteAddrInfoArrayLength; ++i)
  {
    proxyLog("remote address %ld = %s:%s", i,
             proxySettings->remoteAddrInfoArray[i].addrPortStrings.addrString,
             proxySettings->remoteAddrInfoArray[i].addrPortStrings.portString);
  }
  proxyLog("connect timeout milliseconds = %d",
           proxySettings->connectTimeoutMS);

  pollState = newPollState();

  setupServerSockets(
    proxySettings,
    pollState);

  while (true)
  {
    bool pollStateInvalidated = false;
    const struct PollResult* pollResult = blockingPoll(pollState);

    for (i = 0; 
         (!pollStateInvalidated) &&
         (i < pollResult->numReadyFDs);
         ++i)
    {
      const struct ReadyFDInfo* readyFDInfo =
        &(pollResult->readyFDInfoArray[i]);
      struct AbstractSocketInfo* pAbstractSocketInfo = readyFDInfo->data;
      const enum HandleConnectionReadyResult handleConnectionReadyResult =
        (*(pAbstractSocketInfo->handleConnectionReadyFunction))(
          pAbstractSocketInfo, readyFDInfo, proxySettings, pollState);
      if (handleConnectionReadyResult == POLL_STATE_INVALIDATED_RESULT)
      {
        pollStateInvalidated = true;
      }
    }
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

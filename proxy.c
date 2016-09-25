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
      checkedCalloc(1, sizeof(struct ServerSocketInfo));
    serverSocketInfo->handleConnectionReadyFunction = handleServerSocketReady;

    if (addressToNameAndPort(listenAddrInfo->ai_addr,
                             listenAddrInfo->ai_addrlen,
                             &serverAddrPortStrings) < 0)
    {
      proxyLog("error resolving server listen address");
      goto fail;
    }

    serverSocketInfo->socket = socket(listenAddrInfo->ai_family,
                                      listenAddrInfo->ai_socktype | SOCK_NONBLOCK,
                                      listenAddrInfo->ai_protocol);
    if (serverSocketInfo->socket < 0)
    {
      proxyLog("error creating server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
    }

    if (setSocketReuseAddress(serverSocketInfo->socket) < 0)
    {
      proxyLog("setSocketReuseAddress error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
    }

    if (bind(serverSocketInfo->socket,
             listenAddrInfo->ai_addr,
             listenAddrInfo->ai_addrlen) < 0)
    {
      proxyLog("bind error on server socket %s:%s",
               serverAddrPortStrings.addrString,
               serverAddrPortStrings.portString);
      goto fail;
    }

    if (setSocketListening(serverSocketInfo->socket) < 0)
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

static void setupPledge()
{
  if (pledge("stdio inet", NULL) == -1)
  {
    proxyLog("pledge failed");
    abort();
  }
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

static bool getAddressesForClientSocket(
  int clientSocket,
  struct AddrPortStrings* clientAddrPortStrings,
  struct AddrPortStrings* proxyServerAddrPortStrings)
{
  struct sockaddr_storage clientAddress;
  socklen_t clientAddressSize;
  struct sockaddr_storage proxyServerAddress;
  socklen_t proxyServerAddressSize;

  clientAddressSize = sizeof(clientAddress);
  if (getpeername(
        clientSocket, 
        (struct sockaddr*)&clientAddress,
        &clientAddressSize) < 0)
  {
    proxyLog("getpeername error errno = %d", errno);
    goto fail;
  }

  if (addressToNameAndPort((struct sockaddr*)&clientAddress,
                           clientAddressSize,
                           clientAddrPortStrings) < 0)
  {
    proxyLog("error getting client address name and port");
    goto fail;
  }

  proxyServerAddressSize = sizeof(proxyServerAddress);
  if (getsockname(
        clientSocket, 
        (struct sockaddr*)&proxyServerAddress,
        &proxyServerAddressSize) < 0)
  {
    proxyLog("getsockname error errno = %d", errno);
    goto fail;
  }

  if (addressToNameAndPort((struct sockaddr*)&proxyServerAddress,
                           proxyServerAddressSize,
                           proxyServerAddrPortStrings) < 0)
  {
    proxyLog("error getting proxy server address name and port");
    goto fail;
  }

  proxyLog("connect client to proxy %s:%s -> %s:%s (fd=%d)",
           clientAddrPortStrings->addrString,
           clientAddrPortStrings->portString,
           proxyServerAddrPortStrings->addrString,
           proxyServerAddrPortStrings->portString,
           clientSocket);

  return true;

fail:
  return false;
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
              proxySettings->remoteAddrInfo->ai_socktype | SOCK_NONBLOCK,
              proxySettings->remoteAddrInfo->ai_protocol)
  };

  if (result.remoteSocket < 0)
  {
    proxyLog("error creating remote socket errno = %d", errno);
    goto fail;
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
    goto failWithSocket;
  }
  else
  {
    result.status = REMOTE_SOCKET_CONNECTED;
    if (setBidirectionalSplice(clientSocket, result.remoteSocket) < 0)
    {
      proxyLog("splice setup error");
      goto failWithSocket;
    }
  }

  proxyClientAddressSize = sizeof(proxyClientAddress);
  if (getsockname(
        result.remoteSocket,
        (struct sockaddr*)&proxyClientAddress,
        &proxyClientAddressSize) < 0)
  {
    proxyLog("remote getsockname error errno = %d", errno);
    goto failWithSocket;
  }

  if (addressToNameAndPort((struct sockaddr*)&proxyClientAddress,
                           proxyClientAddressSize,
                           proxyClientAddrPortStrings) < 0)
  {
    proxyLog("error getting proxy client address name and port");
    goto failWithSocket;
  }

  proxyLog("connect %s proxy to remote %s:%s -> %s:%s (fd=%d)",
           ((result.status == REMOTE_SOCKET_CONNECTED) ? "complete" : "starting"),
           proxyClientAddrPortStrings->addrString,
           proxyClientAddrPortStrings->portString,
           proxySettings->remoteAddrPortStrings.addrString,
           proxySettings->remoteAddrPortStrings.portString,
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
  int clientSocket,
  const struct ProxySettings* proxySettings,
  struct PollState* pollState)
{
  struct AddrPortStrings clientAddrPortStrings;
  struct AddrPortStrings proxyServerAddrPortStrings;
  struct AddrPortStrings proxyClientAddrPortStrings;
  struct RemoteSocketResult remoteSocketResult;
  struct ConnectionSocketInfo* connInfo1;
  struct ConnectionSocketInfo* connInfo2;

  if (!getAddressesForClientSocket(
        clientSocket,
        &clientAddrPortStrings,
        &proxyServerAddrPortStrings))
  {
    goto fail;
  }

  remoteSocketResult =
    createRemoteSocket(clientSocket,
                       proxySettings,
                       &proxyClientAddrPortStrings);
  if (remoteSocketResult.status == REMOTE_SOCKET_ERROR)
  {
    goto fail;
  }

  connInfo1 = checkedCalloc(1, sizeof(struct ConnectionSocketInfo));
  connInfo1->handleConnectionReadyFunction = handleConnectionSocketReady;
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
  connInfo2->handleConnectionReadyFunction = handleConnectionSocketReady;
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

  addConnectionSocketInfoToPollState(pollState, connInfo1, proxySettings);
  addConnectionSocketInfoToPollState(pollState, connInfo2, proxySettings);

  return;

fail:
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

  if (relatedConnectionSocketInfo)
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
      char* socketErrorString = errnoToString(socketError);
      proxyLog("async remote connect fd %d errno %d: %s",
               connectionSocketInfo->socket,
               socketError,
               socketErrorString);
      free(socketErrorString);
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

      if (setBidirectionalSplice(
            connectionSocketInfo->socket,
            relatedConnectionSocketInfo->socket) < 0)
      {
        proxyLog("splice setup error");
        goto fail;
      }

      removeConnectionSocketInfoFromPollState(pollState, connectionSocketInfo);
      removeConnectionSocketInfoFromPollState(pollState, relatedConnectionSocketInfo);

      connectionSocketInfo->waitingForConnect = false;
      connectionSocketInfo->waitingForRead = true;
      addConnectionSocketInfoToPollState(pollState, connectionSocketInfo, proxySettings);

      relatedConnectionSocketInfo->waitingForConnect = false;
      relatedConnectionSocketInfo->waitingForRead = true;
      addConnectionSocketInfoToPollState(pollState, relatedConnectionSocketInfo, proxySettings);
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
  struct ConnectionSocketInfo* connectionSocketInfo = (struct ConnectionSocketInfo*) abstractSocketInfo;
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
  struct ServerSocketInfo* serverSocketInfo = (struct ServerSocketInfo*) abstractSocketInfo;
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
  return POLL_STATE_NOT_INVALIDATED_RESULT;
}

static void runProxy(
  const struct ProxySettings* proxySettings)
{
  struct PollState pollState;

  proxyLog("remote address = %s:%s",
           proxySettings->remoteAddrPortStrings.addrString,
           proxySettings->remoteAddrPortStrings.portString);
  proxyLog("connect timeout milliseconds = %ld", proxySettings->connectTimeoutMS);

  setupSignals();

  initializePollState(&pollState);

  setupServerSockets(
    proxySettings,
    &pollState);

  setupPledge();

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
      const struct ReadyFDInfo* readyFDInfo =
        &(pollResult->readyFDInfoArray[i]);
      struct AbstractSocketInfo* pAbstractSocketInfo = readyFDInfo->data;
      const enum HandleConnectionReadyResult handleConnectionReadyResult =
        (*(pAbstractSocketInfo->handleConnectionReadyFunction))(
          pAbstractSocketInfo, readyFDInfo, proxySettings, &pollState);
      if (handleConnectionReadyResult == POLL_STATE_INVALIDATED_RESULT)
      {
        pollStateInvalidated = true;
      }
    }
  }
}

int main( 
  int argc,
  char** argv)
{
  const struct ProxySettings* proxySettings;

  proxySettings = processArgs(argc, argv);
  runProxy(proxySettings);

  return 0;
}

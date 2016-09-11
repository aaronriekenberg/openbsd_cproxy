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

#include "log.h"
#include "errutil.h"
#include "kqueue_pollutil.h"
#include "memutil.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

struct InternalPollState
{
  int kqueueFD;
  size_t numReadFDs;
  size_t numWriteFDs;
  size_t numTimeoutFDs;
  struct kevent* keventArray;
  size_t keventArrayCapacity;
};

void initializePollState(
  struct PollState* pollState)
{
  struct InternalPollState* internalPollState;

  assert(pollState != NULL);

  memset(pollState, 0, sizeof(struct PollState));
  internalPollState =
    checkedCalloc(1, sizeof(struct InternalPollState));
  pollState->internalPollState = internalPollState;
  internalPollState->kqueueFD = kqueue();
  if (internalPollState->kqueueFD < 0)
  {
    proxyLog("kqueue error errno %d: %s",
             errno,
             errnoToString(errno));
    abort();
  }
  proxyLog("created kqueue (fd=%d)",
           internalPollState->kqueueFD);
}

static int signalSafeKevent(
  int kq, const struct kevent *changelist, int nchanges,
  struct kevent *eventlist, int nevents,
  const struct timespec *timeout)
{
  bool interrupted;
  int retVal;
  do
  {
    retVal = kevent(
      kq, changelist, nchanges,
      eventlist, nevents,
      timeout);
    interrupted = ((retVal < 0) &&
                   (errno == EINTR));
  } while (interrupted);
  return retVal;
}

static void resizeKeventArray(
  struct InternalPollState* internalPollState)
{
  bool changedCapacity = false;
  while ((internalPollState->numReadFDs +
          internalPollState->numWriteFDs +
          internalPollState->numTimeoutFDs) >
         internalPollState->keventArrayCapacity)
  {
    changedCapacity = true;
    if (internalPollState->keventArrayCapacity == 0)
    {
      internalPollState->keventArrayCapacity = 16;
    }
    else
    {
      internalPollState->keventArrayCapacity *= 2;
    }
  }
  if (changedCapacity)
  {
    internalPollState->keventArray =
      checkedReallocarray(internalPollState->keventArray,
                          internalPollState->keventArrayCapacity,
                          sizeof(struct kevent));
  }
}

void addPollFDForRead(
  struct PollState* pollState,
  int fd,
  void* data)
{
  struct InternalPollState* internalPollState;
  struct kevent event;
  int retVal;

  assert(pollState != NULL);

  internalPollState = pollState->internalPollState;

  EV_SET(&event, fd, EVFILT_READ, EV_ADD, 0, 0, data);

  retVal = signalSafeKevent(internalPollState->kqueueFD, &event, 1, NULL, 0, NULL);
  if (retVal < 0)
  {
    proxyLog("kevent add read event error fd %d errno %d: %s",
             fd,
             errno,
             errnoToString(errno));
    abort();
  }
  else
  {
    ++(internalPollState->numReadFDs);
    resizeKeventArray(internalPollState);
  }
}

void removePollFDForRead(
  struct PollState* pollState,
  int fd)
{
  struct InternalPollState* internalPollState;
  struct kevent event;
  int retVal;

  assert(pollState != NULL);

  internalPollState = pollState->internalPollState;

  EV_SET(&event, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  retVal = signalSafeKevent(internalPollState->kqueueFD, &event, 1, NULL, 0, NULL);
  if (retVal < 0)
  {
    proxyLog("kevent remove read event error fd %d errno %d: %s",
             fd,
             errno,
             errnoToString(errno));
    abort();
  }
  else
  {
    --(internalPollState->numReadFDs);
  }
}

void addPollFDForWriteAndTimeout(
  struct PollState* pollState,
  int fd,
  void* data,
  uint64_t timeoutMillseconds)
{
  struct InternalPollState* internalPollState;
  struct kevent events[2];
  int retVal;

  assert(pollState != NULL);

  internalPollState = pollState->internalPollState;

  EV_SET(&(events[0]), fd, EVFILT_WRITE, EV_ADD, 0, 0, data);
  EV_SET(&(events[1]), fd, EVFILT_TIMER, EV_ADD, 0, timeoutMillseconds, data);

  retVal = signalSafeKevent(internalPollState->kqueueFD, events, 2, NULL, 0, NULL);
  if (retVal < 0)
  {
    proxyLog("kevent add write and timeout events error fd %d errno %d: %s",
             fd,
             errno,
             errnoToString(errno));
    abort();
  }
  else
  {
    ++(internalPollState->numWriteFDs);
    ++(internalPollState->numTimeoutFDs);
    resizeKeventArray(internalPollState);
  }
}

void removePollFDForWriteAndTimeout(
  struct PollState* pollState,
  int fd)
{
  struct InternalPollState* internalPollState;
  struct kevent events[2];
  int retVal;

  assert(pollState != NULL);

  internalPollState = pollState->internalPollState;

  EV_SET(&(events[0]), fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  EV_SET(&(events[1]), fd, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);

  retVal = signalSafeKevent(internalPollState->kqueueFD, events, 2, NULL, 0, NULL);
  if (retVal < 0)
  {
    proxyLog("kevent remove write and timeout events error fd %d errno %d: %s",
             fd,
             errno,
             errnoToString(errno));
    abort();
  }
  else
  {
    --(internalPollState->numWriteFDs);
    --(internalPollState->numTimeoutFDs);
  }
}

const struct PollResult* blockingPoll(
  struct PollState* pollState)
{
  struct InternalPollState* internalPollState;

  assert(pollState != NULL);

  internalPollState = pollState->internalPollState;
  if ((internalPollState->numReadFDs +
       internalPollState->numWriteFDs +
       internalPollState->numTimeoutFDs) > 0)
  {
    size_t i;
    const int retVal = signalSafeKevent(
                   internalPollState->kqueueFD,
                   NULL, 0,
                   internalPollState->keventArray,
                   internalPollState->keventArrayCapacity,
                   NULL);
    if (retVal < 0)
    {
      proxyLog("kevent wait error errno %d: %s",
               errno,
               errnoToString(errno));
      abort();
    }
    setPollResultNumReadyFDs(
      &(pollState->pollResult),
      retVal);
    for (i = 0; i < retVal; ++i)
    {
      struct ReadyFDInfo* readyFDInfo =
        &(pollState->pollResult.readyFDInfoArray[i]);
      const struct kevent* readyKEvent =
        &(internalPollState->keventArray[i]);
      readyFDInfo->data = readyKEvent->udata;
      readyFDInfo->readyForRead = (readyKEvent->filter == EVFILT_READ);
      readyFDInfo->readyForWrite = (readyKEvent->filter == EVFILT_WRITE);
      readyFDInfo->readyForTimeout = (readyKEvent->filter == EVFILT_TIMER);
    }
    return (&(pollState->pollResult));
  }
  return NULL;
}

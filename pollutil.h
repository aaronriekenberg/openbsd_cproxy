#ifndef POLLUTIL_H
#define POLLUTIL_H

#include "pollresult.h"
#include <sys/types.h>

struct PollState
{
  void* internalPollState;
};

extern void initializePollState(
  struct PollState* pollState);

extern void addPollFDForRead(
  struct PollState* pollState,
  int fd,
  void* data);

extern void removePollFDForRead(
  struct PollState* pollState,
  int fd);

extern void addPollFDForWriteAndTimeout(
  struct PollState* pollState,
  int fd,
  void* data,
  uint32_t timeoutMillseconds);

extern void removePollFDForWriteAndTimeout(
  struct PollState* pollState,
  int fd);

extern const struct PollResult* blockingPoll(
  struct PollState* pollState);

#endif
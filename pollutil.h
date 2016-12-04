#ifndef POLLUTIL_H
#define POLLUTIL_H

#include "pollresult.h"
#include <sys/types.h>

struct PollState;

struct PollState* newPollState();

void addPollFDForRead(
  struct PollState* pollState,
  int fd,
  void* data);

void removePollFDForRead(
  struct PollState* pollState,
  int fd);

void addPollFDForWriteAndTimeout(
  struct PollState* pollState,
  int fd,
  void* data,
  uint32_t timeoutMillseconds);

void removePollFDForWriteAndTimeout(
  struct PollState* pollState,
  int fd);

const struct PollResult* blockingPoll(
  struct PollState* pollState);

#endif

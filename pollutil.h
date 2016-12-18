#ifndef POLLUTIL_H
#define POLLUTIL_H

#include "pollresult.h"
#include <stdint.h>

struct PollState;

struct PollState* newPollState();

void addPollFDForRead(
  struct PollState* pollState,
  uintptr_t fd,
  void* data);

void removePollFDForRead(
  struct PollState* pollState,
  uintptr_t fd);

void addPollFDForWriteAndTimeout(
  struct PollState* pollState,
  uintptr_t fd,
  void* data,
  uint32_t timeoutMillseconds);

void removePollFDForWriteAndTimeout(
  struct PollState* pollState,
  uintptr_t fd);

void addPollIDForPeriodicTimer(
  struct PollState* pollState,
  uintptr_t id,
  void* data,
  uint32_t periodMilliseconds);

const struct PollResult* blockingPoll(
  struct PollState* pollState);

#endif

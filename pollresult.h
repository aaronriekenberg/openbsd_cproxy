#ifndef POLLRESULT_H
#define POLLRESULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ReadyEventInfo
{
  uintptr_t id;
  void* data;
  bool readyForRead;
  bool readyForWrite;
  bool readyForTimeout;
};

struct PollResult
{
  size_t numReadyEvents;
  struct ReadyEventInfo* readyEventInfoArray;
  size_t arrayCapacity;
};

struct PollResult* newPollResult();

void setPollResultNumReadyEvents(
  struct PollResult* pollResult,
  size_t numReadyEvents);

#endif

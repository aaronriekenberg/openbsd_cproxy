#include <assert.h>
#include "memutil.h"
#include "pollresult.h"

struct PollResult* newPollResult()
{
  return checkedCallocOne(sizeof(struct PollResult));
}

void setPollResultNumReadyEvents(
  struct PollResult* pollResult,
  size_t numReadyEvents)
{
  assert(pollResult != NULL);

  pollResult->readyEventInfoArray =
    resizeDynamicArray(
      pollResult->readyEventInfoArray,
      numReadyEvents,
      sizeof(struct ReadyEventInfo),
      &(pollResult->arrayCapacity));

  pollResult->numReadyEvents = numReadyEvents;
}

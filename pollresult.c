#include <assert.h>
#include "memutil.h"
#include "pollresult.h"

void setPollResultNumReadyFDs(
  struct PollResult* pollResult,
  size_t numReadyFDs)
{
  assert(pollResult != NULL);

  pollResult->readyFDInfoArray =
    resizeDynamicArray(
      pollResult->readyFDInfoArray,
      numReadyFDs,
      sizeof(struct ReadyFDInfo),
      &(pollResult->arrayCapacity));

  pollResult->numReadyFDs = numReadyFDs;
}

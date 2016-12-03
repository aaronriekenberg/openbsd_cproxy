#include "memutil.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

void* checkedCallocOne(
  const size_t size)
{
  void* retVal = calloc(1, size);
  if (retVal == NULL)
  {
    printf("calloc failed nmemb %u size %lu\n",
           1, size);
    abort();
  }
  return retVal;
}

void* checkedReallocarray(
  void *ptr,
  const size_t nmemb,
  const size_t size)
{
  void* retVal = reallocarray(ptr, nmemb, size);
  if (retVal == NULL)
  {
    printf("reallocarray failed ptr %p nmemb %lu size %lu\n",
           ptr, nmemb, size);
    abort();
  }
  return retVal;
}

extern void* resizeDynamicArray(
  void* array,
  const size_t newLength,
  const size_t memberSize,
  size_t* capacity)
{
  bool changedCapacity = false;

  assert(capacity != NULL);

  while (newLength > (*capacity))
  {
    changedCapacity = true;
    if ((*capacity) == 0)
    {
      (*capacity) = 2;
    }
    else
    {
      (*capacity) *= 2;
    }
  }

  if (changedCapacity)
  {
    array = checkedReallocarray(
      array,
      *capacity,
      memberSize);
  }

  return array;
}

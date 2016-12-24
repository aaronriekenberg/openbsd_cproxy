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
    printf("calloc failed nmemb %u size %zu\n",
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
    printf("reallocarray failed ptr %p nmemb %zu size %zu\n",
           ptr, nmemb, size);
    abort();
  }
  return retVal;
}

#define MAX_DYNAMIC_ARRAY_LENGTH (((size_t)1) << ((sizeof(size_t) * 8) - 1))

void* resizeDynamicArray(
  void* array,
  const size_t newLength,
  const size_t memberSize,
  size_t* capacity)
{
  assert(capacity != NULL);

  if (newLength <= (*capacity))
  {
    return array;
  }

  if (newLength > MAX_DYNAMIC_ARRAY_LENGTH)
  {
    printf("newLength %zu > MAX_DYNAMIC_ARRAY_LENGTH %zu\n",
           newLength, MAX_DYNAMIC_ARRAY_LENGTH);
    abort();
  }

  do
  {
    if ((*capacity) == 0)
    {
      (*capacity) = 2;
    }
    else
    {
      (*capacity) *= 2;
    }
  } while (newLength > (*capacity));

  return checkedReallocarray(
    array,
    *capacity,
    memberSize);
}

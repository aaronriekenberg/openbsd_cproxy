#include "memutil.h"
#include <stdio.h>

void* checkedCallocOne(
  const size_t size)
{
  void* retVal = calloc(1, size);
  if (retVal == NULL)
  {
    printf("calloc failed nmemb %d size %ld\n",
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
    printf("reallocarray failed ptr %p nmemb %ld size %ld\n",
           ptr, nmemb, size);
    abort();
  }
  return retVal;
}

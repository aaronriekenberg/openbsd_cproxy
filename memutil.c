#include "memutil.h"
#include <stdio.h>

void* checkedCallocOne(
  const size_t size)
{
  void* retVal = calloc(1, size);
  if (retVal == NULL)
  {
    printf("calloc failed nmemb %d size %lld\n",
           1, (long long)size);
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
    printf("reallocarray failed ptr %p nmemb %lld size %lld\n",
           ptr, (long long)nmemb, (long long)size);
    abort();
  }
  return retVal;
}

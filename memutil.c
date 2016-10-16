#include "memutil.h"
#include <stdio.h>

void* checkedCalloc(
  const size_t nmemb,
  const size_t size)
{
  void* retVal = calloc(nmemb, size);
  if (retVal == NULL)
  {
    printf("calloc failed nmemb %lld size %lld\n",
           (long long)nmemb, (long long)size);
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

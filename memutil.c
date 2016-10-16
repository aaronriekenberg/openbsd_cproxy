#include "memutil.h"
#include <stdio.h>

void* checkedCalloc(
  size_t nmemb,
  size_t size)
{
  void* retVal = calloc(nmemb, size);
  if ((!retVal) && nmemb && size)
  {
    printf("calloc failed nmemb %ld size %ld\n",
           (long)nmemb, (long)size);
    abort();
  }
  return retVal;
}

void* checkedReallocarray(
  void *ptr,
  size_t nmemb,
  size_t size)
{
  void* retVal = reallocarray(ptr, nmemb, size);
  if ((!retVal) && nmemb && size)
  {
    printf("reallocarray failed ptr %p nmemb %ld size %ld\n",
           ptr, (long)nmemb, (long)size);
    abort();
  }
  return retVal;
}

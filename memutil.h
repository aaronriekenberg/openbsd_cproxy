#ifndef MEMUTIL_H
#define MEMUTIL_H

#include <stdlib.h>

extern void* checkedMalloc(
  size_t size);

extern void* checkedCalloc(
  size_t nmemb,
  size_t size);

extern void* checkedRealloc(
  void* ptr,
  size_t size);

extern void* checkedReallocarray(
  void *ptr,
  size_t nmemb,
  size_t size);

#endif

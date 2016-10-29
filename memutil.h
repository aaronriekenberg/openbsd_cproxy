#ifndef MEMUTIL_H
#define MEMUTIL_H

#include <stdlib.h>

extern void* checkedCallocOne(
  const size_t size);

extern void* checkedReallocarray(
  void *ptr,
  const size_t nmemb,
  const size_t size);

#endif

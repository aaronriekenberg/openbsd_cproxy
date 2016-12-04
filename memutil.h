#ifndef MEMUTIL_H
#define MEMUTIL_H

#include <stdlib.h>

void* checkedCallocOne(
  const size_t size);

void* checkedReallocarray(
  void* ptr,
  const size_t nmemb,
  const size_t size);

void* resizeDynamicArray(
  void* array,
  const size_t newLength,
  const size_t memberSize,
  size_t* capacity);

#endif

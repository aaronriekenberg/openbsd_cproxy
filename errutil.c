#include "errutil.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* errnoToString(const int errnoToTranslate)
{
  const int previousErrno = errno;
  const char* errorString;

  errno = 0;
  errorString = strerror(errnoToTranslate);
  if (errno != 0)
  {
    printf("strerror error errnoToTranslate = %d errno = %d\n",
           errnoToTranslate, errno);
    abort();
  }

  errno = previousErrno;
  return errorString;
}

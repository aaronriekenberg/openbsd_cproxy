#include "timeutil.h"
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

void printTimeString(FILE* fp)
{
  size_t charsWritten;
  char buffer[80];
  struct timeval tv;
  struct tm* tm;

  if (gettimeofday(&tv, NULL) == -1)
  {
    printf("gettimeofday error\n");
    abort();
  }

  tm = localtime(&tv.tv_sec);
  if (tm == NULL)
  {
    printf("localtime error\n");
    abort();
  }

  charsWritten = strftime(buffer, sizeof(buffer), "%Y-%b-%d %H:%M:%S", tm);
  if (charsWritten == 0)
  {
    printf("strftime error\n");
    abort();
  }
  else if (charsWritten > (sizeof(buffer) - 8))
  {
    printf("strftime overflow\n");
    abort();
  }

  snprintf(buffer + charsWritten, 8, ".%06ld", (long)tv.tv_usec);

  fputs(buffer, fp);
}

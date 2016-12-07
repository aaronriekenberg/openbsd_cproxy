#include "timeutil.h"
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

void printTimeString()
{
  size_t charsWritten;
  char buffer[80];
  struct timeval tv;
  struct tm tm;

  if (gettimeofday(&tv, NULL) < 0)
  {
    printf("gettimeofday error\n");
    abort();
  }

  if (!localtime_r(&tv.tv_sec, &tm))
  {
    printf("localtime_r error\n");
    abort();
  }

  charsWritten = strftime(buffer, 80, "%Y-%b-%d %H:%M:%S", &tm);
  if (charsWritten == 0)
  {
    printf("strftime error\n");
    abort();
  }
  else if (charsWritten > (80 - 7 - 1))
  {
    printf("strftime overflow\n");
    abort();
  }

  snprintf(buffer + charsWritten, 8, ".%06ld", tv.tv_usec);
  fputs(buffer, stdout);
}

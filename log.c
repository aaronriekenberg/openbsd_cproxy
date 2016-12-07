#include "log.h"
#include "timeutil.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool flushAfterLog = false;

void proxyLogSetFlush(bool enabled)
{
  flushAfterLog = enabled;
}

void proxyLog(const char* format, ...)
{
  va_list args;

  va_start(args, format);

  printTimeString();
  fputc(' ', stdout);
  vprintf(format, args);
  fputc('\n', stdout);

  if (flushAfterLog)
  {
    fflush(stdout);
  }

  va_end(args);
}

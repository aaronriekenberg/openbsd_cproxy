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

static void internalProxyLog(bool time, const char* format, va_list args)
{
  if (time)
  {
    printTimeString(stdout);
    fputc(' ', stdout);
  }

  vfprintf(stdout, format, args);
  fputc('\n', stdout);

  if (flushAfterLog)
  {
    fflush(stdout);
  }
}

void proxyLog(const char* format, ...)
{
  va_list args;

  va_start(args, format);

  internalProxyLog(true, format, args);

  va_end(args);
}

void proxyLogNoTime(const char* format, ...)
{
  va_list args;

  va_start(args, format);

  internalProxyLog(false, format, args);

  va_end(args);
}

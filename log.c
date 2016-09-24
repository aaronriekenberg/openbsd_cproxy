#include "log.h"
#include "memutil.h"
#include "timeutil.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void proxyLog(const char* format, ...)
{
  va_list args;

  va_start(args, format);

  printTimeString();
  printf(" ");
  vprintf(format, args);
  printf("\n");
  fflush(stdout);

  va_end(args);
}

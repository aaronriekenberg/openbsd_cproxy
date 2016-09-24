#include "fdutil.h"
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

int signalSafeClose(
  int fd)
{
  bool interrupted;
  int retVal;
  do
  {
    retVal = close(fd);
    interrupted =
      ((retVal == -1) &&
       (errno == EINTR));
  } while (interrupted);
  return retVal;
}

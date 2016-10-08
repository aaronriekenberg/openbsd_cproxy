#include "fdutil.h"
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

bool signalSafeClose(
  int fd)
{
  bool interrupted;
  int closeRetVal;
  do
  {
    closeRetVal = close(fd);
    interrupted =
      ((closeRetVal == -1) &&
       (errno == EINTR));
  } while (interrupted);
  return (closeRetVal != -1);
}

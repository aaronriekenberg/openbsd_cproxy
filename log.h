#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

extern void proxyLogSetFlush(bool enabled);

extern void proxyLog(const char* format, ...);

#endif

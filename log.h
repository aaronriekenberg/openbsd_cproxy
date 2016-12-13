#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

void proxyLogSetFlush(bool enabled);

void proxyLog(const char* format, ...);

void proxyLogNoTime(const char* format, ...);

#endif

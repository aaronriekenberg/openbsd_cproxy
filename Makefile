CC = cc
CFLAGS = -g -Wall
LDFLAGS =

SRC = errutil.c \
      fdutil.c \
      kqueue_pollutil.c \
      log.c \
      memutil.c \
      pollresult.c \
      proxy.c \
      proxysettings.c \
      socketutil.c \
      timeutil.c
OBJS = $(SRC:.c=.o)

all: cproxy

clean:
	rm -f *.o cproxy

cproxy: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

depend:
	$(CC) $(CFLAGS) -MM $(SRC) > .makeinclude

include .makeinclude

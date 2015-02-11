CC = gcc
AR = ar
DEBUG = 1
OSTYPE = $(shell uname -s)

CFLAGS = -Wall -I.

ifneq ($(DEBUG), 0)
CFLAGS += -g -DDEBUG
else
CFLAGS += -O2
endif

ifeq ($(OSTYPE), MorphOS)
CFLAGS += -noixemul -DAROS_ALMOST_COMPATIBLE
endif

ifeq ($(OSTYPE), AROS)
# I shouldn't need to do this manually
CFLAGS += -march=i586
endif

OBJ = \
	pthread.o \
	sched.o \
	semaphore.o

all: libpthread.a

libpthread.a: $(OBJ)
	$(AR) rcs $@ $?

clean:
	rm -f $(OBJ)

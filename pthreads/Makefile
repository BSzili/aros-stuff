CC = gcc
AR = ar
DEBUG = 1
OSTYPE = $(shell uname -s)

CFLAGS = -Wall

ifeq ($(DEBUG), 1)
CFLAGS += -g -DDEBUG
else
CFLAGS += -O2
endif

OBJ = \
	pthread.o \
	sched.o

all: libpthread.a

libpthread.a: $(OBJ)
	$(AR) rcs $@ $?

clean:
	rm -f $(OBJ)

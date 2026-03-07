CC = gcc
AR = ar
CFLAGS = -O2 -Wall -Wextra -I./include -I../../../../include
ARFLAGS = rcs

SRCDIR = src
INCDIR = include

SRCS = $(SRCDIR)/cxl_rpc.c \
       $(SRCDIR)/cxl_copyengine.c \
       $(SRCDIR)/cxl_alloc.c \
       $(SRCDIR)/cxl_sync.c
OBJS = $(SRCS:.c=.o)

TARGET = libcxlrpc.a

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(wildcard $(INCDIR)/*.h) $(wildcard $(SRCDIR)/*.h)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean

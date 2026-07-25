CC      ?= cc
CFLAGS   = -Wall -Wextra -std=c99 -g
LDFLAGS  =

SRCDIR   = src
VENDOR   = vendor
TESTDIR  = tests/unit
BINDIR   = bin
BUILDDIR = build

TARGET   = $(BINDIR)/kanban
TESTBIN  = $(BINDIR)/test_board

SRCS     = $(SRCDIR)/main.c $(SRCDIR)/board.c $(SRCDIR)/tui.c $(VENDOR)/cJSON.c
TESTSRCS = $(TESTDIR)/test_board.c $(SRCDIR)/board.c $(VENDOR)/cJSON.c

OBJS     = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
TESTOBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTSRCS))

.PHONY: all test clean

all: $(TARGET)

test: $(TESTBIN)
	./$(TESTBIN)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lncurses

$(TESTBIN): $(TESTOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BINDIR):
	mkdir -p $(BINDIR)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BINDIR) $(BUILDDIR)

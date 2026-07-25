CC      ?= cc
CFLAGS   = -Wall -Wextra -std=c99 -g
LDFLAGS  =

# SQLite is compiled separately without -Wall -Wextra (it produces warnings)
SQLITE_CFLAGS = -std=c99 -g -DSQLITE_THREADSAFE=0

SRCDIR   = src
VENDOR   = vendor
TESTDIR  = tests/unit
BINDIR   = bin
BUILDDIR = build

TARGET     = $(BINDIR)/kanban
TESTBIN    = $(BINDIR)/test_board
TESTDBBIN  = $(BINDIR)/test_db

SRCS       = $(SRCDIR)/main.c $(SRCDIR)/board.c $(SRCDIR)/db.c $(SRCDIR)/tui.c \
             $(VENDOR)/cJSON.c $(VENDOR)/sqlite3.c
TESTSRCS   = $(TESTDIR)/test_board.c $(SRCDIR)/board.c $(SRCDIR)/db.c \
             $(VENDOR)/cJSON.c $(VENDOR)/sqlite3.c
TESTDBSRCS = $(TESTDIR)/test_db.c $(SRCDIR)/db.c $(VENDOR)/sqlite3.c

OBJS       = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
TESTOBJS   = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTSRCS))
TESTDBOBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTDBSRCS))

.PHONY: all test clean

all: $(TARGET)

test: $(TESTBIN) $(TESTDBBIN)
	./$(TESTBIN)
	./$(TESTDBBIN)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lncurses

$(TESTBIN): $(TESTOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTDBBIN): $(TESTDBOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BINDIR):
	mkdir -p $(BINDIR)

# sqlite3.c compiled separately without -Wall -Wextra
$(BUILDDIR)/vendor/sqlite3.o: vendor/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) $(SQLITE_CFLAGS) -c -o $@ $<

# all other .c files compiled with normal CFLAGS
$(BUILDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BINDIR) $(BUILDDIR)

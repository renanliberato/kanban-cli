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
TESTBIN     = $(BINDIR)/test_board
TESTDBBIN  = $(BINDIR)/test_db
TESTLLMBIN = $(BINDIR)/test_llm
TESTENRICH  = $(BINDIR)/test_enrich
TESTUNDOBIN = $(BINDIR)/test_undo
TESTBPTHBIN = $(BINDIR)/test_board_path

SRCS       = $(SRCDIR)/main.c $(SRCDIR)/board.c $(SRCDIR)/db.c $(SRCDIR)/tui.c $(SRCDIR)/llm.c $(SRCDIR)/enrich.c $(SRCDIR)/undo.c $(SRCDIR)/board_path.c \
             $(VENDOR)/cJSON.c $(VENDOR)/sqlite3.c
TESTSRCS   = $(TESTDIR)/test_board.c $(SRCDIR)/board.c $(SRCDIR)/db.c $(SRCDIR)/enrich.c $(SRCDIR)/undo.c \
             $(VENDOR)/cJSON.c $(VENDOR)/sqlite3.c
TESTDBSRCS = $(TESTDIR)/test_db.c $(SRCDIR)/db.c $(VENDOR)/sqlite3.c
TESTLLMSRCS = $(TESTDIR)/test_llm.c $(SRCDIR)/llm.c
TESTENRICHSRCS = $(TESTDIR)/test_enrich.c $(SRCDIR)/enrich.c $(VENDOR)/cJSON.c
TESTUNDOSRCS = $(TESTDIR)/test_undo.c $(SRCDIR)/undo.c
TESTBPSRCS  = $(TESTDIR)/test_board_path.c $(SRCDIR)/board_path.c $(SRCDIR)/db.c $(VENDOR)/sqlite3.c

OBJS       = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
TESTOBJS   = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTSRCS))
TESTDBOBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTDBSRCS))
TESTLLMOBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTLLMSRCS))
TESTENRICHOBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTENRICHSRCS))
TESTUNDOOBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTUNDOSRCS))
TESTBPOBJS  = $(patsubst %.c,$(BUILDDIR)/%.o,$(TESTBPSRCS))

.PHONY: all test clean

all: $(TARGET)

test: $(TESTBIN) $(TESTDBBIN) $(TESTLLMBIN) $(TESTENRICH) $(TESTUNDOBIN) $(TESTBPTHBIN)
	./$(TESTBIN)
	./$(TESTDBBIN)
	./$(TESTLLMBIN)
	./$(TESTENRICH)
	./$(TESTUNDOBIN)
	./$(TESTBPTHBIN)

$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lncurses

$(TESTBIN): $(TESTOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTDBBIN): $(TESTDBOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTLLMBIN): $(TESTLLMOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTENRICH): $(TESTENRICHOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTUNDOBIN): $(TESTUNDOOBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TESTBPTHBIN): $(TESTBPOBJS) | $(BINDIR)
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

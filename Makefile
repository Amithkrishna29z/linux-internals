# Top-level Makefile — builds every example in the course.
#
#   make          build every example under examples/
#   make clean    remove every built binary
#   make list     show what would be built
#
# Each .c file under examples/ becomes a binary of the same name, next to it.
# As new modules are added, no edits are needed here — new .c files are found
# automatically.

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2
# Some later modules need pthreads / realtime libs; harmless for earlier ones.
LDLIBS  := -lpthread -lrt

# Every C source file under examples/ (recursively).
SRCS := $(shell find examples -name '*.c' 2>/dev/null)
# The binary for each source is the source path minus the .c suffix.
BINS := $(SRCS:.c=)

.PHONY: all clean list

all: $(BINS)

# Pattern rule: build examples/foo/bar from examples/foo/bar.c
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

list:
	@echo "Sources found:"
	@for s in $(SRCS); do echo "  $$s"; done
	@echo "Will build:"
	@for b in $(BINS); do echo "  $$b"; done

clean:
	@rm -f $(BINS)
	@echo "Removed built binaries."

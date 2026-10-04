# Makefile for nosleep C migration
# Uses MinGW gcc on Windows
# VERSION can be overridden on command line: make VERSION=2.2.0

VERSION ?= 0.0.0

CC = gcc
RC = windres
CFLAGS = -std=c99 -Wall -Wextra -O2 -Isrc -DVERSION_STR=\"$(VERSION)\"
LDFLAGS = -mwindows -luser32 -lkernel32 -lgdi32 -lpowrprof -ladvapi32 -lwinhttp -lcomctl32

Comma := ,
VERSION_COMMA := $(subst .,$(Comma),$(VERSION))
VERSION_COMMA := $(VERSION_COMMA),0

SRCDIR = src
OBJDIR = obj
BINDIR = bin

SOURCES = $(SRCDIR)/core.c $(SRCDIR)/tray.c $(SRCDIR)/main.c $(SRCDIR)/notify_groups.c $(SRCDIR)/updater.c $(SRCDIR)/updater_logic.c $(SRCDIR)/updater_pe.c $(SRCDIR)/cJSON.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
RESOURCE_OBJ = $(OBJDIR)/resources.o
TARGET = $(BINDIR)/nosleep.exe
VERSION_STAMP = $(OBJDIR)/.version

.PHONY: all clean test-unit test-cli FORCE

all: $(TARGET)

$(TARGET): $(OBJECTS) $(RESOURCE_OBJ) | $(BINDIR)
	$(CC) $(OBJECTS) $(RESOURCE_OBJ) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(VERSION_STAMP) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(RESOURCE_OBJ): $(SRCDIR)/resources.rc $(VERSION_STAMP) | $(OBJDIR)
	sed 's/@VERSION_COMMA@/$(VERSION_COMMA)/g; s/@VERSION_STRING@/$(VERSION)/g' $(SRCDIR)/resources.rc > $(OBJDIR)/resources_built.rc
	$(RC) --include-dir $(SRCDIR) -i $(OBJDIR)/resources_built.rc -o $@

$(VERSION_STAMP): FORCE | $(OBJDIR)
	@printf '%s\n' '$(VERSION)' > $@.tmp
	@if ! cmp -s $@.tmp $@; then mv $@.tmp $@; else rm $@.tmp; fi

FORCE:

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BINDIR):
	mkdir -p $(BINDIR)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

# Run the program with default arguments (tray mode)
run: $(TARGET)
	./$(TARGET)

# Run CLI mode with 30 minute duration
run-cli: $(TARGET)
	./$(TARGET) --duration 30

# Build and run unit tests for updater module (JSON parsing, version comparison)
test-unit: $(OBJDIR) test-cli
	$(CC) -std=c99 -Wall -Wextra tests/test_updater_response_buffer.c -o tests/test_updater_response_buffer_t.exe
	./tests/test_updater_response_buffer_t.exe
	$(CC) -std=c99 -Wall -Wextra tests/test_updater_temp_path.c -o tests/test_updater_temp_path_t.exe
	./tests/test_updater_temp_path_t.exe
	$(CC) -std=c99 -Wall -Wextra tests/test_updater_stream.c -o tests/test_updater_stream_t.exe
	./tests/test_updater_stream_t.exe
	$(CC) -std=c99 -Wall -Wextra -Isrc tests/test_updater_pe.c $(SRCDIR)/updater_pe.c -o tests/test_updater_pe_t.exe
	./tests/test_updater_pe_t.exe
	$(CC) -std=c99 -Wall -Wextra tests/test_updater_redirect_target.c -o tests/test_updater_redirect_target_t.exe
	./tests/test_updater_redirect_target_t.exe
	$(CC) -std=c99 -Wall -Wextra -Isrc tests/test_updater_batch_escape.c -o tests/test_updater_batch_escape_t.exe
	./tests/test_updater_batch_escape_t.exe
	$(CC) -std=c99 -Wall -Wextra -Isrc tests/test_updater_command_line.c -o tests/test_updater_command_line_t.exe
	./tests/test_updater_command_line_t.exe
	$(CC) -std=c99 -Wall -Wextra -Isrc tests/test_updater.c $(SRCDIR)/updater_logic.c $(SRCDIR)/cJSON.c -o tests/test_updater_t.exe
	./tests/test_updater_t.exe

test-cli:
	bash ./test_cli_run_mode_overrides.sh
	bash ./test_cli_batch_mode.sh

# Build main binary (and test it exists)
test: $(TARGET)
	$(TARGET) --help

# Install (copy to current directory)
install: $(TARGET)
	cp $(TARGET) ./nosleep.exe

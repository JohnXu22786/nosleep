# Makefile for nosleep C migration
# Uses MinGW gcc on Windows
# VERSION can be overridden on command line: make VERSION=2.2.0

VERSION ?= 0.0.0

CC = gcc
RC = windres
CFLAGS = -std=c99 -Wall -Wextra -O2 -Isrc -DVERSION_STR=\"$(VERSION)\"
LDFLAGS = -mwindows -luser32 -lkernel32 -lgdi32 -lpowrprof -ladvapi32 -lktmw32 -lwinhttp -lcomctl32

Comma := ,
# windres treats leading-zero resource numbers as octal; keep the display version
# intact while converting the numeric resource components to decimal.
VERSION_EMPTY :=
VERSION_SPACE := $(VERSION_EMPTY) $(VERSION_EMPTY)
version_strip_leading_zeros = $(if $(filter 0%,$(1)),$(call version_strip_leading_zeros,$(patsubst 0%,%,$(1))),$(if $(1),$(1),0))
VERSION_RESOURCE_INPUT := $(firstword $(subst -, ,$(VERSION)))
VERSION_DECIMAL := $(subst $(VERSION_SPACE),.,$(foreach component,$(subst .,$(VERSION_SPACE),$(VERSION_RESOURCE_INPUT)),$(call version_strip_leading_zeros,$(component))))
VERSION_COMMA := $(subst .,$(Comma),$(VERSION_DECIMAL))
VERSION_COMMA := $(VERSION_COMMA),0

SRCDIR = src
OBJDIR = obj
BINDIR = bin

SOURCES = $(SRCDIR)/core.c $(SRCDIR)/tray.c $(SRCDIR)/main.c $(SRCDIR)/notify_groups.c $(SRCDIR)/updater.c $(SRCDIR)/updater_logic.c $(SRCDIR)/updater_response_read.c $(SRCDIR)/updater_pe.c $(SRCDIR)/cJSON.c
HEADERS = $(wildcard $(SRCDIR)/*.h)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
RESOURCE_OBJ = $(OBJDIR)/resources.o
TARGET = $(BINDIR)/nosleep.exe
VERSION_STAMP = $(OBJDIR)/.version

.PHONY: all clean test-unit test-cli FORCE

all: $(TARGET)

$(TARGET): $(OBJECTS) $(RESOURCE_OBJ) | $(BINDIR)
	$(CC) $(OBJECTS) $(RESOURCE_OBJ) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HEADERS) $(VERSION_STAMP) | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(RESOURCE_OBJ): $(SRCDIR)/resources.rc no-sleeping_9260684.ico $(HEADERS) $(VERSION_STAMP) | $(OBJDIR)
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
test-unit: $(OBJDIR) test-cli test-updater-url-policy
	bash ./test_updater_launch_cleanup.sh
	bash ./test_updater_wait_process.sh
	bash ./test_updater_startup_handshake.sh
	bash ./test_updater_replace_failure.sh
	bash ./test_updater_initial_url_query.sh
	bash ./test_updater_url_port.sh
	$(CC) -std=c99 -Wall -Wextra tests/test_updater_response_buffer.c -o tests/test_updater_response_buffer_t.exe
	./tests/test_updater_response_buffer_t.exe
	$(CC) -std=c99 -Wall -Wextra -Isrc tests/test_updater_response_read.c $(SRCDIR)/updater_response_read.c $(SRCDIR)/updater_logic.c $(SRCDIR)/cJSON.c -o tests/test_updater_response_read_t.exe
	./tests/test_updater_response_read_t.exe
	$(CC) -std=c99 -Wall -Wextra -Isrc -Drealloc=test_realloc tests/test_updater_response_content_length.c $(SRCDIR)/updater_response_read.c -o tests/test_updater_response_content_length_t.exe
	./tests/test_updater_response_content_length_t.exe
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

test-updater-url-policy:
	$(CC) -std=c99 -Wall -Wextra -Isrc tests/test_updater_url_policy.c -o tests/test_updater_url_policy_t.exe
	./tests/test_updater_url_policy_t.exe

test-cli:
	bash ./test_cli_run_mode_overrides.sh
	bash ./test_cli_batch_mode.sh

# Build main binary (and test it exists)
test: $(TARGET)
	$(TARGET) --help

# Install (copy to current directory)
install: $(TARGET)
	cp $(TARGET) ./nosleep.exe

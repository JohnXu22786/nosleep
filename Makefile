# Makefile for nosleep C migration
# Uses MinGW gcc on Windows
# VERSION can be overridden on command line: make VERSION=2.2.0

VERSION ?= 0.0.0

CC = gcc
RC = windres
CFLAGS = -std=c99 -Wall -Wextra -O2 -Isrc -DVERSION_STR=\"$(VERSION)\"
LDFLAGS = -mwindows -luser32 -lkernel32 -lgdi32 -lpowrprof -ladvapi32 -lwinhttp

Comma := ,
VERSION_COMMA := $(subst .,$(Comma),$(VERSION))
VERSION_COMMA := $(VERSION_COMMA),0

SRCDIR = src
OBJDIR = obj
BINDIR = bin

SOURCES = $(SRCDIR)/core.c $(SRCDIR)/tray.c $(SRCDIR)/main.c $(SRCDIR)/notify_groups.c $(SRCDIR)/updater.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
RESOURCE_OBJ = $(OBJDIR)/resources.o
TARGET = $(BINDIR)/nosleep.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS) $(RESOURCE_OBJ) | $(BINDIR)
	$(CC) $(OBJECTS) $(RESOURCE_OBJ) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(RESOURCE_OBJ): $(SRCDIR)/resources.rc | $(OBJDIR)
	sed 's/@VERSION_COMMA@/$(VERSION_COMMA)/g; s/@VERSION_STRING@/$(VERSION)/g' $(SRCDIR)/resources.rc > $(OBJDIR)/resources_built.rc
	$(RC) --include-dir $(SRCDIR) -i $(OBJDIR)/resources_built.rc -o $@

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

# Build and test
test: $(TARGET)
	./$(TARGET) --help

# Install (copy to current directory)
install: $(TARGET)
	cp $(TARGET) ./nosleep.exe

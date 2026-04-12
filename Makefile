# Makefile for nosleep C migration
# Uses MinGW gcc on Windows

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2 -Isrc
LDFLAGS = -mwindows -luser32 -lkernel32 -lgdi32

SRCDIR = src
OBJDIR = obj
BINDIR = bin

SOURCES = $(SRCDIR)/core.c $(SRCDIR)/tray.c $(SRCDIR)/main.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
TARGET = $(BINDIR)/nosleep.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS) | $(BINDIR)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

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

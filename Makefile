# ZrnPerformanceMgmnt - Makefile
#
#   make            build (TUI + daemon, no GUI)
#   make gui        build with GTK3 GUI support
#   make clean      remove build artefacts
#   make install    copy binary to ~/bin/zrn_perfd
#   make run        build and launch (TUI mode)
#   make run-gui    build and launch in GUI mode

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 \
           $(shell pkg-config --cflags x11) \
           $(shell pkg-config --cflags ncursesw) \
           $(shell pkg-config --cflags libpulse)
LDFLAGS := $(shell pkg-config --libs x11) \
           $(shell pkg-config --libs ncursesw) \
           $(shell pkg-config --libs libpulse) \
           -lm

# GTK3 flags (only used for `make gui`)
GTK_CFLAGS  := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LDFLAGS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null)

SRCDIR  := src
SRCS    := $(SRCDIR)/main.c \
           $(SRCDIR)/profile.c \
           $(SRCDIR)/proctrack.c \
           $(SRCDIR)/xwatch.c \
           $(SRCDIR)/throttle.c \
           $(SRCDIR)/switchlog.c \
           $(SRCDIR)/gpu_monitor.c \
           $(SRCDIR)/neural_predict.c \
           $(SRCDIR)/exempt.c \
           $(SRCDIR)/audio_monitor.c \
           $(SRCDIR)/tui.c \
           $(SRCDIR)/gui.c
OBJS    := $(SRCS:.c=.o)
TARGET  := zrn_perfd

.PHONY: all gui clean install run run-gui daemon

all: $(TARGET)

# Standard build (no GUI)
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/zrn_perf.h
	$(CC) $(CFLAGS) -c -o $@ $<

# GUI build: recompile main.c and gui.c with ENABLE_GUI + GTK flags
gui: CFLAGS += -DENABLE_GUI $(GTK_CFLAGS)
gui: LDFLAGS += $(GTK_LDFLAGS)
gui: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/bin/$(TARGET)
	@echo "Installed to $(HOME)/bin/$(TARGET)"

run: $(TARGET)
	./$(TARGET)

run-gui: gui
	./$(TARGET) --gui

daemon: $(TARGET)
	./$(TARGET) --daemon

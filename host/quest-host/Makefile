CC ?= cc
PKG_CONFIG ?= pkg-config

TARGET := quest-game-host
PROBE_TARGET := pic32-open-probe
SOURCES := src/main.c src/serial_port.c
PROBE_SOURCES := src/open_probe.c src/serial_port.c

CFLAGS ?= -O2 -g
CPPFLAGS += -D_DARWIN_C_SOURCE
WARNINGS := -Wall -Wextra -Wpedantic -std=c11
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 2>/dev/null)

.PHONY: all clean check-gtk

all: check-gtk $(TARGET) $(PROBE_TARGET)

check-gtk:
	@command -v $(PKG_CONFIG) >/dev/null 2>&1 || { \
		echo "pkg-config was not found."; \
		echo "macOS: brew install gtk+3 pkg-config"; \
		echo "Debian/Ubuntu: sudo apt install libgtk-3-dev pkg-config"; \
		exit 1; \
	}
	@$(PKG_CONFIG) --exists gtk+-3.0 || { \
		echo "GTK+ 3 development files were not found."; \
		echo "macOS: brew install gtk+3 pkg-config"; \
		echo "Debian/Ubuntu: sudo apt install libgtk-3-dev pkg-config"; \
		exit 1; \
	}

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(GTK_CFLAGS) -o $@ $(SOURCES) $(GTK_LIBS)

$(PROBE_TARGET): $(PROBE_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $(PROBE_SOURCES)

clean:
	rm -f $(TARGET) $(PROBE_TARGET)

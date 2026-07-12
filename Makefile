PLUGIN  := libasteroidz_ws.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 json-glib-1.0
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/asteroidz-ws

$(PLUGIN): src/asteroidz_ws.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DATADIR)/layouts assets/layouts/*.svg
	@echo "installed to $(PREFIX)/$(PLUGIN) + layouts in $(DATADIR)/layouts"

clean:
	rm -f $(PLUGIN)

.PHONY: install clean

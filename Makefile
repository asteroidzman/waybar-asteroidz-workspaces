PLUGIN  := libasteroidz_ws.so
PKGS    := gtk+-3.0 gio-2.0 gio-unix-2.0 json-glib-1.0
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -fPIC $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))
PREFIX  ?= $(HOME)/.local/lib/waybar
DATADIR ?= $(HOME)/.local/share/asteroidz-ws
DESTDIR ?=

$(PLUGIN): src/asteroidz_ws.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDLIBS)

install: $(PLUGIN)
	install -Dm755 $(PLUGIN) $(DESTDIR)$(PREFIX)/$(PLUGIN)
	install -Dm644 -t $(DESTDIR)$(DATADIR)/layouts assets/layouts/*.svg
	install -Dm644 assets/logo.svg $(DESTDIR)$(DATADIR)/logo.svg
	@echo "installed to $(DESTDIR)$(PREFIX)/$(PLUGIN) + layouts/logo in $(DESTDIR)$(DATADIR)"

test_asteroidz_ws: tests/test_asteroidz_ws.c src/asteroidz_ws.c
	$(CC) $(CFLAGS) -o $@ tests/test_asteroidz_ws.c $(LDLIBS)

test: test_asteroidz_ws
	./test_asteroidz_ws

clean:
	rm -f $(PLUGIN) test_asteroidz_ws

.PHONY: install clean test

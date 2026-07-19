// Unit tests for asteroidz_ws.c's pure logic (logo_hex color math) and
// filesystem-only logic (resolve_socket) -- no GTK init, no Wayland, no
// live compositor. #includes the plugin source directly to reach its
// `static` functions without changing their visibility for production
// code; this file supplies its own main() (asteroidz_ws.c has none), so
// nothing conflicts.
//
// rebuild()/parse_monitors()/parse_clients() aren't unit-testable this way:
// parse_monitors/parse_clients both unconditionally call rebuild() at the
// end, which creates/destroys real GTK widgets against self->box -- unlike
// waybar-display's parse_monitors, the JSON-parsing logic here isn't
// cleanly separated from the widget-rebuilding side effect.
#include "../src/asteroidz_ws.c"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
	else { printf("ok - %s\n", msg); } \
} while (0)
#define CHECK_STR(a, b, msg) CHECK(strcmp((a), (b)) == 0, msg)

static int make_unix_socket(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return -1; }
	return fd;
}

int main(void) {
	// logo_hex: RGBA -> hex string blend math
	GdkRGBA orange = {1.0, 0.5, 0.0, 1.0};
	char out[8];
	logo_hex(orange, 0.0, 1.0, out);
	CHECK_STR(out, "#ff7f00", "logo_hex with to_white=0, mul=1 is a straight passthrough");

	logo_hex(orange, 1.0, 1.0, out);
	CHECK_STR(out, "#ffffff", "logo_hex with to_white=1.0 always blends fully to white");

	GdkRGBA c2 = {0.2, 0.6, 0.8, 1.0};
	logo_hex(c2, 0.25, 1.0, out);
	CHECK_STR(out, "#66b2d8", "logo_hex with a partial to_white blend computes the expected hex");

	GdkRGBA black = {0.0, 0.0, 0.0, 1.0};
	logo_hex(black, 0.0, 1.0, out);
	CHECK_STR(out, "#000000", "logo_hex of black with no white blend stays black");

	// resolve_socket: prefer an existing env-pointed socket; otherwise fall
	// back to the newest asteroidz-*.sock/mango-*.sock in XDG_RUNTIME_DIR
	char tmpdir[] = "/tmp/wstest-XXXXXX";
	if (!mkdtemp(tmpdir)) { fprintf(stderr, "FAIL: mkdtemp\n"); return 1; }
	g_setenv("XDG_RUNTIME_DIR", tmpdir, TRUE);

	// case 1: env signature points at a real, existing socket -> used as-is,
	// even though a newer one exists in the runtime dir
	char envsock[256]; snprintf(envsock, sizeof envsock, "%s/env-chosen.sock", tmpdir);
	int fd1 = make_unix_socket(envsock);
	CHECK(fd1 >= 0, "test setup: created the env-pointed socket");
	sleep(1);  // ensure a distinct, later mtime for the "newer" one below
	char newer[256]; snprintf(newer, sizeof newer, "%s/asteroidz-newer.sock", tmpdir);
	int fd2 = make_unix_socket(newer);
	CHECK(fd2 >= 0, "test setup: created a newer asteroidz-*.sock");
	g_setenv("ASTEROIDZ_INSTANCE_SIGNATURE", envsock, TRUE);
	char *resolved = resolve_socket();
	CHECK_STR(resolved, envsock, "resolve_socket prefers an existing env-pointed socket over a newer file");
	g_free(resolved);

	// case 2: env signature points at a STALE (nonexistent) path -> falls
	// back to scanning XDG_RUNTIME_DIR for the newest matching socket
	g_setenv("ASTEROIDZ_INSTANCE_SIGNATURE", "/tmp/does-not-exist-12345.sock", TRUE);
	resolved = resolve_socket();
	CHECK_STR(resolved, newer, "resolve_socket falls back to the newest asteroidz-*.sock when the env path is stale");
	g_free(resolved);

	close(fd1); close(fd2);
	unlink(envsock); unlink(newer);
	char *rmcmd = g_strdup_printf("rmdir %s", tmpdir);
	system(rmcmd);
	g_free(rmcmd);

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}

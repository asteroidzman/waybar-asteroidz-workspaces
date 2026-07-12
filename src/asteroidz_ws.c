// waybar CFFI plugin: asteroidz workspace (tag) pills with REAL app icons.
//
// Custom (script) modules are text-only, so per-workspace app icons can only be
// Nerd-Font glyphs. This plugin renders GtkImage app icons resolved from the
// desktop database / icon theme, matching the DankMaterialShell look.
//
// It connects to the asteroidz JSON-over-unix-socket IPC (ASTEROIDZ_INSTANCE_
// SIGNATURE, MANGO_INSTANCE_SIGNATURE fallback), watches all-monitors + all-
// clients, and rebuilds the pills live. Click a pill to view that tag.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

// ─── waybar CFFI ABI (official vendored header) ──────────────────────────────
#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

// ─── state ───────────────────────────────────────────────────────────────────
#define MAXTAGS 9

typedef struct {
  GtkWidget *box;                  // horizontal container of pills
  GtkIconTheme *icon_theme;
  int icon_size;                   // px
  int max_icons;                   // per tag
  int min_pills;                   // pad empties up to this many

  char *active_monitor;            // name of active monitor
  char tag_name[MAXTAGS + 1][128];
  int tag_clients[MAXTAGS + 1];
  int tag_active[MAXTAGS + 1];
  int tag_urgent[MAXTAGS + 1];
  GPtrArray *tag_appids[MAXTAGS + 1];  // char* app-ids on this tag (active monitor)
} Instance;

// ─── icon resolution: app-id -> GtkImage ─────────────────────────────────────
static GtkWidget *image_for_appid(Instance *self, const char *appid) {
  GIcon *gicon = NULL;
  GDesktopAppInfo *dai = NULL;

  if (appid && *appid) {
    char *desktop = g_strconcat(appid, ".desktop", NULL);
    dai = g_desktop_app_info_new(desktop);
    if (!dai) {
      char *low = g_ascii_strdown(appid, -1);
      char *ld = g_strconcat(low, ".desktop", NULL);
      dai = g_desktop_app_info_new(ld);
      g_free(ld);
      g_free(low);
    }
    g_free(desktop);
  }

  GtkWidget *img = NULL;
  if (dai) {
    gicon = g_app_info_get_icon(G_APP_INFO(dai));
    if (gicon)
      img = gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_BUTTON);
    g_object_unref(dai);
  }
  if (!img && appid && gtk_icon_theme_has_icon(self->icon_theme, appid)) {
    img = gtk_image_new_from_icon_name(appid, GTK_ICON_SIZE_BUTTON);
  }
  if (!img) {
    char *low = appid ? g_ascii_strdown(appid, -1) : NULL;
    if (low && gtk_icon_theme_has_icon(self->icon_theme, low))
      img = gtk_image_new_from_icon_name(low, GTK_ICON_SIZE_BUTTON);
    g_free(low);
  }
  if (!img)
    img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_BUTTON);

  gtk_image_set_pixel_size(GTK_IMAGE(img), self->icon_size);
  return img;
}

// ─── click handler: view a tag ───────────────────────────────────────────────
static void on_tag_clicked(GtkButton *btn, gpointer data) {
  (void)btn;
  int tag = GPOINTER_TO_INT(data);
  char arg[32];
  g_snprintf(arg, sizeof arg, "view,%d", tag);
  char *argv[] = {"amsg", "dispatch", arg, NULL};
  g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

// ─── rebuild the pills from current state ────────────────────────────────────
static void rebuild(Instance *self) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(self->box));
  for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);

  // relevant = occupied or active; compute max relevant index + count
  int maxrel = 0, relcount = 0;
  for (int n = 1; n <= MAXTAGS; n++) {
    if (self->tag_clients[n] > 0 || self->tag_active[n]) {
      relcount++;
      if (n > maxrel) maxrel = n;
    }
  }
  int need = self->min_pills - relcount;
  if (need < 0) need = 0;

  for (int n = 1; n <= MAXTAGS; n++) {
    int relevant = (self->tag_clients[n] > 0 || self->tag_active[n]);
    int padding = (n > maxrel && n <= maxrel + need);
    if (!relevant && !padding) continue;

    GtkWidget *btn = gtk_button_new();
    gtk_widget_set_focus_on_click(btn, FALSE);
    GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
    gtk_style_context_add_class(ctx, "ws-pill");
    if (self->tag_urgent[n]) gtk_style_context_add_class(ctx, "urgent");
    else if (self->tag_active[n]) gtk_style_context_add_class(ctx, "focused");
    else if (self->tag_clients[n] > 0) gtk_style_context_add_class(ctx, "occupied");
    else gtk_style_context_add_class(ctx, "empty");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    // label: "index" or "index: name" (when the tag is named)
    char label[160];
    char idx[8];
    g_snprintf(idx, sizeof idx, "%d", n);
    if (self->tag_name[n][0] && strcmp(self->tag_name[n], idx) != 0 &&
        (relevant))
      g_snprintf(label, sizeof label, "%d: %s", n, self->tag_name[n]);
    else
      g_snprintf(label, sizeof label, "%d", n);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);

    // app icons (deduped, capped)
    if (relevant && self->tag_appids[n]) {
      GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
      int shown = 0;
      for (guint i = 0; i < self->tag_appids[n]->len && shown < self->max_icons; i++) {
        const char *appid = g_ptr_array_index(self->tag_appids[n], i);
        if (!appid || g_hash_table_contains(seen, appid)) continue;
        g_hash_table_add(seen, (gpointer)appid);
        gtk_box_pack_start(GTK_BOX(hbox), image_for_appid(self, appid), FALSE, FALSE, 0);
        shown++;
      }
      g_hash_table_destroy(seen);
    }

    gtk_container_add(GTK_CONTAINER(btn), hbox);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_tag_clicked), GINT_TO_POINTER(n));
    gtk_box_pack_start(GTK_BOX(self->box), btn, FALSE, FALSE, 0);
  }
  gtk_widget_show_all(self->box);
}

// ─── parse all-monitors snapshot ─────────────────────────────────────────────
static void parse_monitors(Instance *self, const char *line) {
  JsonParser *p = json_parser_new();
  if (!json_parser_load_from_data(p, line, -1, NULL)) { g_object_unref(p); return; }
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  if (!root || !json_object_has_member(root, "monitors")) { g_object_unref(p); return; }
  JsonArray *mons = json_object_get_array_member(root, "monitors");

  JsonObject *active = NULL;
  for (guint i = 0; i < json_array_get_length(mons); i++) {
    JsonObject *m = json_array_get_object_element(mons, i);
    if (json_object_has_member(m, "active") && json_object_get_boolean_member(m, "active")) { active = m; break; }
  }
  if (!active && json_array_get_length(mons) > 0) active = json_array_get_object_element(mons, 0);
  if (!active) { g_object_unref(p); return; }

  g_free(self->active_monitor);
  self->active_monitor = g_strdup(json_object_has_member(active, "name")
                                      ? json_object_get_string_member(active, "name") : "");

  for (int n = 0; n <= MAXTAGS; n++) {
    self->tag_clients[n] = 0; self->tag_active[n] = 0; self->tag_urgent[n] = 0;
    self->tag_name[n][0] = '\0';
  }
  // asteroidz reports active_tags=[0] while the overview is open → treat as none active.
  if (json_object_has_member(active, "tags")) {
    JsonArray *tags = json_object_get_array_member(active, "tags");
    for (guint i = 0; i < json_array_get_length(tags); i++) {
      JsonObject *t = json_array_get_object_element(tags, i);
      int idx = json_object_has_member(t, "index") ? (int)json_object_get_int_member(t, "index") : 0;
      if (idx < 1 || idx > MAXTAGS) continue;
      self->tag_clients[idx] = json_object_has_member(t, "client_count") ? (int)json_object_get_int_member(t, "client_count") : 0;
      self->tag_active[idx] = json_object_has_member(t, "is_active") && json_object_get_boolean_member(t, "is_active");
      self->tag_urgent[idx] = json_object_has_member(t, "is_urgent") && json_object_get_boolean_member(t, "is_urgent");
      if (json_object_has_member(t, "name")) {
        const char *nm = json_object_get_string_member(t, "name");
        if (nm) g_strlcpy(self->tag_name[idx], nm, sizeof self->tag_name[idx]);
      }
    }
  }
  g_object_unref(p);
  rebuild(self);
}

// ─── parse all-clients snapshot ──────────────────────────────────────────────
static void parse_clients(Instance *self, const char *line) {
  JsonParser *p = json_parser_new();
  if (!json_parser_load_from_data(p, line, -1, NULL)) { g_object_unref(p); return; }
  JsonObject *root = json_node_get_object(json_parser_get_root(p));
  if (!root || !json_object_has_member(root, "clients")) { g_object_unref(p); return; }
  JsonArray *cl = json_object_get_array_member(root, "clients");

  for (int n = 0; n <= MAXTAGS; n++) {
    if (self->tag_appids[n]) g_ptr_array_set_size(self->tag_appids[n], 0);
    else self->tag_appids[n] = g_ptr_array_new_with_free_func(g_free);
  }
  for (guint i = 0; i < json_array_get_length(cl); i++) {
    JsonObject *c = json_array_get_object_element(cl, i);
    const char *mon = json_object_has_member(c, "monitor") ? json_object_get_string_member(c, "monitor") : NULL;
    if (self->active_monitor && mon && strcmp(mon, self->active_monitor) != 0) continue;
    const char *appid = json_object_has_member(c, "appid") ? json_object_get_string_member(c, "appid") : NULL;
    if (!appid) continue;
    if (!json_object_has_member(c, "tags")) continue;
    JsonArray *tg = json_object_get_array_member(c, "tags");
    for (guint j = 0; j < json_array_get_length(tg); j++) {
      int t = (int)json_array_get_int_element(tg, j);
      if (t >= 1 && t <= MAXTAGS)
        g_ptr_array_add(self->tag_appids[t], g_strdup(appid));
    }
  }
  g_object_unref(p);
  rebuild(self);
}

// ─── async line readers ──────────────────────────────────────────────────────
typedef struct { Instance *self; int kind; GDataInputStream *in; } ReadCtx;  // kind 0=mon 1=cli

static void read_line(ReadCtx *rc);

static void on_line(GObject *src, GAsyncResult *res, gpointer data) {
  ReadCtx *rc = data;
  gsize len = 0;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
  if (!line) { g_free(rc); return; }  // EOF / error → stop
  if (len > 0) {
    if (rc->kind == 0) parse_monitors(rc->self, line);
    else parse_clients(rc->self, line);
  }
  g_free(line);
  read_line(rc);
}

static void read_line(ReadCtx *rc) {
  g_data_input_stream_read_line_async(rc->in, G_PRIORITY_DEFAULT, NULL, on_line, rc);
}

static GDataInputStream *open_watch(const char *sockpath, const char *verb) {
  GError *err = NULL;
  GSocket *sock = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &err);
  if (!sock) { if (err) g_error_free(err); return NULL; }
  GSocketAddress *addr = g_unix_socket_address_new(sockpath);
  if (!g_socket_connect(sock, addr, NULL, &err)) {
    if (err) g_error_free(err);
    g_object_unref(addr); g_object_unref(sock); return NULL;
  }
  g_object_unref(addr);
  GSocketConnection *conn = g_socket_connection_factory_create_connection(sock);
  g_object_unref(sock);
  GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  g_output_stream_write_all(os, verb, strlen(verb), NULL, NULL, NULL);
  GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  return g_data_input_stream_new(is);  // keeps conn alive via the stream chain
}

// ─── CFFI entry points ───────────────────────────────────────────────────────
void *wbcffi_init(const wbcffi_init_info *info,
                  const wbcffi_config_entry *entries, size_t entries_len) {
  Instance *self = g_new0(Instance, 1);
  self->icon_theme = gtk_icon_theme_get_default();

  // GtkButton hover otherwise renders the cursor at GTK's default size; pin it to
  // the compositor's XCURSOR_SIZE so the cursor doesn't shrink over the pills.
  const char *xcs = g_getenv("XCURSOR_SIZE");
  int csize = (xcs && atoi(xcs) > 0) ? atoi(xcs) : 24;
  g_object_set(gtk_settings_get_default(), "gtk-cursor-theme-size", csize, NULL);
  self->icon_size = 18;
  self->max_icons = 3;
  self->min_pills = 3;
  for (size_t i = 0; i < entries_len; i++) {
    if (!strcmp(entries[i].key, "icon-size")) self->icon_size = atoi(entries[i].value);
    else if (!strcmp(entries[i].key, "max-icons")) self->max_icons = atoi(entries[i].value);
    else if (!strcmp(entries[i].key, "min-pills")) self->min_pills = atoi(entries[i].value);
  }

  GtkContainer *root = (GtkContainer *)info->get_root_widget(info->obj);
  self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(self->box, "asteroidz-ws");
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  const char *sig = g_getenv("ASTEROIDZ_INSTANCE_SIGNATURE");
  if (!sig || !*sig) sig = g_getenv("MANGO_INSTANCE_SIGNATURE");
  if (sig && *sig) {
    GDataInputStream *mon = open_watch(sig, "watch all-monitors\n");
    if (mon) { ReadCtx *rc = g_new0(ReadCtx, 1); rc->self = self; rc->kind = 0; rc->in = mon; read_line(rc); }
    GDataInputStream *cli = open_watch(sig, "watch all-clients\n");
    if (cli) { ReadCtx *rc = g_new0(ReadCtx, 1); rc->self = self; rc->kind = 1; rc->in = cli; read_line(rc); }
  }
  return self;
}

void wbcffi_deinit(void *instance) {
  Instance *self = instance;
  for (int n = 0; n <= MAXTAGS; n++)
    if (self->tag_appids[n]) g_ptr_array_free(self->tag_appids[n], TRUE);
  g_free(self->active_monitor);
  g_free(self);
}

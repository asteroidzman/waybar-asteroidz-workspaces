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
  double unfocused_saturation;     // icon saturation on unfocused occupied tags

  char *active_monitor;            // name of active monitor
  char tag_name[MAXTAGS + 1][128];
  int tag_clients[MAXTAGS + 1];
  int tag_active[MAXTAGS + 1];
  int tag_urgent[MAXTAGS + 1];
  char tag_focus_appid[MAXTAGS + 1][128];  // app-id of the focused window on this tag
  GPtrArray *tag_appids[MAXTAGS + 1];      // char* app-ids on this tag (active monitor)
  GPtrArray *tag_titles[MAXTAGS + 1];      // char* "app — title" for the tooltip
} Instance;

// ─── icon resolution: app-id -> GdkPixbuf (so we can desaturate/emphasise) ────
static GdkPixbuf *pixbuf_for_appid(Instance *self, const char *appid, int size) {
  GdkPixbuf *pb = NULL;
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

  if (dai) {
    GIcon *gicon = g_app_info_get_icon(G_APP_INFO(dai));  // borrowed
    if (gicon) {
      GtkIconInfo *ii = gtk_icon_theme_lookup_by_gicon(self->icon_theme, gicon, size,
                                                       GTK_ICON_LOOKUP_FORCE_SIZE);
      if (ii) {
        pb = gtk_icon_info_load_icon(ii, NULL);
        g_object_unref(ii);
      }
    }
    g_object_unref(dai);
  }
  if (!pb && appid && gtk_icon_theme_has_icon(self->icon_theme, appid))
    pb = gtk_icon_theme_load_icon(self->icon_theme, appid, size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
  if (!pb && appid) {
    char *low = g_ascii_strdown(appid, -1);
    if (gtk_icon_theme_has_icon(self->icon_theme, low))
      pb = gtk_icon_theme_load_icon(self->icon_theme, low, size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    g_free(low);
  }
  if (!pb)
    pb = gtk_icon_theme_load_icon(self->icon_theme, "application-x-executable", size,
                                  GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
  return pb;  // may be NULL
}

// Build a GtkImage for an app-id at a size, optionally desaturated (sat < 1).
static GtkWidget *image_for_appid(Instance *self, const char *appid, int size, double sat) {
  GdkPixbuf *pb = pixbuf_for_appid(self, appid, size);
  GtkWidget *img;
  if (pb) {
    if (sat < 0.999) {
      GdkPixbuf *d = gdk_pixbuf_copy(pb);
      gdk_pixbuf_saturate_and_pixelate(pb, d, sat, FALSE);
      img = gtk_image_new_from_pixbuf(d);
      g_object_unref(d);
    } else {
      img = gtk_image_new_from_pixbuf(pb);
    }
    g_object_unref(pb);
  } else {
    img = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(img), size);
  }
  return img;
}

// ─── click handler: view a tag ───────────────────────────────────────────────
// EventBox (not GtkButton): buttons own an input window that makes GTK render the
// cursor at its own size, so it doesn't match the compositor cursor. EventBoxes
// leave the cursor alone (like waybar's other modules). Spacing is applied with
// widget margins because GtkEventBox doesn't honour CSS padding/margin.
static gboolean on_tag_pressed(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w;
  if (ev->button != 1) return FALSE;
  int tag = GPOINTER_TO_INT(data);
  char arg[32];
  g_snprintf(arg, sizeof arg, "view,%d", tag);
  char *argv[] = {"amsg", "dispatch", arg, NULL};
  g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
  return TRUE;
}

// ─── rebuild the pills from current state ────────────────────────────────────
static void rebuild(Instance *self) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(self->box));
  for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);

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

    GtkWidget *btn = gtk_event_box_new();
    gtk_widget_add_events(btn, GDK_BUTTON_PRESS_MASK);
    gtk_widget_set_margin_top(btn, 9);      // 48px pill in a 66px bar
    gtk_widget_set_margin_bottom(btn, 9);
    gtk_widget_set_margin_start(btn, 3);     // gap between pills
    gtk_widget_set_margin_end(btn, 3);
    GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
    gtk_style_context_add_class(ctx, "ws-pill");
    int is_focused_tag = self->tag_active[n];
    if (self->tag_urgent[n]) gtk_style_context_add_class(ctx, "urgent");
    else if (is_focused_tag) gtk_style_context_add_class(ctx, "focused");
    else if (self->tag_clients[n] > 0) gtk_style_context_add_class(ctx, "occupied");
    else gtk_style_context_add_class(ctx, "empty");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(hbox, 12);   // horizontal padding inside the pill
    gtk_widget_set_margin_end(hbox, 12);

    char idx[8];
    g_snprintf(idx, sizeof idx, "%d", n);
    char label[160];
    if (self->tag_name[n][0] && strcmp(self->tag_name[n], idx) != 0 && relevant)
      g_snprintf(label, sizeof label, "%d: %s", n, self->tag_name[n]);
    else
      g_snprintf(label, sizeof label, "%d", n);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);

    // Group by app (one icon per unique app-id), emphasise the focused app,
    // desaturate on unfocused tags, and add a +N overflow chip.
    if (relevant && self->tag_appids[n]) {
      GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
      GPtrArray *uniq = g_ptr_array_new();  // borrowed pointers
      for (guint i = 0; i < self->tag_appids[n]->len; i++) {
        char *a = g_ptr_array_index(self->tag_appids[n], i);
        if (a && !g_hash_table_contains(seen, a)) {
          g_hash_table_add(seen, a);
          g_ptr_array_add(uniq, a);
        }
      }
      guint shown = 0;
      for (guint i = 0; i < uniq->len && (int)shown < self->max_icons; i++) {
        const char *appid = g_ptr_array_index(uniq, i);
        gboolean is_focus_app = (self->tag_focus_appid[n][0] &&
                                 strcmp(self->tag_focus_appid[n], appid) == 0);
        double sat = is_focused_tag ? (is_focus_app ? 1.0 : 0.9) : self->unfocused_saturation;
        int sz = is_focus_app ? self->icon_size + 3 : self->icon_size;
        GtkWidget *im = image_for_appid(self, appid, sz, sat);
        gtk_widget_set_valign(im, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(hbox), im, FALSE, FALSE, 0);
        shown++;
      }
      if (uniq->len > shown) {
        char plus[16];
        g_snprintf(plus, sizeof plus, "+%u", uniq->len - shown);
        GtkWidget *ov = gtk_label_new(plus);
        gtk_style_context_add_class(gtk_widget_get_style_context(ov), "overflow");
        gtk_box_pack_start(GTK_BOX(hbox), ov, FALSE, FALSE, 0);
      }
      g_ptr_array_free(uniq, TRUE);
      g_hash_table_destroy(seen);
    }

    // Tooltip: window titles on this tag.
    if (self->tag_titles[n] && self->tag_titles[n]->len) {
      GString *t = g_string_new(NULL);
      for (guint i = 0; i < self->tag_titles[n]->len; i++) {
        if (i) g_string_append_c(t, '\n');
        g_string_append(t, g_ptr_array_index(self->tag_titles[n], i));
      }
      gtk_widget_set_tooltip_text(btn, t->str);
      g_string_free(t, TRUE);
    }

    gtk_container_add(GTK_CONTAINER(btn), hbox);
    g_signal_connect(btn, "button-press-event", G_CALLBACK(on_tag_pressed), GINT_TO_POINTER(n));
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
    if (self->tag_titles[n]) g_ptr_array_set_size(self->tag_titles[n], 0);
    else self->tag_titles[n] = g_ptr_array_new_with_free_func(g_free);
    self->tag_focus_appid[n][0] = '\0';
  }
  for (guint i = 0; i < json_array_get_length(cl); i++) {
    JsonObject *c = json_array_get_object_element(cl, i);
    const char *mon = json_object_has_member(c, "monitor") ? json_object_get_string_member(c, "monitor") : NULL;
    if (self->active_monitor && mon && strcmp(mon, self->active_monitor) != 0) continue;
    const char *appid = json_object_has_member(c, "appid") ? json_object_get_string_member(c, "appid") : NULL;
    if (!appid) continue;
    const char *title = json_object_has_member(c, "title") ? json_object_get_string_member(c, "title") : "";
    gboolean focused = json_object_has_member(c, "is_focused") && json_object_get_boolean_member(c, "is_focused");
    if (!json_object_has_member(c, "tags")) continue;
    JsonArray *tg = json_object_get_array_member(c, "tags");
    for (guint j = 0; j < json_array_get_length(tg); j++) {
      int t = (int)json_array_get_int_element(tg, j);
      if (t < 1 || t > MAXTAGS) continue;
      g_ptr_array_add(self->tag_appids[t], g_strdup(appid));
      g_ptr_array_add(self->tag_titles[t],
                      (title && *title) ? g_strdup_printf("%s — %s", appid, title) : g_strdup(appid));
      if (focused) g_strlcpy(self->tag_focus_appid[t], appid, sizeof self->tag_focus_appid[t]);
    }
  }
  g_object_unref(p);
  rebuild(self);
}

// ─── async line readers ──────────────────────────────────────────────────────
typedef struct { Instance *self; int kind; GDataInputStream *in; } ReadCtx;

static void read_line(ReadCtx *rc);

static void on_line(GObject *src, GAsyncResult *res, gpointer data) {
  ReadCtx *rc = data;
  gsize len = 0;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
  if (!line) { g_free(rc); return; }
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
  return g_data_input_stream_new(is);
}

// ─── CFFI entry points ───────────────────────────────────────────────────────
void *wbcffi_init(const wbcffi_init_info *info,
                  const wbcffi_config_entry *entries, size_t entries_len) {
  Instance *self = g_new0(Instance, 1);
  self->icon_theme = gtk_icon_theme_get_default();
  self->icon_size = 18;
  self->max_icons = 3;
  self->min_pills = 3;
  self->unfocused_saturation = 0.4;

  int cursor_size = 0;  // 0 = leave GTK default alone
  for (size_t i = 0; i < entries_len; i++) {
    if (!strcmp(entries[i].key, "icon-size")) self->icon_size = atoi(entries[i].value);
    else if (!strcmp(entries[i].key, "max-icons")) self->max_icons = atoi(entries[i].value);
    else if (!strcmp(entries[i].key, "min-pills")) self->min_pills = atoi(entries[i].value);
    else if (!strcmp(entries[i].key, "unfocused-saturation")) self->unfocused_saturation = atof(entries[i].value);
    else if (!strcmp(entries[i].key, "cursor-size")) cursor_size = atoi(entries[i].value);
  }

  // Match the compositor's cursor so the pointer size doesn't change over the
  // module. GTK otherwise picks its own size (XCURSOR_SIZE / gsettings), which may
  // differ from the compositor's. Set "cursor-size" in the module config to the
  // compositor's cursor size; the theme name follows XCURSOR_THEME.
  GtkSettings *settings = gtk_settings_get_default();
  const char *ctheme = g_getenv("XCURSOR_THEME");
  if (ctheme && *ctheme) g_object_set(settings, "gtk-cursor-theme-name", ctheme, NULL);
  if (cursor_size > 0) g_object_set(settings, "gtk-cursor-theme-size", cursor_size, NULL);

  GtkContainer *root = info->get_root_widget(info->obj);
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
  for (int n = 0; n <= MAXTAGS; n++) {
    if (self->tag_appids[n]) g_ptr_array_free(self->tag_appids[n], TRUE);
    if (self->tag_titles[n]) g_ptr_array_free(self->tag_titles[n], TRUE);
  }
  g_free(self->active_monitor);
  g_free(self);
}

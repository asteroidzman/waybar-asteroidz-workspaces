# waybar-asteroidz-workspaces

A [waybar](https://github.com/Alexays/Waybar) **CFFI plugin** that renders
[asteroidz](https://github.com/asteroidzman/asteroidz) workspace
**tags as pills with real application icons** — the way DankMaterialShell shows
them. Waybar's `custom` modules are text-only, so per-workspace app icons
otherwise have to be Nerd-Font glyphs; this plugin uses actual `GtkImage` icons
resolved from the desktop database / icon theme.

## Features

- One pill per occupied/focused tag (empties padded up to `min-pills`).
- Real app icons per tag (deduped, capped at `max-icons`), from `*.desktop` /
  the GTK icon theme.
- `index` or `index: name` label (named tags show their name).
- Live updates via the asteroidz JSON socket (`watch all-monitors` +
  `all-clients`) — no polling.
- Click a pill to `view` that tag.
- CSS classes for styling: `.ws-pill` plus `.focused` / `.occupied` / `.empty`
  / `.urgent`.

## Build & install

Requires `gtk3`, `glib2`, `json-glib` (and their dev headers) and a C compiler.

```sh
make
make install                 # → ~/.local/lib/waybar/libasteroidz_ws.so
# or: PREFIX=/usr/lib/waybar sudo make install
```

## waybar config

```jsonc
"modules-left": ["cffi/asteroidz_ws"],

"cffi/asteroidz_ws": {
    "module_path": "/home/YOU/.local/lib/waybar/libasteroidz_ws.so",
    "icon-size": 18,
    "max-icons": 3,
    "min-pills": 3
}
```

## style.css

Pills are `GtkButton`s carrying `.ws-pill` plus a state class. (The plugin pins
GTK's cursor size to `XCURSOR_SIZE` so the pointer doesn't shrink on hover.)

```css
#asteroidz-ws .ws-pill {
    border-radius: 9px;
    padding: 0 10px;
    margin: 9px 3px;
}
#asteroidz-ws .ws-pill.focused  { background-color: @primary; color: @on_primary; }
#asteroidz-ws .ws-pill.occupied { background-color: @surface_container_high; }
#asteroidz-ws .ws-pill.empty    { background-color: transparent; color: @outline; }
#asteroidz-ws .ws-pill.urgent   { background-color: @error; color: @on_error; }
```

## License

MIT

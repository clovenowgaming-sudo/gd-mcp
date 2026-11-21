# GD MCP (Geode mod)

Geode mod for **Geometry Dash 2.2081** + **Geode SDK 5.11.2** (Android) that embeds an
[MCP](https://modelcontextprotocol.io) server (Streamable HTTP, JSON) giving an AI client
**full control** of the game: change the player's icon/colors, click any button on screen,
play levels, and build/edit levels in the editor.

## Build (GitHub Actions, no PC needed)
1. Push this folder to a GitHub repo.
2. Actions -> **Build Geode Mod** -> the `.geode` is attached to a GitHub Release (`build-N`).
3. Put it in the Geode mods folder of GD (Android: `Android/media/com.geode.launcher/game/geode/mods/`).

Local build (if you have the SDK): `geode build` (or cmake with `GEODE_SDK` set).

## Use
1. Start GD. A toast says `GD MCP: listening on port 8766`.
2. Open mod settings -> copy the **Secret token**.
3. Connect a client:

**Claude Code / any local client on the same phone (Termux):**
```
claude mcp add --transport http gd http://127.0.0.1:8766/mcp --header "Authorization: Bearer <token>"
```

**Claude.ai / ChatGPT (need a public HTTPS URL)** - tunnel from Termux:
```
pkg install cloudflared
cloudflared tunnel --url http://127.0.0.1:8766
```
Then add a custom connector with URL `https://<random>.trycloudflare.com/mcp/<token>` (no other auth).

## Tools

**Full game control:** `game_status`, `list_levels`, `open_level`, `play_level`, `return_to_menu`,
`list_buttons`, `click_tag`, `click_id`, `tap_screen`, `get_icons`, `set_icon`, `set_colors`.

**Level editor** (same as the old editor-only bridge): `editor_status`, `list_objects`,
`place_object`, `place_objects`, `fill_rect`, `place_line`, `delete_objects`, `edit_objects`,
`select_objects`, `import_objects`, `get_level_string`, `history` (undo/redo), `playtest`,
`save_level`, `scroll_to`, `screenshot`, `backup_level`, `restore_backup`, `clear_level`.

Coordinates in editor tools are level units (30 = 1 block). `tap_screen` / `list_buttons`
coordinates are screen pixels, origin bottom-left.

## Safety
- Token required on every request; server listens on 127.0.0.1 by default.
- `edit_objects` is not undoable -> `backup_level` first. `clear_level` / `import_objects(replace)` auto-backup.
- Backups live in the mod save dir under `backups/`.
- `set_icon` lets the client equip icons regardless of unlock state in some cases; it does not
  unlock anything server-side, it just points GameManager at an icon ID.

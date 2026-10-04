# wow-world-editor

Standalone world editor for WoW 3.3.5a (build 12340) and AzerothCore. Design spec:
the "WoW 3.3.5 World Editor — Design Spec" doc.

Current state: **M1 in progress.** Docked editor UI (menu, mode toolbar, status bar, Viewport,
Tools, Maps, Inspector, Changes, Problems, Log, Ctrl+P command palette), projects with saved change
history, undo/redo, and the first adapter: terrain height sculpting (raise, lower, flatten, smooth)
with export of patched ADTs and a one-click play test into the WXL client overlay.
See `docs/change-model.md`.

## Build

Windows, Visual Studio 2022, CMake 3.24+. Dependencies (Dear ImGui docking, StormLib,
nlohmann/json, ImGuizmo) are fetched at configure time. The AzerothCore database link uses the MySQL 8.x client of a
local MySQL install (`-DMYSQL_DIR=...`, default `C:/Program Files/MySQL/MySQL Server 8.4`); its DLLs are copied next
to the exe and loaded on first use.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Run

1. Start `build\Release\wow-world-editor.exe`.
2. **File > New project**: name, parent folder, and the WXL client folder (the one with `Data`).
3. In **Maps**, pick a map and click a lit square in the tile grid.
4. Sculpt with the left mouse (B), select chunks (V); right drag looks, WASD/Q/E move.
5. **Ctrl+S** saves, **F5** play-tests (exports into the client overlay; relog to see it).
6. **File > Server setup** links the AzerothCore server: "Read settings from worldserver.conf" fills the database and
   SOAP fields; SOAP needs `SOAP.Enabled = 1` and a game account with GM level 3. Passwords go to the Windows
   credential store, the rest to `%APPDATA%\wow-world-editor\profiles.json`; the project keeps only the profile name.
   The **Server** panel runs GM commands; **Problems** lists what export would refuse or fix (click to go there).

The last project reopens on start. Panel layout is kept in `imgui.ini` (View > Reset panel layout).

## Checks

```
build\Release\wow-world-editor.exe --selftest
build\Release\wow-world-editor.exe --check "<client>\Data" Azeroth 32 48
```

`--server-check "<AzerothCore server folder>" [soap account] [password]` reads `configs\worldserver.conf`, queries
the world database and runs `server info` over SOAP.

`--spawn-check "<AzerothCore server folder>"` runs the creature and gameobject adapters against the world database (place, move,
undo, redo, reload, export, edit an existing row) and leaves the database as it found it. `--sql "<server folder>"
"<query>"` prints a query's rows.

`--anim-check "<client>\Data" <model.m2> [...]` loads M2 skeletons and checks the Stand animation moves skinned
vertices without flying apart (`WWE_GEOSETS=1` also lists submesh ids). `--render` draws spawns from a server when
`WWE_SERVER="<server folder>"` is set (`WWE_LIST=1` prints the looks of spawns near the camera).
`--map-preview "<client>\Data" <map> <out.png>` writes the Maps panel picture of a map (WDL heights, tile grid).

`--selftest` runs the parser checks on synthetic data (exit 0 = pass). `--check` parses one real
tile and its textures without opening a window and prints a summary.

## Layout

| File | Contents |
| --- | --- |
| `src/Mpq.*` | MPQ chain in client priority order (StormLib) |
| `src/Formats.*` | ADT, WDT and BLP parsers; no GPU code |
| `src/Renderer.*` | D3D11 terrain, placement boxes, overlay lines |
| `src/Changes.*` | Change, Adapter, ChangeStore (undo/redo, save/load) |
| `src/Project.*` | Project folder and project.json |
| `src/Terrain.*` | Terrain-height adapter: tile loading, picking, sculpt brush, ADT export |
| `src/App.*` | Editor UI: panels, tools, camera, commands, dialogs |
| `src/AppServer.cpp` | Server setup wizard, Server panel (GM commands), Problems panel |
| `src/Loader.*` | Background tile preparation: reads/parses ADTs, decodes textures, builds model meshes off the UI thread |
| `src/Spawns.*` | Spawn adapter (world.creature / world.gameobject): rows as changes (full rows before/after), DB writes, SQL export |
| `src/AppPopulate.cpp` | Creatures (N) and Gameobjects (I) tools: template search, place, click/box select (marker or model), group move/edit/delete, show toggles, markers, labels, spawn models |
| `src/Tables.*` | Generic table-rows adapter (all rows of a key as one change): waypoint_data by path id, creature_addon by guid |
| `src/Paths.*` | Creature waypoint path: load/save as one grouped change (creature.MovementType, creature_addon.path_id, waypoint_data) |
| `src/AppPaths.cpp` | Path editing in the Creatures tool: points as solid spheres with move handles, add/delete/drop to ground, wait and walk/run per point, walk preview, .wp reload |
| `src/Looks.*` | Display ids to models: creature skins, humanoid NPCs (baked skin, hair, geosets, helmet/shoulders/weapons), gameobjects |
| `src/Server.*` | Server profiles (per user), Windows credential store, MySQL link, SOAP commands |
| `src/main.cpp` | Window, device, frame loop, `--selftest` and `--check` |

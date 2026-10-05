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

### Differences: review another version's edits

**Find differences with another version...** in the Versions window (or Catalog > **Differences**, Terrain group F1) compares the
open map, as the project has it, with one other version: one of its `<map>_*` copies or the same map in an attached
client. **Scan** reads every tile of that version on a background thread; a progress bar (tiles done, time left)
shows in the tab and in the status bar, and you can keep working meanwhile. Touching edited chunks are grouped into
areas, each a card with a picture of the other version (areas over 9 tiles get none), its zone name, chunk count
and what changed (heights, textures, holes, water, new objects, new terrain). Sort by size, height change, new
objects or map position; search by area name.

- **Click a card**: the camera flies there, the area is selected and shown in place as that version (blended, like a
  compare). Shift/Ctrl still change the selection.
- **Enter** approves: the version is pasted (one undo step) and the area's chunks, blend band included, are marked
  done. **Del** rejects it (Catalog: tick **Rejected** to see those; right-click puts one back). **Esc** closes.
- Verdicts and per-tile results are saved under `<project>/differences/`. **Rescan** reuses every tile whose file and
  project edits have not changed, so it takes seconds; approved and rejected areas stay as decided.
- Objects only your map has are noted on a card but are not an edit by themselves (a paste cannot remove them).
  Areas whose tiles your map lacks altogether (new islands, say) cannot be pasted yet: they are counted and hidden
  behind the **New terrain** checkbox. An area larger than one tile's worth of chunks is split into one card per tile.
- Measured: a whole Eastern Kingdoms scan (687-741 tiles) takes about 10 s; a rescan about 6 s.

### Water in copy and paste

Copies carry each chunk's terrain water (MH2O; old-style MCLQ water from 1.12-era maps is converted). A paste replaces
the water of every pasted chunk with the copy's, so a dry cave pasted into a lake removes the lake water there; the blend
band around the paste keeps its own. Untick **Water** in the Copy tool's Placement tab to leave the target's water alone.
Undo, blueprints (saved from now on), rotation and export all include it; export writes the tile's MH2O afresh, and the
client then ignores the old MCLQ data of that tile. Water that belongs to a WMO (a building's or cave model's own
liquid) comes with the model as before.

### Comparing versions of an area

Select chunks (V or C), then press **]** (or **Versions > Compare versions**). The selection is shown, in place and
blended like a pinned paste, as each other version of it in turn: every ghost layer, plus every map whose folder starts
with this map's name (`Azeroth_Asc`, `Azeroth_Epoch`, `Azeroth_Turtle`, ... over `Azeroth`). Nothing needs to be soloed
or set up first; those map copies are loaded as hidden layers for the selected tiles only and dropped when the compare ends.

- **[ / ]** cycle versions. Versions identical to the map here, or not covering the selection, are skipped. While you
  flick through them the preview is a hard paste (a few ms); the blended result follows once you pause for 0.25 s. Each
  version's copy and plan are kept, so going back to one is instant, and the models a version would add load in the
  background as soon as its numbers are in.
- **Shift+click / Shift+drag** adds chunks, **Ctrl** removes them; the preview and the numbers follow the selection.
- **Enter** pastes the shown version and ends the compare (one undo step); **Esc** ends it and the map comes back.

Per version (Versions window table, and the blue line in the viewport): chunks that differ (heights > 0.5 yd, textures,
holes or water), mean / max height difference, chunks whose water differs (fishing/fatigue masks ignored, map editors
rewrite them), the largest height step on the selection's outer edge (what the blend band has to
absorb; orange above 2 yd), objects only the version has (+, added by the paste) and only the map has (-, left standing:
delete them by hand), and textures/models the version names that only another client has (copied on export) or no
source has (missing). A pasted version adds only its new objects, so placements both share are not doubled.

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
`--compare-check "<client>\Data" <map> <x> <y>` checks the chunk edge layout the compare reads, that the map compared
with itself is identical, prints every `<map>_*` copy's difference for a 4x4-chunk area and the whole tile, and times one
cycling step (difference, copy, blended and hard plan, preview).
`--diff-check "<client>\Data" <base map> <other map>` scans a whole map pair, prints progress and the areas found, rejects the
largest, then scans again: every tile must come from the saved results and the rejection must hold.
`--water-check "<client>\Data" <map> <x> <y>` pastes a dry chunk over a wet one and back on a tile with both, checks
undo/redo and four quarter turns, then exports and reads the tile back (pasted water exact, every other chunk's kept).

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
| `src/Ghosts.*` | Ghost layers (other clients, single archives, other maps at the same coordinates), background streaming, `CompareArea` |
| `src/Differences.*` | Whole-map scan of another version on a worker thread, areas of touching edited chunks, saved results and verdicts |
| `src/AppDifferences.cpp` | Catalog > Differences: scan controls, progress, cards with pictures, review / approve / reject |
| `src/AppCompare.cpp` | Compare selection: cycle versions of the selected chunks in place, per-version difference table |
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

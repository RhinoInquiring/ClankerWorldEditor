# wow-world-editor

Standalone world editor for WoW 3.3.5a (build 12340) and AzerothCore. Design spec:
the "WoW 3.3.5 World Editor — Design Spec" doc.

Current state: **M1 done, merging tools in use.** Docked editor UI (menu, tool groups, status bar, Viewport,
Tools, Maps, Inspector, Catalog, Versions, Changes, Problems, Log, Ctrl+P command palette), projects with saved
change history and undo/redo. Terrain: sculpt, texture paint, holes, copy/paste with seam blending, water,
objects, zones and world maps; creatures, gameobjects and their paths in the AzerothCore database. Map versions:
ghost layers, in-place compare of a selection across versions, and the Differences catalog that scans a whole
other version and lets you approve its edits area by area, new tiles included. Export writes patched ADTs (and
WDTs for added tiles) for a one-click play test in the WXL client. See `docs/change-model.md`.

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
5. **Ctrl+S** saves, **F5** play-tests (exports into the WXL client overlay; relog to see it).
   **Ctrl+Shift+E** (File > Build patch MPQ) builds the patch any 3.3.5 client loads, no WXL needed:
   `<project>/out/patch-enUS-Z.MPQ`. **File > Build patch MPQ and install into client** also copies it into the
   client's `Data\enUS\` (close the client first; it holds its archives open).
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
  An area larger than one tile's worth of chunks is split into one card per tile.
- **New terrain** (tiles your map lacks altogether, e.g. Turtle's islands): reviewing such a card shows those tiles
  alone (solo) from the other version; **Enter** adds them whole, as one undo step. Each is converted to your map's
  alpha format, ground effects your client lacks are dropped, and objects get fresh unique ids (a placement a
  neighbouring tile already lists keeps its id: it is the same object). The tiles are kept in `<project>/tiles/` and
  served through `<project>/overlay/` (with a WDT that lists them), so streaming, compare, scans and export all see
  them; the overlay is rebuilt from the changes when the project opens. The map's low-detail WDL gets each added
  tile's far heights (chunk corners and centres from the tile itself; within 1 yd of what Blizzard's tools write),
  so it shows from a distance in the editor and the client. The other version's minimap image of the tile is kept
  beside it. Export writes the tiles, the map's WDT and WDL, the minimap images and the client's md5translate.trs
  with their lines added.
  Where your map's own neighbouring tiles differ, the seam shows in Problems: paste or sculpt across it.
  **Only new terrain** lists just these cards.
- Measured: a whole Eastern Kingdoms scan (687-741 tiles) takes about 10 s; a rescan about 6 s.

### Export and the patch MPQ

- **Ctrl+E** writes the client files to `<project>/out/client`, starting from an empty folder each time (an undone
  edit leaves nothing behind); server files go to `<project>/out/server` (spawn/path SQL, server DBCs).
- **F5** does the same and mirrors `out/client` into the WXL extension's overlay (files no longer exported are taken
  out), for a quick relog test.
- **Ctrl+Shift+E** exports, then packs `out/client` into one MPQ (StormLib: zlib, (listfile), (attributes)):
  tiles, WDT/WDL of added tiles, minimap images and md5translate.trs, client DBCs, world map art, and the files
  copied from other clients. The name is `patchName` in project.json, default `patch-enUS-Z.MPQ`: it loads after
  every `patch-X.MPQ` (the map modules use letters up to Z), so its files win. Archives in the client that would load
  after it are listed as warnings. Install puts it in `Data\enUS\` (`Data\` for a non-locale name).
- The patch carries the client side only: the server still needs `out/server` applied and, after terrain or area
  edits, its maps extracted again from the patched client.

### Minimaps

Every export draws the minimap of each edited tile of the open map again, as it now looks: straight down, terrain,
buildings and water, no trees or editor outlines (the client's own minimaps leave doodads out too), 256 x 256, in the
client's layout (checked against Blizzard's pictures). Only tiles whose edits changed since the last export are drawn
(`<project>/minimaps/`, with `index.json` holding the edits each picture shows). Export ships them as
`textures/Minimap/wwe_<map>_<x>_<y>.blp` with the client's md5translate.trs plus their lines; a drawn picture wins
over an added tile's picture from its other version. Tiles edited on another map keep their last picture until their
map is open at an export. Building interiors (caves) use the buildings' own minimaps, which come with the map modules.

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
- Objects: a doodad comes with the chunk its origin stands on; a building (WMO) comes with every area its bounds
  reach, so a cave whose origin lies off to one side still comes with the area it runs through. The paste waits for
  the tile holding each carried object's origin (the object is filed there). The Copy tool keeps the origin rule,
  so copying a chunk in a city does not drag the whole city model along.

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
`--diff-objects "<client>\Data" <base map> <other map> <zone id>` lists, for every difference area in a zone, the
buildings the compare carries and every building of the other version reaching into it (origin inside or not, on the
map already or not, model in the client or not), e.g. `Kalimdor Kalimdor_Turtle 400` for Thousand Needles.
`--mpq-check <folder> [keep.MPQ]` packs a folder the way the patch is packed, opens it as a client Data folder and
compares every file read back (the tiles check does this with a real export too).
`--minimap-check "<client>\Data" <map> <x> <y> [out.png]` draws the tile as the editor's minimaps are drawn and saves it
beside the client's own minimap of it (`<out>.client.png`) to compare; it also prints a rough match score per
rotation/mirror (lighting differs too much for the score to decide alone).
`--tiles-check "<client>\Data" <base map> <other map>` adds two tiles only the other map has (with objects when it
can) in a scratch project and checks heights, alpha, water, object ids, far heights (also against Blizzard's WDL on 20
stock tiles), undo/redo, the rebuilt overlay and the export (ADT, WDT, WDL, minimap images and md5translate.trs).
`--water-check "<client>\Data" <map> <x> <y>` pastes a dry chunk over a wet one and back on a tile with both, checks
undo/redo and four quarter turns, then exports and reads the tile back (pasted water exact, every other chunk's kept).

`--selftest` runs the parser checks on synthetic data (exit 0 = pass). `--check` parses one real
tile and its textures without opening a window and prints a summary.

## Layout

| File | Contents |
| --- | --- |
| `src/Mpq.*` | MPQ chain in client priority order (StormLib), fallbacks to attached clients, the project overlay for added tiles |
| `src/Formats.*` | ADT, WDT, WDL, DBC and BLP parsers and writers (MH2O, rewritten ADTs, WDT tile flags); no GPU code |
| `src/Renderer.*` | D3D11 terrain, placement boxes, overlay lines |
| `src/Changes.*` | Change, Adapter, ChangeStore (undo/redo, save/load) |
| `src/Project.*` | Project folder and project.json |
| `src/Terrain.*` | Terrain adapter: tile loading, picking, sculpt/paint/holes/areas, copy/paste (heights, textures, water, objects), added tiles, ADT export |
| `src/App.*` | Editor UI: panels, tools, camera, commands, dialogs |
| `src/Ghosts.*` | Ghost layers (other clients, single archives, other maps at the same coordinates), background streaming, `CompareArea` |
| `src/Differences.*` | Whole-map scan of another version on a worker thread, areas of touching edited chunks, saved results and verdicts |
| `src/AppDifferences.cpp` | Catalog > Differences: scan controls, progress, cards with pictures, review / approve / reject |
| `src/Minimap.*` | Top-down orthographic render (minimaps, world maps, thumbnails) and the minimap look |
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

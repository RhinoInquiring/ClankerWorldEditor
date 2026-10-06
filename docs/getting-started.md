# Getting started

[Back to the README](../README.md)

## Build

Windows, Visual Studio 2022 (or newer), CMake 3.24+.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Dear ImGui (docking), ImGuizmo, StormLib and nlohmann/json are fetched at configure time. The AzerothCore database
link uses the MySQL 8.x client of a local MySQL install (`-DMYSQL_DIR=...`, default
`C:/Program Files/MySQL/MySQL Server 8.4`); its DLLs are copied next to the exe and loaded only when a server is linked.

## First project

1. Start `build\Release\wow-world-editor.exe`.
2. **File > New project**: a name, a parent folder, and your 3.3.5 client folder (the one holding `Data`).
3. In **Maps**, pick a map. The picture shows the whole map from its low-detail heights; lit squares are tiles. Click
   one to fly there.
4. Edit. The project saves itself a moment after each edit (**Ctrl+S** saves at once); **Ctrl+Z / Ctrl+Y** undo and redo.
5. **Ctrl+Shift+E** builds the patch MPQ ([Export and the patch MPQ](output/export-and-patch.md)).

The last project reopens on start. To compare against other clients or stack mods on your client, see
[Sources](concepts/sources.md). To link an AzerothCore server, see [Server link](server/server-link.md).

## The window

| Panel | What it shows |
| --- | --- |
| Viewport | The world; tools act here |
| Tools | The current tool's settings, in tabs |
| Maps | Every map of the client, the whole-map picture and its tile grid (orange = edited, blue outline = loaded). A WMO-only map (most dungeons) shows its WMO and an **Open this map** button instead: spawns, paths, triggers, POIs and flight nodes then work on its floors; terrain tools have nothing to edit there |
| Inspector | What the tool works on: the chunk, object, spawn or area under the cursor or selected, every field |
| Object | Transform of the selected objects |
| Catalog | Models, buildings, textures, units, blueprints, differences, portal effects ([Catalog](objects/catalog.md)) |
| Versions | Ghost layers, compare, sources ([Ghost layers](versions/ghost-layers.md)) |
| Changes | The project's edits, newest first; filter them, click one to go where it was made, right-click to undo or redo up to it |
| Problems | What an export would refuse or fix; click one to go there. The status bar shows the count (click it to come here) |
| Server | GM command console (up / down: commands sent before) |
| Log | What the editor did; errors in red, things to act on in amber (the latest also shows in the status bar for a few seconds); filter, copy |

Panels dock anywhere; **View > Reset panel layout** puts them back. **Ctrl+P** opens the command palette: every
command by name, every tool included. **Ctrl+/** (Help > Keyboard shortcuts) lists every key and mouse gesture.
The toolbar's **NPCs** button opens the [NPC viewer](units/npc-viewer.md).

With no project open, the viewport shows the start screen: new, open, and the recent projects. The editor reopens the
last project on start. Per-user settings (panel layout, recent projects, server profiles) live in
`%APPDATA%\wow-world-editor`.

While flying the camera (right drag), the mouse wheel sets its speed.

Sizes, scales, angles and other values with limits are sliders: drag the bar, or **Ctrl+click** it to type a value.
Positions have no limits and stay as drag fields (drag sideways, or double-click to type).

## Tool groups

Tools are grouped; **F1 to F4** switch groups, and each group remembers its last tool and brings its panels forward.

| Group | Tools (key) |
| --- | --- |
| F1 Terrain | Select (V), Sculpt (B), Paint (T), Holes (H), Copy (C) |
| F2 Objects | Objects (O) |
| F3 Units | Creatures (N), Gameobjects (I) |
| F4 Regions | Zones (Z), Triggers (K), POIs (J), Flights (Y) |

## Camera

| Input | Action |
| --- | --- |
| Right drag | Look |
| W A S D, Q / E | Move, down / up (Shift: fast) |
| Wheel | Move forward / back |
| F | Focus the tile under the camera |
| Ctrl+wheel | Brush radius (sculpt, paint, holes, zones) |

Tiles stream in around the camera (load radius in the View tab). Far terrain, level of detail, object boxes and
wireframe are in the **View** menu.

## Saving and history

Every edit is a change in the project's history ([Change model](concepts/change-model.md)). The history and project
settings are saved about two seconds after each edit (database edits are live at once, so the history that undoes
and reverts them is written right behind them); **Ctrl+S** saves at once, and a project with unsaved edits asks
before closing. Opening a project replays its history over the
game files, so the client's files are never modified.

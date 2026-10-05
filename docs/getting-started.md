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
4. Edit. **Ctrl+S** saves, **Ctrl+Z / Ctrl+Y** undo and redo.
5. **Ctrl+Shift+E** builds the patch MPQ ([Export and the patch MPQ](output/export-and-patch.md)).

The last project reopens on start. To compare against other clients or stack mods on your client, see
[Sources](concepts/sources.md). To link an AzerothCore server, see [Server link](server/server-link.md).

## The window

| Panel | What it shows |
| --- | --- |
| Viewport | The world; tools act here |
| Tools | The current tool's settings, in tabs |
| Maps | Every map of the client, the whole-map picture and its tile grid (orange = edited, blue outline = loaded) |
| Inspector | What the tool works on: the chunk, object, spawn or area under the cursor or selected, every field |
| Object | Transform of the selected objects |
| Catalog | Models, buildings, textures, units, blueprints, differences ([Catalog](objects/catalog.md)) |
| Versions | Ghost layers, compare, sources ([Ghost layers](versions/ghost-layers.md)) |
| Changes | The project's edits, newest last |
| Problems | What an export would refuse or fix; click one to go there |
| Server | GM command console |
| Log | What the editor did |

Panels dock anywhere; **View > Reset panel layout** puts them back. **Ctrl+P** opens the command palette: every
command by name.

## Tool groups

Tools are grouped; **F1 to F4** switch groups, and each group remembers its last tool and brings its panels forward.

| Group | Tools (key) |
| --- | --- |
| F1 Terrain | Select (V), Sculpt (B), Paint (T), Holes (H), Copy (C) |
| F2 Objects | Objects (O) |
| F3 Units | Creatures (N), Gameobjects (I) |
| F4 Regions | Zones (Z) |

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

Every edit is a change in the project's history ([Change model](concepts/change-model.md)). **Ctrl+S** saves the
history and project settings; an unsaved project asks before closing. Opening a project replays its history over the
game files, so the client's files are never modified.

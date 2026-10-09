# Command-line checks

[Back to the README](../../README.md)

`wow-world-editor.exe` runs headless when given one of these; each prints what it did and exits with 0 when every
expectation held. Most need a 3.3.5 client's Data folder (`"<client>\Data"` below); rendering checks use a software
Direct3D device. Checks that write files use a temporary folder and remove it.

## Core

| Command | Checks |
| --- | --- |
| `--selftest` | Parsers, writers, change store, clipboard rotation, blueprints, on synthetic data |
| `--check <Data> <map> <x> <y>` | Parses one real tile and its textures |
| `--validate <adt files...>` | Structural check of ADTs as the client reads them |
| `--validate-refs <Data> <adt files...>` | Every model and texture the tiles use exists |
| `--ground-effects <Data> <adt files...>` | Every ground effect the tiles use exists in the client |
| `--rewrite-check <Data> <map> <x> <y>` | Rewrites a tile's layers, re-parses and validates it |
| `--normals-check <Data> <map> <x> <y>` | Recomputed normals against Blizzard's |
| `--loader-check <Data> <map> <x> <y> [tiles] [yd/s]` | Flies across tiles through the background loader with one worker, then with the editor's count: time to load a jump's 5 x 5 tiles, frame times, same tiles and objects |
| `--stream-check <project> <map> <x> <y> [tiles] [nofallback]` | Opens the project like the window, walks `tiles` tiles east: time per step and read counters |

## Terrain and objects

| Command | Checks |
| --- | --- |
| `--plan-check <Data>` | Copy, rotate, blended paste, objects, furniture carrying, area paint, export |
| `--water-check <Data> <map> <x> <y>` | Dry over wet and back, undo/redo, rotation, Map's water paste, export of water |
| `--water-tool-check <Data>` | Water tool on Azeroth 32_48: add, keep level beside, level, slope, remove, magma takes lake cells, flat ocean, undo/redo, export read-back |
| `--sculpt-check <Data>` | Falloff shapes, sloped flatten lands on its plane, fill only, vertex selection moves with no cracks, even out, undo |
| `--newmap-check <Data>` | New map: Map.dbc + MapDifficulty rows, WDT/WDL, 2 x 2 flat tiles as one step; loads, exports, undo, redo, overlay rebuilt on reopen |
| `--impass-check <Data>` | Impassable brush sets MCNK 0x2, export writes exactly those flags, clear, undo |
| `--swap-check <Data>` | Texture swap: merge into an existing layer (shares add per texel), swap for a new texture, remove, export validates, undo |
| `--blueprint-check <Data> <map> <x> <y> <out.png>` | Save, load and draw a blueprint |
| `--model-check <Data> <map> <x> <y>` | Every model a tile places loads, boxes fit |
| `--dialogue-check <AC server dir>` | Dialogue tables against the world database: menu, submenu, texts, option, conditions and a bark as one batch; a non-gossip condition under the same id is never touched; undo leaves nothing |
| `--npc-check <AC server dir> [entry]` | NPC editor tables against the world database: copy a template with models, equipment and loot (one batch), edit, undo; leaves the database as it was |
| `--skin-check <Data> <out.png> <display id> [...]` | Character skins the editor composites vs Blizzard's bakes, side by side; fails above a mean difference of 12 |
| `--race-check <Data> [other Data...]` | Every race of each client in its own layout (1.12 or 3.3.5): identity, classes, choices per sex, textures missing; fails when a playable race has no character model |
| `--race-look <Data> <race> <sex> <out.png> [all]` | A race's character as the Races window previews it: the model loads, its textures exist, the composited skin is saved (`all`: every skin colour) |
| `--race-import-check <Data> <source Data> <source race> [target race]` | Imports a race of another client as the Races window does: every row of the new race, ids new and in the project's ranges, displays pointing at the source's model files, the same choices, save and reload, undo, redo, remove |
| `--appearance-check <Data> [display id]` | A new appearance (copy, bake dropped, hair changed) draws composited, exports both DBCs, and reads back intact |
| `--anim-check <Data> <model.m2> [...]` | Skeletons animate without flying apart; every animation (incl. `.anim` files) loads and Walk differs from Stand |
| `--catalog-check <Data> <out.png>` | Catalog build time, search, thumbnails |
| `--asset-check <Data> <other client> <map> <x> <y>` | Paste from another client, cracks, the asset closure |
| `--casc-to-mpq <install*product> <out dir> [map ...]` | Not a check: converts a CASC client's maps (all, or the named folders) and everything they use to 3.3.5a under `<out dir>\staging`, then packs `<out dir>\<name>.MPQ`; a rerun skips what is staged; `report.txt` lists what was lost |
| `--render <Data> <map> <x> <y> <out.png> [yaw pitch above fx fz]` | Draws a view to a PNG. WMO-only maps (any x y): fx fz are fractions of the WMO box, above is yards over its middle; with WWE_SERVER=<AC dir> it also checks every creature stands on a WMO floor |
| `--map-preview <Data> <map> <out.png>` | The Maps panel picture |

## Versions

| Command | Checks |
| --- | --- |
| `--ghost-check <Data> <other client> <map> <x> <y>` | Tile versions, ghost layers, copy from a ghost |
| `--compare-check <Data> <map> <x> <y>` | Edge layout, every `<map>_*` copy's difference, cycling cost, object-replacing paste, revert to client, undo |
| `--diff-check <Data> <base map> <other map>` | Whole-map scan, areas, rejection, rescan from saved results |
| `--diff-objects <Data> <base map> <other map> <zone id>` | Buildings each difference area carries, and why |
| `--tiles-check <Data> <base map> <other map>` | Adding tiles: heights, alpha, water, ids, far heights, undo, overlay, export, packing |

## Sources and output

| Command | Checks |
| --- | --- |
| `--sources-check <Data>` | Layer order, overrides, disabling, "players have it", notes, project.json |
| `--scan-check` | Scanning a mixed folder into layers, strays, rescan |
| `--mpq-check <folder> [keep.MPQ]` | Packs a folder like the patch and reads every file back |
| `--minimap-check <Data> <map> <x> <y> [out.png]` | The editor's minimap of a tile beside the client's (`<out>.client.png`) |
| `--export <project folder>` | What Ctrl+E does, without the window |

## Server

| Command | Checks |
| --- | --- |
| `--server-check <server folder> [soap account] [password]` | Reads worldserver.conf, queries the database, runs `server info` |
| `--spawn-check <server folder>` | Spawn and path adapters: place, move, group edits, undo, export, id ranges (leaves the database as it was) |
| `--unit-catalog-check <server folder> <Data>` | Every creature and gameobject template and its model |
| `--area-check <Data>` | AreaTable, WMOAreaTable and world map adapters on the real DBCs |
| `--triggers-check <Data>` | AreaTrigger.dbc and Map.dbc adapters on the real DBCs: layout, shapes, export changes only the edited fields |
| `--poi-check <Data> [<AC server dir>]` | AreaPOI.dbc adapter on the real DBC (row stats, export changes only the edited fields); with a server, game_tele and points_of_interest rows written, read back, undone |
| `--poi-read <Data> <map folder>` | The landmarks a client of any build (1.x, 2.x, 3.3.5) has on a map, as copies from that version pick them up |
| `--taxi-check <Data>` | TaxiNodes / TaxiPath / TaxiPathNode on the real DBCs (ids, mounts, path ends on their nodes), a planned path, add + move + export round trip |

## Tools

| Command | Does |
| --- | --- |
| `--find <Data> <word>` | Every listed file whose path contains the word, with its archive |
| `--extract <Data> <game path> <out file>` | One file as the client resolves it |
| `--where <Data> <map> <text>` | Every placement on the map whose model path contains the text |
| `--sql <server folder> "<query>"` | Prints a query's rows |

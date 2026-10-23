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
| `--ground-effect-check <Data>` | Ground effect tables and models; Blizzard's cell map vs the dominant layer on 8 stock tiles (layout); scatter count and placement; set effect, hide cells, paint, export, rotate a copy, undo |
| `--swap-check <Data>` | Texture swap: merge into an existing layer (shares add per texel), swap for a new texture, remove, export validates, undo |
| `--blueprint-check <Data> <map> <x> <y> <out.png>` | Save, load and draw a blueprint |
| `--model-check <Data> <map> <x> <y>` | Every model a tile places loads, boxes fit |
| `--wmo-group-check <Data> [every]` | Every WMO rebuilt from its own groups as they are comes out the same (tables and groups byte for byte but the doodad lists); every `every`-th one (default 25) pasted whole into the first WMO with portals, a quarter turn and a move away: portals kept (bar ones no group uses), vertex counts, each sampled face found by its moved collision tree, the result parses (3.3.5a client, every 1: 1985 the same, 1984 pasted, 4.5M faces, 0 problems, 21 s) |
| `--wmo-doodad-check <Data> [max]` | Every WMO of the client (or the first `max`) read as the interior editor reads it and written back unchanged: same sets, same doodads in order, same MODR per group, the rest of each group byte for byte, mesh parses the same (3.3.5a client: 1985 WMOs, 0 problems, 10 s) |
| `--building-check <Data> <wmo> <out.png>` | The Building editor's view of one WMO: alone on its own layer at the origin, not on the map's layer (boxes, group and doodad picks, pixels), groups picked by rays down, every shown doodad picked from above, a doodad moved through SetWmoDoodads shows moved, the framed view saved (GoldshireInn: 47/49 group rays, 301/303 doodads, 0 problems) |
| `--dialogue-check <AC server dir>` | Dialogue tables against the world database: menu, submenu, texts, option, conditions and a bark as one batch; a non-gossip condition under the same id is never touched; undo leaves nothing |
| `--race-server-check <AC server dir> [race] [donor race]` | The Races window's Server tab on the world database: a donor's (default Human) character-creation rows copied to a race (default 22, which must have none) for Warrior, Paladin and Shaman (from another race), Orcish as its language, one start set, then undone (leaves the database as it was); then the race masks: every quest and item of the donor takes the race, again changes nothing, revert restores |
| `--npc-check <AC server dir> [entry]` | NPC editor tables against the world database: copy a template with models, equipment and loot (one batch), edit, undo; leaves the database as it was |
| `--skin-check <Data> <out.png> <display id> [...]` | Character skins the editor composites vs Blizzard's bakes, side by side; fails above a mean difference of 12 |
| `--race-check <Data> [other Data...]` | Every race of each client in its own layout (1.12 or 3.3.5): identity, classes, choices per sex, textures missing; fails when a playable race has no character model |
| `--race-look <Data> <race> <sex> <out.png> [all]` | A race's character as the Races window previews it: the model loads, its textures exist, the composited skin is saved (`all`: every skin colour) |
| `--race-import-check <Data> <source Data> <source race> [target race]` | Imports a race of another client as the Races window does: every row of the new race, ids new and in the project's ranges, displays pointing at the source's model files, the same choices, save and reload, undo, redo, remove |
| `--race-export-check <Data> <source Data> <source race>` | Import with the race's files listed (clashing names renamed), export into a temp folder, then the client with the export over it: the race reads back with the same choices, its models load, its character composites, every other race is unchanged, CharSections sorted, the server's DBCs the same; the creator script keeps the client's and lists the race once, also when exported over an earlier export's script. `WWE_KEEP=1` keeps the temp export to look at |
| `--race-model-check <project dir>` | Reads a project's changes (writes nothing) and lists, per race, the model rows its import added and whether they still need filling for 3.3.5 (what the race editor's **Fill its model rows** offers) |
| `--creature-model-check <Data> [client Data] [path word] [out.png]` | The Creature models list of a client: models grouped with their skins, no character models; every 25th model loaded with its first skin (a file the source lacks read from the client); fails above 2% broken (file or skin texture missing; no mesh is fine). With a path word: each matching model in detail (where each file comes from, batches with blend mode, env map and opacity, how many show in Stand, skin textures) and nothing fails; with a PNG, up to 16 of them drawn side by side with their first skin |
| `--creature-import-check <Data> <source Data> <path word>` | Imports the first creature model of the source whose path holds the word, as the Creature models window does: rows in their ranges, effect scales, every file readable, no client file replaced by other bytes, a second import refused; exports it to a temp folder and loads it over the client with every skin texture; before export, the project layer of the client's chain loads it under its project path and its first display draws it with readable skins; removes it again |
| `--creature-skin-check <Data>` | A new skin on the client's murloc as the Creature models window makes one: a picture saved as a palettized BLP beside the model in a temp assets folder, a project display naming it; the look finds it through the assets-then-client chain, keeps its own scale, and the model draws with it |
| `--creature-file-check <Data> [model.m2 [names Data]]` | A model from loose files as Add a model file takes it (without a file: the client's murloc written out as loose files first; `names Data`, a newer client such as `<install>*<product>`, names a converted model's texture file ids): its row under the new folder, collision from its size, its files in a temp assets folder, the model loading from there over the client, its skin beside it; a 1.12 model is refused |
| `--race-edit-check <Data> [race]` | The Races window's edits on a client race (default Human): the middle value of every choice of both sexes removed, the rest moved down with their textures and no gap, the rows tied to it gone; classes kept, dropped and added with another race's outfits |
| `--appearance-check <Data> [display id]` | A new appearance (copy, bake dropped, hair changed) draws composited, exports both DBCs, and reads back intact |
| `--anim-check <Data> <model.m2> [...]` | Skeletons animate without flying apart; every animation (incl. `.anim` files) loads and Walk differs from Stand |
| `--catalog-check <Data> <out.png>` | Catalog build time, search, thumbnails |
| `--asset-check <Data> <other client> <map> <x> <y>` | Paste from another client, cracks, the asset closure. `WWE_WXL=1`: exports for a WXL client: kept files must be ones WXL reads, the FileDataID tables are read back, and every reference resolves through them alone |
| `--cdn-check <install*product> <game path>` | A file missing on disk misses at once and arrives from the CDN in the background; a file on disk is fetched from the CDN anyway (a range out of its archive) and must match the disk copy byte for byte. Close the editor first: they share the CDN cache |
| `--casc-to-mpq <install*product> <out dir> [map ...]` | Not a check: converts a CASC client's maps (all, or the named folders) and everything they use to 3.3.5a under `<out dir>\staging`, then packs `<out dir>\<name>.MPQ`; a rerun skips what is staged; `report.txt` lists what was lost |
| `--render <Data> <map> <x> <y> <out.png> [yaw pitch above fx fz]` | Draws a view to a PNG. WMO-only maps (any x y): fx fz are fractions of the WMO box, above is yards over its middle; with WWE_SERVER=<AC dir> it also checks every creature stands on a WMO floor; WWE_GROUNDFX=<0-4> adds the ground effects at that quality (Low..Ultra) |
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
| `--sources-check <Data>` | Layer order, overrides, disabling, "players have it", notes, project.json, a project's base leaving out its installed patch |
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
| `--area-check <Data>` | AreaTable, WMOAreaTable and world map adapters on the real DBCs; closing a project drops its rows |
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

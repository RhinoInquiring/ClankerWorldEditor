# Code layout

[Back to the README](../../README.md)

C++20, x64, Direct3D 11, Dear ImGui (docking) with ImGuizmo, StormLib, nlohmann/json, the MySQL client. Data and
format code never touches the GPU; the `App*` files are the UI.

## Data and formats

| File | Contents |
| --- | --- |
| `src/Mpq.*` | Layered game files (MPQ folders in client order, single MPQs, unpacked folders), the overlay for added tiles, folder scan, MPQ writer |
| `src/Formats.*` | ADT, WDT, WDL, BLP, DBC readers and writers: rewritten ADTs, MH2O, WDT tile flags, WDL heights, minimap index, structure check |
| `src/Models.*` | M2 and WMO loading: meshes, skeletons, animation, doodad sets, liquids, placement matrices |
| `src/Catalog.*` | Every file of the sources by kind, with folder trees and search |
| `src/Assets.*` | References of tiles, models and buildings; copying what players lack into the export |
| `src/Project.*` | The project folder and `project.json`: sources, id ranges, patch name |
| `src/Changes.*` | Change, Adapter, ChangeStore: undo/redo, batches, save/load |

## Adapters (edits)

| File | Contents |
| --- | --- |
| `src/Terrain.*` | Terrain: tile streaming, picking, sculpt, paint, holes, areas, copy/paste plans, water, objects, added tiles, export, crack check |
| `src/Blend.*` | The seam blend of a paste (smooth fill of the band, texture cross-fade) |
| `src/Blueprint.*` | Blueprints on disk |
| `src/Areas.*` | DBC row adapters: AreaTable, WMOAreaTable, WorldMapArea, WorldMapOverlay |
| `src/Spawns.*` | Creature and gameobject spawns in the world database, SQL export |
| `src/Tables.*` | Rows-by-key adapter: waypoint_data, creature_addon, areatrigger, areatrigger_teleport, instance_template, points_of_interest, game_tele, creature_template, creature_template_model, creature_equip_template |
| `src/Paths.*` | A creature's waypoint path as one change |
| `src/Triggers.*` | Area triggers (shape, inside test, ray hit), teleports, AreaTrigger.dbc and Map.dbc corpse entrance adapters |
| `src/Pois.*` | Points of interest (landmark, gossip point, teleport) and their rows; AreaPOI.dbc adapter |
| `src/Flights.*` | Taxi node, path and point rows; TaxiNodes / TaxiPath / TaxiPathNode adapters; planning a new path |

## Versions

| File | Contents |
| --- | --- |
| `src/Ghosts.*` | Ghost sources and layers, tile versions, background streaming, area and chunk comparison |
| `src/Differences.*` | Whole-map scan on a worker thread, areas of edited chunks, saved results and verdicts |

## Rendering

| File | Contents |
| --- | --- |
| `src/Renderer.*` | Terrain (blended layers, LOD, far terrain), water, overlay lines, solids, textures |
| `src/ModelRenderer.*` | M2 and WMO instances, picking, thumbnails, attachments |
| `src/Looks.*` | Display ids to models: creature skins, humanoid NPCs with equipment, gameobjects |
| `src/Minimap.*` | Top-down orthographic render (minimaps, world maps, thumbnails) and the minimap look |
| `src/Loader.*` | Background preparation of tiles, textures and meshes |

## Server

| File | Contents |
| --- | --- |
| `src/Server.*` | Server profiles, Windows credential store, MySQL link, SOAP |

## UI

| File | Contents |
| --- | --- |
| `src/App.*` | Main window: panels, tool groups, viewport input, camera, commands, export, copy/paste, ghosts |
| `src/AppCompare.cpp` | Compare: cycling versions of a selection, the numbers |
| `src/AppDifferences.cpp` | Catalog > Differences: scan, cards, review, new tiles |
| `src/AppSources.cpp` | Sources window |
| `src/AppPopulate.cpp` | Creatures and Gameobjects tools, unit catalog |
| `src/AppNpc.cpp` | NPC viewer: template list, posed preview, animations, skins, equipment, geosets, textures |
| `src/AppPaths.cpp` | Path editing |
| `src/AppZones.cpp` | Zones tool, buildings' room names |
| `src/AppTransform.cpp` | Shared move / rotate / scale: each tool's selection as a `Transformable`, the gizmo, keys, Alt+click move, transform bar |
| `src/AppTriggers.cpp` | Triggers tool: triggers, teleports, entrances, their checks |
| `src/AppPois.cpp` | POIs tool: landmarks, gossip points, teleports, their checks |
| `src/AppFlights.cpp` | Flights tool: nodes, paths, points, flight masters, their checks |
| `src/AppWorldMap.cpp` | World map drawing |
| `src/AppInspect.cpp` | Inspector |
| `src/AppServer.cpp` | Server setup, Server panel, Problems |
| `src/main.cpp` | Window and device, frame loop, the [command-line checks](checks.md) |

## Adding a tool that moves things

Do not write a gizmo, nudge keys or an Apply button for it. Return its selection from `App::ActiveTransform`
(`AppTransform.cpp`) as a `Transformable`: the handles' frame, what it allows (rotate free / vertical only, scale
none / uniform / per axis, with a `limits` note), and `begin` / `preview(delta)` / `commit` / `cancel` / `ground` /
`remove`. Add the tool to `TransformTool()`, call `DrawTransformBar` at the top of its selection panel, and make its
fields preview live and save when let go (no Apply). It then gets the same handles, snapping, keys, Alt+click move,
caption and undo as every other tool. `TransformSelfTest` (in `--selftest`) covers the shared math.

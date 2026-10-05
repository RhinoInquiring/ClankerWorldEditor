# Change model (M1)

Every edit is a **change** recorded in the project. Undo, redo, saving, export and (later) server
apply and publish all work on the same list.

## Pieces

| Piece | Role |
| --- | --- |
| `Change` | One undoable edit: id, domain, target, label, author, time, and a domain payload with both the before and the after state |
| `Adapter` | One per domain (terrain, objects, spawns, DBC, ...). Applies and reverts its own changes and exports its files |
| `ChangeStore` | Undo and redo stacks, the history list, dirty tracking, save/load to the project folder |
| `Project` | The project folder: `project.json`, `changes/`, `out/` |

A tool edits live while the user drags, then hands one finished `Change` to the store. The store
never re-applies a change it was just given; it only calls the adapter for undo, redo and loading.

## Adapter operations

Implemented now: `Apply` (the after state), `Revert` (the before state), `Export` (files for the client).
Later, per the spec: `ApplyServer`, `Diff` against the base data, publish export.

## Project folder

```
<project>/
  project.json        name, Data folder, client folder, author, next change id
  changes/
    000001.json       one file per change, in order; text so git diffs and merges them
  out/
    client/           exported client files (ADT, ...), same layout as the MPQs
```

Opening a project replays `changes/` in order through the adapters. A change keeps both states, so
reverting never needs the base files.

## Terrain heights (first adapter)

- Payload: map, tile x/y, and per touched vertex `[chunk, vertex, before, after]` (MCVT values).
- One brush stroke (mouse down to mouse up) is one change.
- Export patches the MCVT floats of the original ADT bytes; nothing else in the file moves.

Known gap: MCNR normals are not rewritten yet, so the client lights edited terrain with the old normals.

## Terrain water

- Terrain changes may carry `"liquids": [[tileX, tileY, chunk, before, after], ...]`, each side the chunk's whole
  LiquidState: an array of MH2O instances (`type`, `format`, `x`, `y`, `w`, `h`, absolute `heights`, base64 `exists`
  bytes and `extra` per-vertex data, `fishable` / `deep` masks); `[]` = no water. Pastes write one entry per pasted
  chunk whose water changes.
- Export rebuilds the tile's MH2O from its liquids (inserting one when the file had none); the client then ignores
  MCLQ for that tile. Like areas, the server sees water through its extracted maps: re-run the extractor.

## Added tiles

- Terrain changes may carry `"tiles": [[tileX, tileY, file, lastUid, minimap], ...]`: whole tiles the map lacked,
  taken from another version. `file` is the converted copy in `<project>/tiles/`; `lastUid` the highest object id
  given to it (NextUniqueId continues above it); `minimap` the other version's minimap image kept beside it (empty
  when it had none).
- Apply copies the file into `<project>/overlay/World/Maps/<map>/`, marks the tile in overlay copies of the map's
  WDT and gives it far heights in the WDL (computed from the tile); revert removes the file and clears both. The archive reader (MpqChain::SetOverlay) serves overlay files above every MPQ for
  `World\Maps\` paths. The overlay is rebuilt from the applied changes when the project opens, so it never outlives
  an unsaved session.
- Export writes each added tile (with any later edits), the map's WDT and WDL, the minimap images (as
  `textures/Minimap/wwe_<map>_<x>_<y>.blp`) and the client's `md5translate.trs` with lines naming them.

## Sources (project.json)

- `"base": {"name", "layers": [{"kind": "mpqfolder"|"mpq"|"folder", "path", "enabled", "installed"}]}`: the files
  the project edits and exports against, later layers above earlier ones. `"compare": [...]`, same shape: versions
  to compare against. `installed` = players' clients have the layer; export copies referenced files no installed
  layer has (MpqChain::HasInstalled). `from` (optional) = the folder a scan found the layer in (Rescan updates those).
- Older projects (`clientDir` + `"sources": [{name, dataDir}]`) load as base = the client, compare = those sources;
  the next save writes the new shape. `clientDir` stays: play test and install target.

## Output

- `out/client/`: client files of the last export, rebuilt from empty each time; `out/<patchName>` (project.json,
  default `patch-enUS-Z.MPQ`) is that folder packed. `out/server/`: SQL and server DBCs.

## Derived files

- `<project>/minimaps/`: pictures of edited tiles drawn by the editor at export, `index.json` mapping each tile to
  the hash of its edits (TerrainAdapter::EditHashes); a tile is drawn again when its hash changes.
- `<project>/differences/`: per-tile scan results and verdicts (see Differences), keyed by the same hashes.
- Neither is part of the change history; both can be deleted and are rebuilt.

## Zones (AreaTable + chunk area ids)

- Terrain changes may carry `"areas": [[tileX, tileY, chunk, before, after], ...]`; export writes the MCNK header
  area id (+0x34). One paint stroke is one change. The server reads areas from its extracted `.map` files, so
  painted tiles need the map extractor run again.
- `dbc.AreaTable` changes hold `{id, before, after}`, rows as every field by mod-dbc-patch schema name (null = no
  row). New ids come from the project's `area.id` range; a new AreaBit is the lowest unused one below 4096.
- Export writes `out/client/DBFilesClient/AreaTable.dbc` and `out/server/dbc/AreaTable.dbc` (client rows + the
  project's) and `out/dbc/AreaTable.json` (mod-dbc-patch add/modify). The client reads DBCs only at start.
- `dbc.WMOAreaTable` changes have the same shape (`DbcTable` serves both). Rows are keyed by (WMOID from the root's
  MOHD, the placement's MODF name set, the group's MOGP id at +0x38); AzerothCore looks groups up one by one (never
  the -1 row) and reads the name set as int8, so "whole building" writes a row per group and name sets stop at 127.
  New ids come from `wmoarea.id`. Giving a placement its own name set is an object edit; the server needs its vmaps
  extracted again for that, while row changes only need a restart.
- World map: `dbc.WorldMapArea` (a zone's picture and its world rectangle; editor x = zero - LocLeft side, z = zero -
  LocTop side, view 1002 x 668 of a 1024 x 768 canvas in 4 x 3 tiles) and `dbc.WorldMapOverlay` (pieces shown once
  AreaID[0..3] is explored). Pictures are rendered top-down from the loaded terrain into `<project>/assets/` (not
  changes: rendering needs the tiles loaded) and export copies `assets/` into `out/client/`. Overlay tiles follow
  WorldMapFrame.lua: 256 px, the last column / row a power of two of at least 16.

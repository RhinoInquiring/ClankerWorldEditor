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

## Zones (AreaTable + chunk area ids)

- Terrain changes may carry `"areas": [[tileX, tileY, chunk, before, after], ...]`; export writes the MCNK header
  area id (+0x34). One paint stroke is one change. The server reads areas from its extracted `.map` files, so
  painted tiles need the map extractor run again.
- `dbc.AreaTable` changes hold `{id, before, after}`, rows as every field by mod-dbc-patch schema name (null = no
  row). New ids come from the project's `area.id` range; a new AreaBit is the lowest unused one below 4096.
- Export writes `out/client/DBFilesClient/AreaTable.dbc` and `out/server/dbc/AreaTable.dbc` (client rows + the
  project's) and `out/dbc/AreaTable.json` (mod-dbc-patch add/modify). The client reads DBCs only at start.

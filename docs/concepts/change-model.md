# Change model

[Back to the README](../../README.md)

Every edit is a **change** recorded in the project. Undo, redo, saving, opening, export and the server link all work
on the same list, and the game's own files are never modified.

## Pieces

| Piece | Role |
| --- | --- |
| `Change` | One undoable edit: id, domain, target, label, author, time, and a payload holding both the before and the after state |
| `Adapter` | One per domain (terrain, spawns, tables, DBCs). Applies and reverts its own changes, replays them on load, exports its files |
| `ChangeStore` | The history, undo and redo, batches (several domains as one undo step), dirty tracking, save/load |
| `Project` | The project folder and `project.json` |

A tool edits live while you drag, then hands one finished change to the store. The store never re-applies a change it
was just given; it calls the adapter for undo, redo and loading. Because a change keeps both states, reverting never
needs the original files.

## Project folder

```
<project>/
  project.json        name, client folder, author, server profile name, sources, id ranges, patch name
  changes/            one JSON file per change, in order: plain text that git diffs and merges
  blueprints/         saved blueprints
  assets/             generated pictures (world maps), copied into every export
  tiles/              added tiles (converted) and their minimap pictures
  overlay/            added tiles and the WDT/WDL listing them, served above the game files (rebuilt on open)
  minimaps/           minimap pictures of edited tiles (derived)
  differences/        scan results and verdicts (derived)
  out/client/         the last export's client files
  out/server/         spawn and path SQL, server DBCs
  out/dbc/            DBC changes as mod-dbc-patch JSON
  out/<patchName>     the patch MPQ
```

Opening a project replays `changes/` in order through the adapters (tiles replay their edits as they stream in).
Derived folders can be deleted; they are rebuilt.

## IDs

New rows (spawns, areas, world map rows) take ids from the project's ranges (`project.json` `idRanges`, edited in
**File > Project settings**): the lowest free one, never reused. An id is stored in the change that creates the row and
never recomputed, so exports stay stable and other projects can claim other ranges.

## Domains

### terrain.heights

One change may carry, per map:

| Field | Entries | Meaning |
| --- | --- | --- |
| `edits` | `[tileX, tileY, chunk, vertex, before, after]` | Heights (MCVT) |
| `layers` | `[tileX, tileY, chunk, before, after]` | A chunk's texture layers, flags, ground effects, alpha (base64) |
| `holes` | `[tileX, tileY, chunk, before, after]` | Hole mask |
| `areas` | `[tileX, tileY, chunk, before, after]` | Area id |
| `liquids` | `[tileX, tileY, chunk, before, after]` | The chunk's whole water state (below); `[]` = none |
| `objects` | `[tileX, tileY, "m2"/"wmo", before, after]` | A placement added, moved or deleted (null side) |
| `tiles` | `[tileX, tileY, file, lastUid, minimap]` | A whole tile added from another version |

Export: heights are patched in place and normals recomputed for reshaped chunks and their neighbours (across tile
borders too); a tile with texture, object or water changes is rebuilt chunk by chunk in the client's own order; every
written tile passes a structure check first.

**Water** (`liquids`): each side is an array of MH2O instances (`type`, `format`, `x`, `y`, `w`, `h`, absolute
`heights`, base64 `exists` and `extra` per-vertex data, `fishable` / `deep` masks). Export rebuilds the tile's MH2O
(inserting one when the file had none); the client then ignores old-style MCLQ water in that tile.

**Added tiles** (`tiles`): `file` is the converted copy in `<project>/tiles/`, `lastUid` the highest object id it was
given (new ids continue above it), `minimap` the other version's minimap picture kept beside it (empty if none).
Applying copies the file into `<project>/overlay/World/Maps/<map>/` and marks the tile in overlay copies of the map's
WDT and WDL (far heights computed from the tile); reverting removes them. The archive reader serves overlay files above
every source for `World\Maps\` paths. Export writes the tiles, the WDT, the WDL, and their minimaps.

### world.creature, world.gameobject

`{rows: [{guid, before, after}]}`, each side the whole row as column to text (null = no row). Applied to the world
database as they happen when a server is linked; exported as SQL with revert scripts.

### world.waypoint_data, world.creature_addon

Rows of one key (path id, guid) before and after, written as delete-then-insert. A path save is a batch: points,
addon and the creature's movement type in one undo step.

### dbc.AreaTable, dbc.WMOAreaTable, dbc.WorldMapArea, dbc.WorldMapOverlay

`{id, before, after}`, rows as every field by mod-dbc-patch schema name (null = no row). Export writes the client's
table plus the project's rows to `out/client/DBFilesClient` and `out/server/dbc`, and the changes as JSON to `out/dbc`.
The client and worldserver read DBCs at start.

- WMOAreaTable rows are keyed by (building id from its root file, the placement's name set, the room's group id).
  AzerothCore looks rooms up one by one and reads the name set as a signed byte, so naming a whole building writes a
  row per room and name sets stop at 127.
- WorldMapArea gives a zone picture's world rectangle; WorldMapOverlay the pieces shown once areas are explored.
  Pictures are rendered into `<project>/assets/` (not changes: rendering needs the tiles loaded).

## Sources

`project.json` `base` and `compare`: see [Sources](sources.md). Older projects (`clientDir` plus `sources`) load as
base = the client, compare = those sources; the next save writes the new shape. `clientDir` stays as the play-test
and install target.

## Output

`out/client/` is rebuilt from empty on every export; `out/<patchName>` (default `patch-enUS-Z.MPQ`) is that folder
packed. See [Export and the patch MPQ](../output/export-and-patch.md).

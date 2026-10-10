# Export and the patch MPQ

[Back to the README](../../README.md) · File menu

## Commands

| Command | Key | Does |
| --- | --- | --- |
| Export client files | Ctrl+E | Writes the client files to `<project>/out/client`, the server files to `<project>/out/server` |
| Build patch MPQ | Ctrl+Shift+E | Exports, then packs `out/client` into `<project>/out/patch-enUS-Z.MPQ` |
| Build patch MPQ and install into client and server | | Also copies it into the client's `Data\enUS\`, and, with a server folder set (File > Server setup), copies `out/server/dbc` into the server's `Data\dbc` and writes the project's rows into the connected world database. The server's own DBCs are kept once in `<project>/server-build/original-dbc` and put back when the project no longer changes that table. Restart the worldserver afterwards. |
| Play test | F5 | Exports into the WXL client's overlay for a quick relog test |

Every export starts from an empty `out/client`, so an undone edit never leaves a file behind.

## What goes in

**Client** (`out/client`, and the patch):
- Every edited tile (heights with new normals, textures, holes, water, objects, areas), plus the neighbouring tiles
  whose edge lighting changed.
- [Added tiles](../versions/differences.md#new-terrain), with the map's WDT (which tiles exist) and WDL (far
  terrain).
- [Minimaps](minimaps-and-far-terrain.md) of edited tiles and the minimap index (`md5translate.trs`).
- DBCs the project changed (AreaTable, WMOAreaTable, WorldMapArea, WorldMapOverlay), and generated pictures (world
  maps) from `<project>/assets/`.
- Textures and models the edits use that players do not have: files from compare sources, or from base layers
  without **Players have it** ([Sources](../concepts/sources.md)). Each file's references are followed, so a model
  comes with its skins and textures.

**Server** (`out/server`): spawn and path SQL with revert scripts, and server copies of the changed DBCs.
`out/dbc` holds the DBC changes as mod-dbc-patch JSON.

Tiles that fail the structure check are not written and are listed in **Problems**, as are ground effects the client
lacks (dropped, since they crash it), missing assets, and open edges between tiles.

## The patch MPQ

One archive (zlib-compressed, with its file list) that any 3.3.5 client loads; no extension needed. Its name is
`patchName` in `project.json`, default `patch-enUS-Z.MPQ`: it loads after every `patch-X.MPQ`, including map module
patches up to `patch-Z`, so the project's files win. Archives in the client that would load after it are listed as
warnings. Install copies it into `Data\enUS\` (`Data\` for a name without a locale); close the client first, as a
running client holds its archives open.

## Play test

F5 mirrors `out/client` into the WXL extension's overlay (`<client>/Extensions/wxl-editor-poc/overlay`), removing files
no longer exported; relog in the client to see the changes. It needs the WXL client the project was made with.

## The server side

The patch is client-only. For the server: apply `out/server` SQL (spawns are already in the database when the server
is linked), copy the changed DBCs, restart the worldserver, and after terrain, water or area edits extract its maps
(and vmaps for building changes) again from the patched client.

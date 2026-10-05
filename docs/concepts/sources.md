# Sources

[Back to the README](../../README.md) · View > Sources

A source is where game files come from: a stack of **layers**, later layers winning where they hold the same file,
as later patches do.

## Layer kinds

| Kind | What it is |
| --- | --- |
| MPQ folder | A client's `Data` folder (or the client folder holding it), a module bundle, any folder of MPQs. Its archives are read in the client's own load order, locale folders included |
| MPQ file | One archive, e.g. a single module patch |
| Unpacked folder | Files laid out by game path (`World\Maps\...`, `DBFilesClient\...`, `Textures\...`), indexed when the source opens. A folder with none of those at its top gets a note: probably picked one level too high or low |

**+ Scan folder** turns a folder holding a mix into layers: every `.mpq` inside, at any depth, becomes its own
layer in the client's load order; every folder that is the top of unpacked game files becomes an unpacked layer above
them (a mod shipping an archive and loose files gives both); a client folder (holding `Data`) is scanned into rather
than taken as one tree. Files outside any game tree are listed as left out. **Rescan** (one per scanned folder) keeps
the layers still there in place with their settings, drops the ones gone, and puts new ones on top.

## Roles

- **Project base**: what you edit, export against and see. It starts as your client folder and can stack more on top
  (a module bundle, an unpacked mod) without installing anything into the client.
- **Compare sources**: other versions, for [ghost layers](../versions/ghost-layers.md),
  [compare](../versions/compare.md) and [differences](../versions/differences.md). Assets missing from the base are
  read from them too.

## Players have it

Each layer has **Players have it**. On: players' clients already have these files (a client's `Data`, installed
patches), so the [patch MPQ](../output/export-and-patch.md) does not carry them. Off: new art the patch must carry;
files the edits use from such a layer go into the patch. Unpacked folders added to the base start with it off.

## Editing

New layers go on top. Arrows reorder, the checkbox disables, **x** removes; each layer shows what it gave (archives,
files, or a note). **Apply** saves the sources in `project.json`: a base change reopens the project on the new files,
compare changes reattach. Projects made before sources existed convert on open: the client becomes the base, its other
clients compare sources.

## In project.json

```json
"base": { "name": "Client", "layers": [
  { "kind": "mpqfolder", "path": "D:/WoW 3.3.5a/Data", "enabled": true, "installed": true },
  { "kind": "folder", "path": "D:/Mods/MyArt", "enabled": true, "installed": false }
] },
"compare": [ { "name": "Turtle", "layers": [ { "kind": "mpqfolder", "path": "D:/TurtleWoW" } ] } ]
```

`kind` is `mpqfolder`, `mpq` or `folder`; `from` (optional) is the folder a scan found the layer in.

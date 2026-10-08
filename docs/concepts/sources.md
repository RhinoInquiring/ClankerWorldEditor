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

## Newer clients (CASC)

**+ CASC** adds a newer client's storage (an install folder and its product, e.g. `wow_classic_beta`). Its files are
read as 3.3.5a ones: split tiles merged, newer models and WMOs converted. Files missing from the install come from
Blizzard's CDN in the background, so a first look is slow and areas fill in as files arrive. For comparing a whole
client, convert it once instead and add the MPQ as an MPQ file layer:

```
wow-world-editor.exe --casc-to-mpq "D:\World of Warcraft*wow_classic_beta" "D:\MPQs\Forever Beta"
```

It writes every map (or the map folders named after the out dir) with every model and texture they use, already in
3.3.5a form, packs them into `Forever Beta.MPQ` (MPQ format 4: the editor reads it, the 3.3.5a client does not), and
lists in `report.txt` what was converted with a loss (particles, newer shaders). Close the editor while it runs: both
use the same CDN cache. A rerun after an interruption skips what is already written; a new client build needs the
`staging` folder deleted first.

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

## Labels

Each layer has a **Label**: your name for it ("Client", "Epoch zones", ...). The Maps window shows it in brackets
beside every map whose WDT that layer supplies (the topmost base layer holding the map wins); maps the project made
with File > New map show **[Project]**. The map search matches labels too. Labels are saved with the sources and change
nothing else.

## Versions of a map

The project's maps are the client's: its Map.dbc and each map's own files (`World\Maps\<map>\`) come from the lowest
layer that has them, the plain client, never a pack stacked over it. Every other file still comes from the topmost
layer, as patches work. Each source's own Map.dbc is read to learn what it offers: maps whose ids the client lacks are
added to the list, tagged with that source.

When several sources have a map, the Maps window lists each version under it: every source (a base layer, or the
layers sharing its label) whose Map.dbc lists the map's id and that holds its WDT, marked **under**, and every compare
source with the same id, marked **compare**. A source's copy may use another folder; the row then names it. A source
without a Map.dbc counts when it holds the map's WDT. The map's own row is the version you edit, the client's.
Selecting another version shows its tile grid and far heights as that source has them, and **Show as ghost over the
open map** lays it over your map (a source's version is read from its own layer alone).

## Editing

New layers go on top. Arrows reorder, the checkbox disables, **x** removes; each layer shows what it gave (archives,
files, or a note). **Apply** saves the sources in `project.json`: a base change reopens the project on the new files,
compare changes reattach. Projects made before sources existed convert on open: the client becomes the base, its other
clients compare sources.

## In project.json

```json
"base": { "name": "Client", "layers": [
  { "kind": "mpqfolder", "path": "D:/WoW 3.3.5a/Data", "enabled": true, "installed": true, "label": "Client" },
  { "kind": "folder", "path": "D:/Mods/MyArt", "enabled": true, "installed": false }
] },
"compare": [ { "name": "Turtle", "layers": [ { "kind": "mpqfolder", "path": "D:/TurtleWoW" } ] } ]
```

`kind` is `mpqfolder`, `mpq` or `folder`; `from` (optional) is the folder a scan found the layer in; `label` (optional) is its name in the Maps window.

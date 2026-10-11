# Creature models

[Back to the README](../../README.md) · View > Creature models

The creature models of every source: the project's client and each compare source (View > Sources). It is where a
project's creatures will take their models from; it needs no database, since models and their skins are client
tables (CreatureModelData, CreatureDisplayInfo).

Its three panels, **Model list**, **Model preview** and **Model details**, can be moved like any other panel.
View > Reset panel layout puts them back.

| Part | What it shows |
| --- | --- |
| List | The source and its models (file name, number of skins, CreatureModelData id), filtered by path. In another source, **new** marks models the project's client does not have; **Only new** lists just those |
| Preview | The model with the picked skin, as it spawns. Left drag turns, right drag pans, the wheel zooms. Below it every animation (names from the source's `AnimationData.dbc`), play / pause, frame steps, speed and a time slider |
| Details | The model's path and id, its skins (one CreatureDisplayInfo row each: display id, skin textures, scale; click one to show it, or **Bare model** for none), its textures (a missing skin texture is flagged) and the number of animations |

- Character models (`Character\...`) and humanoid NPC displays are left out: those are races and appearances
  ([Races](races.md), the [NPC viewer](npc-viewer.md)'s Appearance tab).
- A 1.12 client's tables read the same way as 3.3.5's (the columns used sit in the same places); its models need to be
  converted to 3.3.5 in that source (wow-upport) to show.
- A source that holds only a client's patches (such as an up-ported Turtle folder) lacks the files it did not
  change: each file it lacks is read from the project's client under the same path, and the details say so.
- A newer client (CASC) has DB2 tables this cannot read yet: its list is every `Creature\*.m2` file, without skins.
- Some models have nothing to draw (particles only, invisible markers); the preview says so.
- A model file in neither the source nor the project's client cannot be shown; the details name the file. Either the source's client never shipped it (Turtle's tables name skardyn, tamamo and a few others it does not ship), or the source is a converted copy missing the archive that holds it (convert that archive into it too).

## Importing a model

In another source (not the project's client), **Import into the project** in the details takes the picked model with every
skin it has, as one undo step:

- a new CreatureModelData row in the project's `creaturemodeldata.id` range and one CreatureDisplayInfo row per skin in
  `creaturedisplayinfo.id` (File > Project settings; the defaults are 9000-9999 and 90000-90999);
- a 1.12 row gets the effect scales it lacks (from the client's row of that model, else 1), so spell effects show on it;
- the list of files export copies from the source: the model, its `.skin` and `.anim` files, its textures and its skin
  textures. A file the project's client has with the same bytes is not copied (players have it). When the client has
  any of them with other bytes, the whole model moves to `Creature\<source>\...` (its skins sit beside it), so the
  client's own model and the creatures using it stay as they are.

The list marks imported models **in project**; their details show the model and display ids and offer **Remove from
the project** (the rows and the file list go; undo brings them back). Export (File > Export) copies the files into the
patch and writes the rows with CreatureModelData and CreatureDisplayInfo; the source must still be attached then.
Sound, blood and footprint ids are kept as the source has them. Importing from a newer client (CASC) needs its DB2
tables and is not possible yet.

## New skins on a client model

In the project's client, the details of a model offer **New skin**: a display of the project's own on that model
(`creaturedisplayinfo.id` range), copied from the skin picked above it (or blank with none picked). The skins table
marks the project's skins **project**; with one picked:

- **Skin 1-3**: the texture of each skin slot the model draws, chosen from the BLPs in the model's folder, or
  **Picture...**: a PNG, JPEG, BMP or BLP whose sides are powers of two (64 ... 2048), saved as
  `assets\<model folder>\<model>Skin<display>.blp` (palettized). The preview shows it at once; export copies the
  assets folder into the patch.
- **Scale**: the display's size (the preview follows the slider; let go to save).
- **Delete skin**.

Each edit is one undo step. A client skin is never changed: **New skin** makes an editable copy.

## Adding a model from files

**Add a model file...** (above the list) takes an `.m2` from disk, for example an export from a modelling tool or another
client's unpacked files, and asks for its folder in the game (default `Creature\<name>\`). It copies into the
project's assets folder, under that game folder:

- the model; a newer one (chunked `MD21`, or past version 264) is converted to 3.3.5a (it names textures by file id:
  attach that newer client as a source so the editor knows their names). A 1.12 model (version 256) is
  refused: convert it with wow-upport first;
- its `<name>NN.skin` and `<name>NNNN-NN.anim` files (converted with it) and every `.blp` in its folder;
- a fixed texture the model names that the client lacks but the folder has (same file name) is renamed to the new folder
  inside the model. One that neither has is reported in the log.

It then must load from the assets over the client, or nothing is added. The project gets a CreatureModelData row
(collision size from the model's box, effect scales 1) and one skin (the folder's other textures in the slots the model
draws), as one undo step. The project's models are listed first in the project's client, marked **project**; their
details say where they came from and offer **Remove from the project** (a model from files keeps its files in assets:
delete them there to leave them out of the patch). New skins work on them as on any client model.

## Using the library

The editor reads the project's own files as the patch will hold them, before export: everything in the assets folder,
and every file an import copies (read from its source, under its project path, with the texture names export
renames). So the NPC viewer, the world view and spawns draw imported models, models from files and new skins as soon
as they are made. An import whose source is not attached reads nothing until it is again.

In the [NPC viewer](npc-viewer.md)'s **Models & gear** tab, **From library...** lists the project's own skins (display,
model path, first skin texture, filtered by path); picking one adds it to the creature's models (`creature_template_model`)
and shows it. Apply writes it with the creature's other edits; the worldserver needs a restart for models, and the
display needs the exported patch and tables in the client and server.

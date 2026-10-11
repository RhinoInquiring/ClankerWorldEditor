# Known bugs

[Back to the README](../../README.md)

Bugs that are known and left for later, with what shows them.

## Replacing or reverting a whole tile can leave a few objects wrong

**Where:** pasting another version's tile in place with its objects (Versions, Compare), and Revert selection to
client.

**What happens:** objects whose bounds only graze the edge of the selection can be missed. Replacing Azeroth 32_48
with Ascension's version (`Azeroth_Asc`) leaves 3 of its objects missing and 2 of the map's objects behind; reverting it
restores 763 of the client's 766 objects. The terrain itself is right. Epoch's version of the same tile has no such
objects and comes out exact.

**Shown by:** `--compare-check <Data> Azeroth 32 48` on a client that has the Ascension maps (2 problems).

**Workaround:** delete or place the few objects on the tile's border by hand.

**Likely cause:** objects straddling the tile border are counted on one side of the comparison but not the other
(the added copies are clipped to loaded tiles; see the note in `src/ChecksVersions.cpp`).

## Turtle's Raven shows wrong in Creature models

**Where:** View > Creature models, source Turtle Client (the up-ported Turtle 1.18.1 client), `Creature\Raven\Raven.mdx`.

**What happens:** the model loads but does not look right in the preview. Not looked into yet: the file comes from
Turtle's converted archives (wow-upport), so the cause may be the conversion or the editor's renderer.

**Shown by:** `--creature-model-check "<MPQs-upported>\Turtle Client" "<3.3.5 client>" raven <out.png>` draws it next to
its batch list.

**Workaround:** none yet.

## An imported creature model does not show with `.morph`

**Where:** Creature models window, a model imported from another client (seen with Turtle's `Creature\Turtle\TurtleChristmas`,
model 9002, display 90000), exported, then `.morph 90000` in game.

**What happens:** nothing changes. Not looked into yet. Suspects, in order: the client did not load the exported patch or
its CreatureDisplayInfo.dbc (patch not installed, or the client not restarted); the worldserver lacks the row (`out/server/dbc`
not copied, or not restarted); the model itself (moved to `Creature\TurtleClient\...`, its texture names rewritten).

**Shown by:** `--creature-import-check` passes for the same kind of model (files, rows, the exported model loading), so the
file side looks right outside the client.

**Workaround:** none yet.

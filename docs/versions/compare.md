# Compare

[Back to the README](../../README.md) · Select or Copy tool, then **]**

Compare shows a selected area, in place, as each other version of it in turn, with what pasting each one would take.
No layers or solo needed.

## Use

Select chunks, then press **]** (or **Compare versions** in the Versions window). The versions are every ghost layer
plus every map whose folder starts with this map's name (`Azeroth_Asc`, `Azeroth_Epoch`, `Azeroth_Turtle`, ... over
`Azeroth`); those copies load hidden, for the selected tiles only, and are dropped when the compare ends.

| Input | Action |
| --- | --- |
| [ / ] | Previous / next version (versions identical here, or not covering the area, are skipped) |
| Shift / Ctrl + click or drag | Add / remove chunks; preview and numbers follow |
| Enter | Paste the shown version (one undo step) and end the compare |
| Esc | End it; the map comes back |

While you flick through versions the preview is a hard paste (a few milliseconds); the blended result follows once
you pause for a quarter second. Each version's copy and plan are kept, so going back to one is instant; the models a
version would add load in the background.

## The numbers

Per version (the table in the Versions window and the blue line in the viewport):

| Column | Meaning |
| --- | --- |
| Chunks | Chunks that differ (heights over 0.5 yd, textures, holes or water) / chunks the version has |
| Height | Mean / largest height difference |
| Edge step | Largest step on the selection's outer edge: what the blend band must absorb (orange above 2 yd) |
| Water | Chunks whose water differs (fishing and fatigue masks aside: map editors rewrite those) |
| Objects + / - | Objects only the version has (the paste adds them) / only your map has (the paste removes them) |
| Assets | Textures and models only another client has (copied on export) or no source has (missing) |

## Objects

A doodad comes with the chunk its origin stands on. A building comes with every area its bounds reach, so a cave
whose origin lies off to one side still comes with the area it runs through. A pasted version leaves the area's objects
as that version has them: placements both share (same model, position to the yard, turn to the degree, scale) stay
untouched, your map's others are removed and the version's others added, all in the same undo step. Creatures are
server spawns and are not touched. The paste waits for the tile holding each carried
object's origin to load.

## See also

- [Copy and paste](../terrain/copy-paste.md): what a paste does, blending
- [Differences](differences.md): find the areas worth comparing across a whole map

# Differences

[Back to the README](../../README.md) · Catalog > Differences (Terrain group, F1)

Differences scans a whole other version of the open map and turns every edit it makes into a card you can review,
approve (paste into your map) or reject.

## Scan

**Find differences with another version...** in the Versions window opens the tab. Pick the other version (one of
the map's `<map>_*` copies, or the same map in a compare source) and press **Scan**. Every tile of that version is
read and compared with your map *as the project has it*, on a background thread; a progress bar (tiles, time left)
shows in the tab and in the status bar, cards appear as areas are found, and you keep working meanwhile. A whole
continent takes about 10 seconds; a **Rescan** about 6, reusing every tile whose files and project edits have not
changed.

## Cards

Touching edited chunks form an area (larger than one tile's worth: one card per tile). Each card shows a picture of
the other version (areas over 9 tiles get none), the zone name, the chunk count, and what changed: heights, textures,
holes, water, new objects, new terrain. Sort by size, height change, new objects or map position; search by area
name; **Rejected** shows the rejected ones; **Only new terrain** shows areas on tiles your map lacks.

## Review

| Input | Action |
| --- | --- |
| Click a card | Fly there; the area is selected and shown in place as that version (a one-version [compare](compare.md)) |
| Shift / Ctrl | Change the selection before approving |
| Enter | Approve: paste it (one undo step); its chunks, blend band included, are marked done |
| Del | Reject |
| Esc | Close |

Right-click a card to reject it or bring a rejected one back. Verdicts are kept in the project, so a rescan
remembers them; approved areas stop showing as different.

Objects only your map has count as a difference too: approving removes them, as the paste leaves the area's objects as the
other version has them ([Compare](compare.md#objects)). Creatures are not touched.

## New terrain

Tiles your map lacks altogether (new islands, say) are shown alone from the other version; **Enter** adds them whole,
as one undo step. Each tile is converted to your map's format (alpha maps), ground effects your client lacks are
dropped, and its objects get fresh unique ids, except a placement a neighbouring tile already lists, which is the same
object. Added tiles are kept in the project and served to the editor as if the client had them; export writes them
with the map's updated WDT and WDL, and their minimaps ([Minimaps and far terrain](../output/minimaps-and-far-terrain.md)).
Where your map's own neighbouring tiles differ, the seam shows in **Problems**: paste or sculpt across it.

## Files

`<project>/differences/` holds per-tile results and verdicts per map pair; it can be deleted and is rebuilt.

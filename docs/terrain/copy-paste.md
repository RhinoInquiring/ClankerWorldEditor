# Copy and paste

[Back to the README](../../README.md) · Terrain group (F1), Copy tool (C)

Copy takes selected chunks (heights, texture layers, holes, water and the objects standing on them) and puts them
anywhere, on this map or another, with the seam blended into the ground around.

## Copy

Select chunks (click, box drag, **Shift** adds, **Ctrl** removes), then **Ctrl+C**. The copy follows the cursor as a
see-through ghost. To copy from another version of the map instead, pick that layer as **Copy from** in the Versions
window ([Ghost layers](../versions/ghost-layers.md)); the copy is then pinned in place, as it stands in that version.

## Place

| Input | Action |
| --- | --- |
| Click | Pin the paste there (the terrain shows the blended result) |
| Arrows | Move the pinned paste by one chunk |
| R / Shift+R | Turn a quarter clockwise / anticlockwise (lossless, objects included) |
| Alt+wheel | Fine height offset (Shift: finer) |
| Enter | Paste (one undo step) |
| Esc | Unpin; again to stop placing |
| Ctrl+V | Paste at the cursor at once |
| Ctrl+Shift+V | Paste in place: the spot it was copied from, its own heights |
| P | Start / stop placing the clipboard |

**Placement** tab: what comes along (**Heights**, **Textures**, **Holes**, **Water**, **Objects**), the height mode
(follow the ground, follow its slope, lowest point, absolute) and the blend band.

## Blending

A pinned paste blends into the ground: a smooth band of terrain (its width set automatically from how far the heights
differ at the edge, or by hand) carries the ground up to the pasted edge with no step, and textures cross-fade inward
over the band. A hard paste (blend off) still closes its edges: neighbouring chunks take the pasted edge heights, so
the client never shows a gap. **Problems** reports any open edge left anyway.

## Objects

Objects standing on the copied chunks come along, keeping their place on the copy, turned with it, and get fresh
unique ids. In the Copy tool an object belongs to the chunk its origin stands on, so copying one chunk of a city does
not drag the whole city model along. ([Compare](../versions/compare.md) uses buildings' bounds instead.)
Copied from another version (a ghost layer), the [landmarks](../regions/pois.md#landmarks-from-other-versions) on the
copied chunks come along too, with **Objects**.

## Rotate in place

**Edit > Rotate selection in place** turns the selected chunks a quarter about their centre, with the objects on them
(same ids, one undo step).

## See also

- [Blueprints](blueprints.md): keep a copy for later
- [Water](water.md): how water moves with copies
- [Compare](../versions/compare.md): paste an area from another version after comparing them all

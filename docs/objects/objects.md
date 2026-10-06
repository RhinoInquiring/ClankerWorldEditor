# Objects

[Back to the README](../../README.md) · Objects group (F2), Objects tool (O)

Doodads (M2 models: trees, rocks, furniture) and buildings (WMOs) are drawn with their real meshes, textures and
animations, including the doodad sets inside buildings. The Objects tool selects and edits them.

## Select

Click an object (picking is exact to its triangles); drag a box to select every object in it. **Shift** adds,
**Ctrl** removes, **Esc** clears.

## Move, turn, scale

A standard gizmo sits on the selection; every tool that moves things shares it ([Moving things](../concepts/moving-things.md)):

| Key | Action |
| --- | --- |
| 1 / 2 / 3 | Move / rotate / scale |
| X | World or local axes |
| Ctrl (while dragging) | Snap: 1 yd, 15 degrees, 0.1 scale |
| PgUp / PgDn | Raise / lower 1 yd (Shift: 0.1) |
| + / - | Scale up / down (Shift: finer) |
| G | Drop to the ground |
| Alt+click the ground | Move the selection there |
| Del | Delete |

The **Object** panel shows and edits position, rotation and scale; a drag of any field is one undo step. Buildings
keep scale 1 (the client ignores a building's scale).

**Furniture follows its building.** Blizzard places much of a building's furniture as map doodads rather than inside
the building. When **Move what stands inside buildings along** is on (Tools panel), moving a building moves every doodad inside
its bounds with it; faint boxes show which ones.

## Place

Click a model or building in the [Catalog](catalog.md): it follows the cursor; click the ground to place it (and
select it). **Shift+click** keeps placing; each copy gets a random turn and an optional scale jitter. **Esc** or a
right-click stops.

## On export

Moved, added and deleted objects are written into the tiles they stand on, with fresh unique ids for new ones (above
200,000,000, clear of Blizzard's). Unchanged objects keep their original references, so the client's view of them
does not change.

## See also

- [Copy and paste](../terrain/copy-paste.md): objects travel with copied terrain
- [Zones](../regions/zones.md): giving a building's rooms names (WMOAreaTable)

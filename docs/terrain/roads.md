# Roads

[Back to the README](../../README.md) · Terrain group (F1) · Roads (Tools panel, or Ctrl+P "Tool: Roads")

A road is a spline through points you place. It paints a centre texture and an optional shoulder texture along it and
grades the ground under it, live: move a point and the road follows. The spline exists only in the editor; the game
gets the ground it leaves.

## Drawing

**New road**, then click the ground for each point; **Enter** or a right-click finishes. With a road selected:

- **Add points** adds at the end (with the first point selected: at the start); **Ctrl+click** inserts a point between
  the nearest two.
- Click a point to select it; it moves with the shared handles (**1** move, **3** scale = the road's width there,
  **G** to the ground, **Del** deletes it, **Alt+click** moves it there). With no point selected the whole road moves
  and turns.
- Click the road's line to select the road; **Esc** steps back (point, then road).

## Seeing it

Every tool shows roads as they will look; the Roads tool adds the line, its edges and the points on top. Untick
**Show lines and points** (Roads tab, or View > Road lines and points) to see only the road: the handles hide too
until you tick it again.

Moving a point redraws only the stretch of road around it. A setting (width, textures, flatten...) redraws the whole
road; on a long road the preview then updates a few times a second while you drag, and once more when you let go.

## Settings

| Setting | What it does |
| --- | --- |
| Width | Edge to edge of the centre texture; a point can have its own (Width here) |
| Follow the ground | On: the road rides the ground, smoothed along its length. Off: it runs straight between its points' heights (ramps, cuttings) |
| Flatten | How far the ground is pulled to the road: level side to side, smooth along it |
| Sink | How far the road sits below the ground beside it |
| Centre, Shoulder | Textures: pick one in Catalog > Ground textures, then **Use the picked texture** |
| Shoulder | Width of the worn edge each side |
| Ragged edges | How much the edges wander and fray (stones thinning into dirt, dirt breaking into grass) |

A Barrens road: centre `Tileset\Barrens\BarrensRoad01.blp`, shoulder `Tileset\Barrens\BarrensBaseDirt.blp`.

## On top of the terrain

A road is drawn over whatever is under it: sculpting or painting there edits the ground underneath, and the road stays
on top. Delete the road and that ground shows again. A chunk holds four textures; a road's textures take the place of
the weakest ones where they need room.

**Bake into the terrain** writes the road as ordinary height and texture edits and removes the spline, in one undo
step. Export writes live roads the same way, so baking is only needed to edit the result by hand.

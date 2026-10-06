# Sculpt, paint and holes

[Back to the README](../../README.md) · Terrain group (F1)

## Sculpt (B)

Left drag shapes the ground under the brush. Modes (keys **1 to 4**): **Raise**, **Lower**, **Flatten** (to the
height where the stroke started) and **Smooth**. **Ctrl+wheel** sets the radius; strength is in the Tools panel. One
stroke, mouse down to mouse up, is one undo step.

On export the edited chunks get new normals, and so do their neighbours, across tile borders too, so the client
lights the new shape correctly with no seam lines.

## Paint (T)

Left drag paints the active texture; **Ctrl+drag** erases it. **Alt+click** picks the texture under the cursor.
Radius (**Ctrl+wheel**), pressure and hardness are in the Brush tab; recently used textures are one click away.
Clicking a ground texture in the [Catalog](../objects/catalog.md) picks it and switches to Paint.

A 3.3.5 chunk holds at most four textures. Painting a fifth replaces the chunk's weakest one; erasing hands the
share to the remaining textures. A painted texture takes the ground effect (grass, pebbles) it has elsewhere on the
tile. Ground effects the client has no record of are dropped on export, because they crash the client.

## Shade (U)

Vertex shading (MCCV) tints the ground per vertex (every 4.2 yd) on top of its textures. Left drag moves the colours
towards the picked colour; **Ctrl+drag** returns them to neutral; **Alt+click** picks the colour under the cursor.
Mid grey (127) leaves the ground unchanged, darker darkens, brighter lightens up to twice as bright. Radius
(**Ctrl+wheel**), pressure and hardness are in the Brush tab.

The client reads vertex colours only on maps whose WDT turns them on: Northrend does, Azeroth, Kalimdor and Outland
do not, and there the tool only explains why. Copy, paste and blueprints carry the shading; a blended paste fades it
in over the same edge as the textures. Export writes the colours into the tile in place.

## Holes (H)

Left drag cuts holes; **Ctrl+drag** fills them (or the other way round, set in the tab). **Ctrl+wheel** sets the
radius. A hole cell is 1/16 of a chunk (8.3 x 8.3 yd), the finest size the 3.3.5 client and AzerothCore support.
Holes are how caves and cellars open into the ground.

## Select (V)

Click a chunk to select it, drag a box to select every chunk you can see in it; **Shift** adds, **Ctrl** removes,
**Esc** clears. A selection is what [Copy](copy-paste.md), [Blueprints](blueprints.md) and
[Compare](../versions/compare.md) work on; its tiles stay loaded wherever the camera goes.

# Water

[Back to the README](../../README.md) · Terrain

The editor reads and draws terrain water: rivers, lakes, ocean, magma and slime, animated with the client's own
liquid textures, and the liquids inside buildings (WMO liquids). Old-style water from 1.12-era maps (MCLQ) is read too.

## Water tool

**Terrain > Water** paints liquid onto the ground, one cell (4.2 x 4.2 yd) at a time, 8 x 8 cells per chunk.

| Mode | Left drag |
| --- | --- |
| **Add** | Makes cells wet with the chosen liquid. New water takes the level; water already there keeps its height, so painting beside a lake does not move the lake |
| **Remove** | Dries cells of any liquid |
| **Level** | Moves the surface under the brush to the level (a blue ring shows it) |

**Ctrl+drag** swaps Add and Remove, **Ctrl+wheel** sets the radius, **Alt+click** takes the level and liquid of the water
(or ground) under the cursor. With **Match nearby water** on (the default), each Add stroke takes the liquid and level of
the water it starts on, or of the nearest water within 80 yd, so new water joins a lake or river without a seam; with no
water near, it starts at the ground plus **Depth** in the chosen liquid. **Take cells from other liquids** (on by default) makes magma painted into a lake
replace the lake there; off, the new liquid is layered over it.

**Liquid** lists LiquidType.dbc. Each liquid is written in the vertex format its material asks for: water with heights
and depth, magma and slime with heights and flow coordinates. Ocean is written flat (one height per chunk, no per-vertex
heights), as Blizzard's tiles store it. Depth (how see-through the water is) is worked out from the ground: 9 steps per
yard, measured on Blizzard's lakes.

**Slope** tilts the surface for rivers and falls: **Angle** in degrees, **Downhill** the direction it falls towards
(0 = +z, 90 = +x). The surface passes through the level at the point where the stroke starts. Ocean stays flat.

Each stroke is one undo step.

## In copy and paste

Copies carry each chunk's water. A paste replaces the water of every pasted chunk with the copy's, so a dry cave
pasted into a lake removes the lake's water there, and a wet area pasted onto dry ground brings its water. The blend
band around a paste keeps its own water. Untick **Water** in the Copy tool's Placement tab to leave the target's water
as it is. Tick **Map's water** to keep the copy's water where it is wet but give it this map's liquid and level: the
liquid (and so its colour) and surface of the map's water on that chunk, or the nearest within 80 yd, with its depth
worked out from the ground as pasted. Chunks pasted from another version beside a lake then join it without a change
of colour or a step in the surface. With no water within 80 yd the copy's water comes as it is. Undo, rotation, [blueprints](blueprints.md) and export all include water.

Water that belongs to a building or cave model (its WMO liquid) comes with the model, not with the terrain.

## On export

A tile whose water changed gets its water data (MH2O) written afresh; the client then ignores any old-style water
in that tile. Old-style water copied from a 1.12-era map is converted on the way. The fishing and fatigue masks of
the water travel with it.

The server knows water from its extracted maps: after water edits, extract the server's maps again from the patched
client.

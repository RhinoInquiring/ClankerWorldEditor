# Water

[Back to the README](../../README.md) · Terrain

The editor reads and draws terrain water: rivers, lakes, ocean, magma and slime, animated with the client's own
liquid textures, and the liquids inside buildings (WMO liquids). Old-style water from 1.12-era maps (MCLQ) is read too.

## In copy and paste

Copies carry each chunk's water. A paste replaces the water of every pasted chunk with the copy's, so a dry cave
pasted into a lake removes the lake's water there, and a wet area pasted onto dry ground brings its water. The blend
band around a paste keeps its own water. Untick **Water** in the Copy tool's Placement tab to leave the target's water
as it is. Undo, rotation, [blueprints](blueprints.md) and export all include water.

Water that belongs to a building or cave model (its WMO liquid) comes with the model, not with the terrain.

## On export

A tile whose water changed gets its water data (MH2O) written afresh; the client then ignores any old-style water
in that tile. Old-style water copied from a 1.12-era map is converted on the way. The fishing and fatigue masks of
the water travel with it.

The server knows water from its extracted maps: after water edits, extract the server's maps again from the patched
client.

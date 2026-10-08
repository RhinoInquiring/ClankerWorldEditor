# New maps

[Back to the README](../../README.md) · Terrain

**File > New map...** makes a map that exists nowhere else, as one undo step:

| Piece | What it is |
| --- | --- |
| Map.dbc row | Id from the project's `map.id` range (File > Project settings, default 800-999); folder, name, kind (world, dungeon, raid). Loading screen, minimap icon scale, expansion and flags come from the **Like** map |
| MapDifficulty.dbc row | Dungeons and raids only: player count, a week's reset for raids. Id from `mapdifficulty.id` (default 1000-1099) |
| instance_template row | Dungeons and raids, when the world database is linked: AzerothCore needs it to create the instance. Without the link, set it later in Triggers > Entrances |
| WDT and WDL | The map's tile list and far heights, in the project's overlay (8-bit alpha by default, as Northrend) |
| Tiles | A square of flat tiles (1 x 1 up to 8 x 8, 533 yd each) at one height, one ground texture and one area id |

The map opens at once, centred on its tiles. From there every tool works on it as on any other map: sculpt, paint,
water, objects, spawns. Paste terrain from other maps (Copy), or add whole tiles from another version (Differences).

Export writes the row(s) into `DBFilesClient\Map.dbc` (and `MapDifficulty.dbc`) for the client and the server, and the
tiles, WDT and WDL under `World\Maps\<folder>\`. The client and worldserver read DBCs at start: restart both, then
`.go xyz <x> <y> <z> <map id>` takes you there. **File > Build server data** makes its maps, vmaps and mmaps.

Undo removes everything the step added (the rows, the WDT, the tiles) and closes the map if it is open.

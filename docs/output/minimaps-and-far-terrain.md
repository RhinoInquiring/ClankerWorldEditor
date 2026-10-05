# Minimaps and far terrain

[Back to the README](../../README.md) · Export

## Minimaps

Every export draws the minimap picture of each edited tile of the open map again, as it now looks: straight down,
terrain, buildings and water, without trees or editor outlines (the client's own minimaps leave doodads out too),
256 x 256, in the client's layout (checked against Blizzard's pictures). Only tiles whose edits changed since the last
export are drawn again; the pictures are kept in `<project>/minimaps/`.

Export ships them as `textures/Minimap/wwe_<map>_<x>_<y>.blp` with the client's minimap index (`md5translate.trs`)
plus lines naming them. The client reads a single minimap index, so the exported one is the whole index as the
project's files have it, plus these lines. A drawn picture wins over an added tile's picture from its other version.

Tiles edited on another map keep their last picture until that map is open at an export.

**Inside buildings** (caves, dungeons) the client uses each building's own minimap pictures, per room. Those come with
the building's files (for the map modules, from their exporter), not from the editor.

## Far terrain

The client and the editor draw distant land from the map's low-detail heights (its WDL). Tiles added from another
version get theirs computed from the tile itself (chunk corners and centres, within 1 yd of what Blizzard's tools
write); the editor's far view updates at once, and export ships the map's WDL. Tiles you sculpt keep their old far
heights for now.

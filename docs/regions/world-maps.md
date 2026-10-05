# World maps

[Back to the README](../../README.md) · Regions group (F4), Zones tool (Z) > World map

A zone's world map is a picture (WorldMapArea.dbc gives its place in the world) plus overlays: pieces of picture that
appear once the player has explored an area (WorldMapOverlay.dbc).

**Create world map** (World map tab, with a zone picked) draws them from the terrain:

1. The bounds come from the zone's chunks (plus a margin, at the client's 3:2 shape).
2. The tiles under them load; the picture is rendered from straight above (terrain, buildings, water).
3. The base is a faded parchment version; each area of the zone gets an overlay with soft edges, cut into the tile
   sizes the client's world map frame expects.

The pictures go into `<project>/assets/Interface/WorldMap/<zone>/` and are copied into every export; the
WorldMapArea and WorldMapOverlay rows are changes like any other (ids from the project's `worldmaparea.id` and
`worldmapoverlay.id` ranges). The client reads DBCs at start: restart it to see a new map.

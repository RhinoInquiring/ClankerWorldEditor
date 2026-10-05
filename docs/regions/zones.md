# Zones

[Back to the README](../../README.md) · Regions group (F4), Zones tool (Z)

Every terrain chunk carries an area id (a row of AreaTable.dbc): it names the zone on screen, sets music, flight and
PvP rules, exploration and more. The Zones tool paints area ids and edits the rows.

## Paint

Pick an area in the **Areas** tab (a tree of the map's zones and sub-areas, with a filter), then left drag to paint it
onto chunks; **Ctrl+wheel** sets the radius. **Alt+click** picks the area under the cursor. Coloured borders show the
areas while the tool is active. One stroke is one undo step.

## Areas

- **New area**: a sub-area of a zone (copying the zone's sound, music and flags) or a new zone; ids come from the
  project's `area.id` range, and a new exploration bit is the lowest one unused.
- **Edit**: name, parent zone, level and flags of any row.

## Buildings (WMOAreaTable)

The rooms of a building can carry their own names (the "inside" names the client shows). **Shift+click** a building
in the Zones tool: the **Building** tab lists its rooms (WMO groups). Name the whole building (one row per room) or
single rooms. A placement can also get its own name set, so two copies of one building can have different names.

## On export

Painted tiles get their area ids written; AreaTable and WMOAreaTable are exported for the client
(`out/client/DBFilesClient`), the server (`out/server/dbc`) and as mod-dbc-patch JSON (`out/dbc`). The client and the
worldserver read DBCs at start, so restart them; the server reads chunk areas from its extracted maps, so extract
those again after painting (and its vmaps after giving a placement its own name set).

## See also

- [World maps](world-maps.md): the zone's map picture and its explored-area overlays

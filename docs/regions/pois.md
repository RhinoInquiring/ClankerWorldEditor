# Points of interest

[Back to the README](../../README.md) · Regions group (F4), POIs tool (J)

Three kinds of marked point, one tool. Pick the kind in the **Points** tab:

| Kind | Stored in | Who sees it | Takes effect |
| --- | --- | --- | --- |
| Landmarks | `AreaPOI.dbc` | Players: an icon with a name (and description) on the world map | Export, restart the client |
| Gossip | world `points_of_interest` | Players: the flag a gossip option marks on their map (`gossip_menu_option.ActionPoiID`) | At once (`.reload points_of_interest`) |
| Teleports | world `game_tele` | GMs: `.tele <name>` destinations | At once (`.reload game_tele`) |

Gossip and teleport points need the world database (File > Server setup).

## Points

The tab lists the kind's points on the open map; double-click one to fly there. In the viewport, landmarks are gold
squares, gossip points red flags, teleports cyan crosses with a line for the way players face; the selected one is
white and shows its unapplied edits live. Labels show the name and, for landmarks and gossip points, the icon.

Gossip points have no map (the client puts the flag on whatever map the player is on), so the editor shows every one
wherever the ground under it is loaded. Landmarks with no height (most of Blizzard's) stand on the ground.

- **Click** a point to select it; **Alt+click** the ground moves the selected one there; **Del** deletes it.
- **New**: name and fields, then **Place on the ground** (**Shift+click** keeps placing). Ids come from the project's
  ranges `areapoi.id`, `points_of_interest.id` and `game_tele.id` (default 60000-60999 each). A landmark takes the
  area of the chunk it stands on; a teleport faces where the camera looks.
- **Selected**: the fields and position are saved when let go; the handles and keys are those of [Moving things](../concepts/moving-things.md) (teleports also turn). For a gossip point, **Shown by** lists the gossip options
  (and their creatures) that mark it.

## Landmark fields

The icon button opens every cell of `Interface\Minimap\POIIcons`. **Like** copies the importance, flags and icon of a
Blizzard landmark: village (Goldshire: 3, 517, icon 7), town (Darkshire: 3, 525, icon 5), capital (Stormwind City:
3, 541, icon 6). What each flag bit does is not documented, so copying one of these is the safe choice.
**World state** stays 0 for a plain landmark; battleground and Wintergrasp points follow one. Only `Icon[0]` is set:
`Icon[1-8]` hold the state icons of destructible buildings (Wintergrasp towers, Strand gates) and are kept as they are.

## Problems

- An id range unset, or client AreaPOI rows inside the project's range.
- A teleport name the project uses that another `game_tele` row has too (`.tele` finds one of them).
- A gossip point the project made or edited that no gossip option shows.

## Landmarks from other versions

Terrain pulled in from another version of the map brings that version's landmarks along: a copy from a ghost layer,
a compare commit, a blueprint saved from a ghost, and whole tiles added from Differences. The other client's
`AreaPOI.dbc` is read whatever its build (1.x, 2.x or 3.3.5; 1.x icons and flags are converted to 3.3.5's numbering),
and the landmarks standing on the copied chunks move, turn and lift with the copy. On commit each becomes a new
AreaPOI row (project `areapoi.id` range) on the open map, with the area of the chunk it lands on, in the same undo
step as the terrain. One the map already has (same name within 150 yards) is skipped. The Log says how many came over.
Teleports and gossip points are not copied: they belong to the server, not to the map files.

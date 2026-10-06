# Flight paths

[Back to the README](../../README.md) · Regions group (F4), Flights tool (Y)

Flights are three client tables, all read by worldserver too: **TaxiNodes.dbc** (the flight points), **TaxiPath.dbc**
(one direction between two points, with its cost) and **TaxiPathNode.dbc** (the points that direction flies through).
Export writes them to the client's `DBFilesClient`, to `out/server/dbc` and as mod-dbc-patch edits; restart the client
and worldserver to fly them.

## In the viewport

Nodes are rings with a pole: gold when both teams fly from them, blue Alliance only, red Horde only, grey nobody.
Green lines join each flight master to the node it serves. Every path of the map is a thin orange line (Paths tab:
**Show every path of the map**); the selected path is drawn solid, with numbered balls and blue arrows for the way it
flies.

- **Click** a node to select it, or a ball of any path to select that path and point.
- **Click the ground** with a point selected: a new point after it, level with its neighbours.
- Nodes and points move with the [shared handles and keys](../concepts/moving-things.md) (move only). Moving a node
  moves the path ends that sat on it. **Del** deletes the selected point, or the node with every path from or to it.

## Nodes

**New node**: name, the Alliance and Horde mounts (Riding Gryphon, Riding Hippogryph, Wind Rider, Riding Bat, Red
Drake, or none: that team cannot fly there), then **Place on the ground** where players land.

A node needs a flight master: AzerothCore gives a player the node nearest to the flight master they talk to, among
the nodes of that map with a mount for the player's team. Place a creature whose template has the flight master flag
(npcflag 0x2000) next to the node with the Creatures tool; the **Selected** tab lists who serves the node, for which
team, and how far away they stand.

Node ids come from the project's `taxinode.id` range, 1 to 448 by default. AzerothCore keeps the nodes a player knows
as 14 x 32 bits, so a node above 448 can never be learned; Blizzard's go up to 440 and leave 84 gaps, which new nodes
fill.

## Paths

Select a node, pick **To** another node of the map, tick **And back** for the return path, set the cost (copper) and
the clearance, then **Create path**. Generated paths use as few points as they can.

- **Takeoff and landing are copied** from the nodes' existing flights: the first ~200 yards of the flight leaving the
  start node that heads most towards the destination (or an arriving flight, reversed), and likewise the last ~200
  yards into the destination. Blizzard's routes out of towers and cities are kept (less their nearly straight points). A node without
  flights yet gets a straight climb off the node.
- **The middle** is one straight line over the map between the copied takeoff and landing. Seen from the side it is
  the tightest line that keeps the clearance over the ground and the top of any building, tree or bridge (a ray
  straight down through the loaded models, every 4 yards), climbing to it 1 yard per 2 off a node and never less than
  5 yards over anything: a point only where something forces a bend. With nothing in the way the middle adds no
  points at all.
  Where tiles are not loaded it follows the map's low-detail heights and cannot see models: fly the camera along the route first for full avoidance.

Shape the route by moving the balls, adding points (click the ground) and deleting them. **Make the path back** adds the reverse of a path that has none. The Selected tab shows the
length and about how long the flight takes.

Path ids come from `taxipath.id` (5000-5999), point ids from `taxipathnode.id` (60000-69999).

## Problems

- An id range unset, or the node range going above 448; a project id another patch's client rows now use.
- A node with an id above 448, with no mount for either team, or with no paths.
- A path with fewer than two points, a gap in its point numbers, an end at a node TaxiNodes.dbc lacks, or not starting
  and ending within 30 yards of its nodes.

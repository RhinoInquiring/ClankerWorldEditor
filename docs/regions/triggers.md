# Triggers and entrances

[Back to the README](../../README.md) · Regions group (F4), Triggers tool (K)

An area trigger is a sphere or a turned box in the world. The client fires it when a player walks in (it knows the
trigger from AreaTrigger.dbc); AzerothCore checks the player against its own copy of the shape (the world table
`areatrigger`, same id) and then acts: a teleport (`areatrigger_teleport`), a quest objective, a tavern, a script.
The Triggers tool edits both copies together, the teleports, and how instances are entered and left.

Editing needs the world database (File > Server setup).

## Triggers

The **Triggers** tab lists the open map's triggers with their shape and, for teleports, where they lead; double-click
one to fly there. In the viewport, teleports are purple, other triggers yellow, the selected one white; arrival points
on this map are cyan crosses with a line for the way players face.
Boxes carry an arrow out of one face, crossed over: the way **Turn** points and **Length** runs (the game fires a
trigger from any side, so this is for lining a box up with an opening). The selected trigger shows its unapplied
changes live.

- **Click** a trigger to select it; **Alt+click** the ground moves the selected one there; **Del** deletes it.
- **New**: pick sphere or box and its size, then **Place on the ground**. Ids come from the project's
  `areatrigger.id` range. With "Teleports players" on, the next click sets where it sends players.
- **Selected**: position, shape, size and turn, saved when let go (handles and keys as in [Moving things](../concepts/moving-things.md): scale sets a sphere's radius or a box's width, height and length). One undo step covers the AreaTrigger.dbc row, the
  `areatrigger` row and the teleport.

## Teleports

In **Selected**, "Sends players somewhere" makes the trigger a teleport: name, target map, arrival point and facing.
**Pick the arrival on the ground** takes the next click: players arrive there facing where the camera looks. For an
arrival on another map, open that map in **Maps** first, then click (the trigger stays selected). The pick is saved at
once; the other fields are saved when let go. Ticking the box starts the arrival pick.

## Portal effects

Instance portals in the game are plain doodads named `InstancePortal*` (`World\GENERIC\ACTIVEDOODADS\INSTANCEPORTAL\`,
raid ones in `SPELLS\`) or `InstanceNewPortal*` (`SPELLS\`): client only, no server rows. With the Triggers tool, the Catalog shows a **Portal effects** tab of them (the scale slider
sits at its top). Select a trigger, then click a portal: it goes at the trigger's own position (caves included), facing
the camera, as its own undo step. It does not follow the trigger later; move, turn or scale it with the Objects tool
(O). Most of these models are particles, so their pictures may be empty; the client shows the effect.

## Entrances

The **Entrance** tab shows one map (the open one, or any other):

- **Corpse entrance** (Map.dbc `CorpseMapID`, `Corpse`): where a dead player's spirit appears to run back in.
  **Pick on the ground** sets it on the open map (open the continent the entrance is on, then click).
- **Instance** (dungeons and raids): `instance_template.parent`, the map players leave to.
- **Ways in**: every trigger teleporting to this map. The dungeon finder and logins without a saved instance use one
  of these (AzerothCore's `GetMapEntranceTrigger`).
- **Way out**: the trigger on this map teleporting to its parent (dungeons) or its corpse map (other maps), as
  AzerothCore's `GetGoBackTrigger` finds it. A map without a corpse entrance has no way out in AzerothCore.

## Problems

Problems > Check now looks at the triggers and teleports the project touched: a teleport without an AreaTrigger.dbc
row (the client never fires it), a target map Map.dbc lacks, an arrival inside another teleporting trigger (players
bounce on), and instances the project leads into without a corpse entrance, an instance_template row, or a way out.

## When changes take effect

| What | Client | Server |
| --- | --- | --- |
| Trigger shape, position, new or deleted trigger | Export, restart the client (AreaTrigger.dbc) | Written to `areatrigger` at once; worldserver reads it only at start |
| Teleport | Nothing to do | Written at once and reloaded over SOAP (`.reload areatrigger_teleport`) |
| Corpse entrance | Export, restart the client (Map.dbc) | Restart worldserver (Map.dbc in `out/server/dbc`) |
| Instance parent | Nothing to do | Restart worldserver |

On export, AreaTrigger.dbc and Map.dbc go to `out/client/DBFilesClient`, `out/server/dbc` and `out/dbc` (mod-dbc-patch
JSON); the rows go to `out/server/areatrigger.sql`, `areatrigger_teleport.sql` and `instance_template.sql`, each with
a `_revert.sql`.

## See also

- [Zones](zones.md): area ids and AreaTable
- [Server link](../server/server-link.md): the database and SOAP the tool writes through

# Paths

[Back to the README](../../README.md) · Units group (F3), Creatures tool (N)

Select one creature; the **Path** tab shows its waypoint path (`waypoint_data`, linked through `creature_addon.path_id`)
as solid balls joined by tubes, with blue arrows showing the direction of travel.

## Edit

**Draw / Edit path** starts editing:

| Input | Action |
| --- | --- |
| Click the ground | Insert a point after the selected one |
| Click a ball | Select that point |
| Gizmo | Move the selected point (Ctrl snaps); points can sit on bridges and floors |
| G | Drop the selected point to the ground |
| Del | Delete the selected point |
| Enter | Save |
| Esc | Cancel |

The table sets each point's wait time and movement (walk, run, land, take off). A cyan ball walks the path at walk
and run speed, waits included, as a preview.

## Saving

Saving writes the points, the creature's `creature_addon` row and `creature.MovementType` (2 = waypoints) as one undo
step; an empty path removes it and sets the creature back to standing. The path id is the addon's existing id, or a
free one derived from the creature's guid. **Reload on server** sends `.wp reload <id>` over SOAP so a running
server picks the path up.

AzerothCore walks straight lines between points (unless `smoothTransition` is set); the editor draws exactly that.

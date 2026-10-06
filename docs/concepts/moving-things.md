# Moving things

[Back to the README](../../README.md)

Everything in the world that can be moved is moved the same way: objects, creatures, gameobjects, triggers, points of
interest and path points. Select it in its tool, then use the handles, the keys or the fields.

## Handles and keys

A gizmo sits on the selection (on one item, its own axes; on several, their centre).

| Key | Action |
| --- | --- |
| 1 / 2 / 3 | Move / rotate / scale handles |
| X | World or local axes |
| Ctrl (while dragging) | Flip snapping (the **Snap** box and its yards / degrees / scale steps) |
| PgUp / PgDn | Raise / lower 1 yd (Shift: 0.1) |
| + / - | Scale up / down (Shift: finer) |
| G | Drop to the ground |
| Alt+click the ground | Move the selection there (spawns and triggers land on the ground) |
| Del | Delete |

A drag shows live (models and outlines follow the handle) and is one undo step when let go. Fields in the tool's
panel work the same way: dragging or typing **Position**, **Facing**, a size or a name shows at once and is saved
when let go. There is no Apply button anywhere; undo (Ctrl+Z) takes a change back.

The bar at the top of each selection panel holds the handle mode, world / local, the snap steps, and says what the
selection allows.

## What each kind allows

| Kind | Move | Rotate | Scale |
| --- | --- | --- | --- |
| [Objects](../objects/objects.md): doodads | yes | free | uniform |
| Objects: buildings (WMOs) | yes | free | no (the client ignores it) |
| [Creatures](../units/spawns.md) | yes | about the vertical (facing) | no (none per spawn) |
| [Gameobjects](../units/spawns.md) | yes | about the vertical (tilt not edited yet) | no (none per spawn) |
| [Triggers](../regions/triggers.md): spheres | yes | no | radius |
| Triggers: boxes | yes | about the vertical (turn) | width, height, length separately |
| [Points of interest](../regions/pois.md): teleports | yes | about the vertical (arrival facing) | no |
| Points of interest: landmarks, gossip points | yes | no | no |
| [Path points](../units/paths.md) | yes | no | no |
| [Flight nodes and points](../regions/flight-paths.md) | yes | no | no |

Picking a handle the selection does not allow shows no handles and a note in the bar; press 1 to move.

Moving a trigger, point or spawn updates what depends on the spot: a spawn's and a landmark's area are those of the
chunk they land on.

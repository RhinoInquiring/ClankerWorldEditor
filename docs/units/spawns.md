# Creatures and gameobjects

[Back to the README](../../README.md) · Units group (F3): Creatures (N), Gameobjects (I)

Spawns are rows of AzerothCore's world database (`creature`, `gameobject`). With a [server link](../server/server-link.md)
the editor shows the spawns around the camera with their real models: creature skins and texture variations,
humanoid NPCs with their baked faces, hair and equipment (helmet, shoulders, weapons, shield), gameobjects from their
display ids, all animated. Markers and labels mark each spawn; the View tab and View menu hide them per kind.

## Place

Pick a template in Catalog > **Creatures** or **Gameobjects** (or search in the Place tab): it follows the cursor;
click the ground to place it. **Shift+click** keeps placing. New spawns stand on the ground and take the zone and
area of the chunk they stand on.

## Select and edit

| Input | Action |
| --- | --- |
| Click a marker or model | Select |
| Drag a box | Select every marker in it (Shift adds, Ctrl removes) |
| Alt+click | Move the selection to the cursor (each to the ground) |
| Del | Delete |
| Esc | Clear the selection |

The **Selected** tab edits one spawn or many at once: facing, wander distance, respawn time, drop to the ground,
delete. Editing a group is one undo step. The **Inspector** shows every column of the spawn's row and of its
template.

## How edits reach the server

Each edit is a change holding the whole row before and after. With the server linked, changes are written to the
world database as you make them (and undone there too). Export also writes
`out/server/creature_spawns.sql` / `gameobject_spawns.sql` and their `_revert.sql` counterparts, to apply elsewhere.
The worldserver reads spawns at start: restart it to see them in game.

## IDs

New rows take guids from the project's ranges (**File > Project settings**; default 9,000,000 to 9,099,999 for each
table): the lowest free one, never reused, never outside the range. **Suggest** proposes the next free block above
everything in the table. Problems warns when other rows sit in your range, and errors when a range is full or unset.
An id is fixed once a change records it.

## See also

- [Paths](paths.md): waypoints for a selected creature

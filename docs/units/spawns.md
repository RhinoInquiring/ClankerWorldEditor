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
| 1 / 2 / 3, X, PgUp / PgDn, G, Del | Handles and keys of [Moving things](../concepts/moving-things.md) (move; rotate turns the facing) |
| Esc | Clear the selection |

The **Selected** tab edits one spawn or many at once: position (one spawn) and facing preview live, as do the handles; wander distance, respawn time, drop to the ground,
delete. Editing a group is one undo step. The **Inspector** shows every column of the spawn's row and of its
template.

**On this map** (a tab of the Creatures and Gameobjects tools) lists every spawn of the open map, grouped by
template, most spawned first, with a filter by name or entry. Open a group to see its spawns; click one to fly there
(close in, for dungeons) and select it. Right-click a group to select every one of them, or open it in the NPC viewer.
Dungeons list their whole instance.

With one creature selected, the **Inspector** shows the [NPC viewer](npc-viewer.md)'s editing tabs for its template
(Template, Models & gear, Appearance, Loot, Dialogue), after a **Spawn** tab with the spawn's own row. They are the
same edits as the viewer's, applied with **Apply** as one undo step; selecting another creature while edits are not
applied asks first.

## Game events

Spawns tied to a game event (`game_event_creature`, `game_event_gameobject`) carry a tag in their label:
`[event 12]` appears only while event 12 runs, `[not in event 12]` leaves while it runs. The Selected tab names the
event. **Game event** (Place tab, also in the View menu) picks what the viewport shows:

| Choice | Shows |
| --- | --- |
| Every spawn | All rows, events ignored (default) |
| No event running | The world on a normal day |
| During event N | The world while only event N runs (listed: the events of the spawns around the camera) |

Changing the filter clears the selection, so hidden spawns are never edited.

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

# Objects

[Back to the README](../../README.md) · Objects group (F2), Objects tool (O)

Doodads (M2 models: trees, rocks, furniture) and buildings (WMOs) are drawn with their real meshes, textures and
animations, including the doodad sets inside buildings. The Objects tool selects and edits them.

## Select

Click an object (picking is exact to its triangles); drag a box to select every object in it. **Shift** adds,
**Ctrl** removes, **Esc** clears.

## Move, turn, scale

A standard gizmo sits on the selection; every tool that moves things shares it ([Moving things](../concepts/moving-things.md)):

| Key | Action |
| --- | --- |
| 1 / 2 / 3 | Move / rotate / scale |
| X | World or local axes |
| Ctrl (while dragging) | Snap: 1 yd, 15 degrees, 0.1 scale |
| PgUp / PgDn | Raise / lower 1 yd (Shift: 0.1) |
| + / - | Scale up / down (Shift: finer) |
| G | Drop to the ground |
| Alt+click the ground | Move the selection there |
| Del | Delete |

The **Object** panel shows and edits position, rotation and scale; a drag of any field is one undo step. Buildings
keep scale 1 (the client ignores a building's scale).

**Furniture follows its building.** Blizzard places much of a building's furniture as map doodads rather than inside
the building. When **Move what stands inside buildings along** is on (Tools panel), moving a building moves every doodad inside
its bounds with it; faint boxes show which ones.

## Place

Click a model or building in the [Catalog](catalog.md): it follows the cursor; click the ground to place it (and
select it). **Shift+click** keeps placing; each copy gets a random turn and an optional scale jitter. **Esc** or a
right-click stops.

## Doodad sets, replacing, dungeon WMOs

With one WMO selected, the **Object** window's **Doodads** list picks its furniture set (set 0 always shows; the
placement adds one more). To swap a model for another in the same place and turn, pick the new one in the Catalog,
then **Replace with ...** in the Object window (doodads take doodads, WMOs take WMOs).

On a WMO-only map (most dungeons) the map's own WMO is an object like any other: select it, move or turn it with the
handles, change its doodad set or replace it. It lives in the map's WDT, which export writes with it; it cannot be
deleted (the map is that WMO).

## Building editor

A WMO file carries its own furniture (doodad sets, MODS, of doodads, MODD, that every placement of that file shows)
and is made of groups (rooms, halls, a bridge span). Both are edited in the **Building editor**: a window of its own
where the building stands alone at the origin, in its own axes, with nothing of the map around it.

Open one:

- from the map: Objects tool, select a building, **Edit building** in the Object window (the tab remembers that
  placement and shows the doodad set it shows);
- from the Catalog: right-click a building, **Edit in the Building editor**;
- View > Building editor, or the command palette (Ctrl+P), shows the window again.

Each building is a tab; opening one already open brings its tab forward. The panel on the left edits the shown tab; the
map shows every edit too.

The view:

| Input | Action |
| --- | --- |
| Right drag | Look around (the eye stays put) |
| W A S D / Q E | Fly (Shift: 5x); the wheel while looking sets the fly speed |
| Middle drag | Orbit the centre; Shift+middle drag pans |
| Wheel | Zoom towards the centre |
| F | Frame the selection, else the whole building |
| Click | Select (Shift adds, Ctrl removes); Alt+click moves the selection there |

While a Building editor panel has the keyboard, keys go to it (the map's tool keys wait).

The first time, pick how to edit:

| Choice | What changes |
| --- | --- |
| **Make a copy for this placement** (opened from the map; the usual one) | A new WMO file in the project, `<name>_c1.wmo` with its groups; this placement uses it (one undo step with the copy) |
| **Make a copy** (opened from the Catalog) | The same new file, used by nothing yet: it is picked for placing, click the map to put it there |
| **Edit the original** | The game's file itself: every placement of it, on every map |

### Doodads

The building's doodads work like map objects: click to select (Shift adds, Ctrl removes), the same handles and keys
move, turn and scale them, **G** drops them onto the floor below, **Del** deletes. The panel lists the doodads of one set
and edits position (in the building's own axes), scale and the lighting colour of one; **Duplicate**, **Delete**,
**Reset turn**, **Replace with ...** and **Move to set** work on the selection.

- **Add**: pick a doodad in the Catalog, then click in the view; it joins the set shown in the panel and takes the
  lighting colour of the nearest doodad of that set. Shift+click keeps adding.
- **Sets**: the view shows set 0 (always shown) and the set picked in the panel. Add a set, rename one, or delete the
  last one (placements name sets by number, so only the last can go; set 0 always stays). Opened from the map, a set
  the placement does not show gets a **Show it there** button.
- Each doodad belongs to the group whose box holds it (the group lists it in MODR, which the game uses to draw and
  light it). Untouched doodads keep the groups the file gave them.

Edited WMOs live in `<project>/wmos/` (rebuilt from the changes when the project opens), are read above the game's
files, and export writes the root and every group, plus any model a new doodad needs that players lack. WMOs from
newer clients (doodads or groups by FileDataID) cannot be edited yet; export converts them.

Not yet: a copy goes to one placement only (other placements keep the original); a copy has no interior minimap
pictures of its own; doodads inside a WMO are not part of a terrain copy and paste.

### Groups: cut and paste between buildings

**Groups** at the top of the panel switches from doodads to the building's parts (WMO groups). Click a group to select
it (Shift adds, Ctrl removes); the list names each group, the file it came from and whether it is lit as indoor or
outdoor.

| Key / button | Action |
| --- | --- |
| Ctrl+C | Copy the selected groups, with the doodads standing in them |
| Ctrl+X | Cut them (copy, then delete from this building) |
| Ctrl+V | Paste into the shown building: its bottom middle lands where the cursor last pointed in the view (a floor, else the floor plane) |
| Handles, PgUp/PgDn, Alt+click | Move the selected groups; turning snaps to quarter turns about the vertical |
| Del | Delete the selected groups and their doodads |
| Lit as | As it is, indoor or outdoor |

To move groups between buildings: copy in one tab (even one not made editable), switch to the other tab, make it
editable and paste. What a group needs comes with it: its materials and textures (shared when identical), its name, the
lights and fog volumes it uses, and its doodads (into the set of the same name, made if missing). Pasted groups get
their own WMOAreaTable group ids, so room names do not collide.

- **Portals** stay between groups that came in together and moved together. A group pasted alone has none: lit as
  outdoor it is drawn from outside; drawing new portals comes with the portal tool.
- **Turns** are quarter turns so each group's collision tree stays exact (it splits along the axes); free turns need
  the tree built again, which comes with geometry editing.
- Notes under the list say when a source names its liquids or lights its vertex colours differently from this building.

Each group is written from its source file every time the building is saved; groups whose inputs did not change are
not written again.

## On export

Moved, added and deleted objects are written into the tiles they stand on, with fresh unique ids for new ones (above
200,000,000, clear of Blizzard's). Unchanged objects keep their original references, so the client's view of them
does not change.

## See also

- [Copy and paste](../terrain/copy-paste.md): objects travel with copied terrain
- [Zones](../regions/zones.md): giving a building's rooms names (WMOAreaTable)

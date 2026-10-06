# NPC viewer

[Back to the README](../../README.md) · View > NPC viewer · right-click a creature in the Catalog · **Open in NPC viewer**
on a selected creature spawn

One creature template in a preview of its own: every animation, its skins, what it carries, its submeshes and
textures. The layout follows wow.export's Creatures tab ([Kruithne/wow.export](https://github.com/Kruithne/wow.export),
MIT), rebuilt on this editor's renderer and AzerothCore's tables. Needs the world database.

| Part | What it shows |
| --- | --- |
| List (left) | Every `creature_template`, filtered by name or entry |
| Preview | The model as it spawns. Left drag turns, right drag pans, the wheel zooms |
| Animation bar | Every animation of the model (names from `AnimationData.dbc`, `.anim` files loaded as needed), play / pause, frame steps, a time slider, speed |
| Models | The template's `creature_template_model` rows (display, scale, chance); pick one to show it |
| Skins | Every display drawing the same model, with its skin names (preview only) |
| Equipment | The `creature_equip_template` sets; each carried item (helmet, shoulders, weapons, shield) and the cape can be hidden |
| Geosets | One "Group: variant" dropdown per submesh group (None / a variant / All), named like wow.export (Hair, Gloves, Cloak, ...) on character models; single submeshes and the base mesh are checkboxes; **As spawned** resets |
| Textures | The look's skins, hair, cape and the model's fixed textures; hover for a large view, click to copy the path |

Items follow the animated attachment points here (in the world view they stay at the first frame).

## Editing

The **Template** and **Models & gear** tabs edit the open creature. Edits show in the preview at once and are written
with **Apply** (one undo step for all three tables); **Revert** drops them. Opening another creature with edits
pending asks first.

| Tab | Edits |
| --- | --- |
| Template | `creature_template`: name, subname, cursor, levels, expansion, rank, class, type, faction (its name shown), NPC flags, gossip menu, movement and speeds, aggro range, health / mana / armor / damage / experience multipliers, attack time, regeneration, loot id, money, AI and script name. **All columns** edits any other column as text |
| Models & gear | `creature_template_model`: display, scale and chance per model (**Add the shown display** adds the one previewed, e.g. picked under View > Skins); `creature_equip_template`: sets with main hand, off hand and ranged items chosen from a searchable list with their icons |

**Duplicate as new NPC** copies the template, its models and its equipment to the next entry of the project's
`creature_template.entry` range (default 9000000-9099999, File > Project settings); the copy appears in the Catalog to
place. **Delete** is offered only for entries in that range (undo brings them back; spawns are left alone). Other
tables keyed by entry (`creature_template_addon`, `_movement`, `_resistance`, `_spell`, locales) are not copied yet.

## Appearance

The **Appearance** tab edits character-model looks (CreatureDisplayInfo + CreatureDisplayInfoExtra rows), in the
layout of wow.export's character tab: **Race** and **Body**, then one dropdown each for **Skin color**, **Face**,
**Hair style**, **Hair color** and **Facial hair** (only the values CharSections, CharHairGeosets and
CharacterFacialHairStyles have for that race and body; faces follow the skin, hair colours the style), and the 11
**Armour** slots (head to back), each filled from a searchable list of items for that slot.

- A client display is shared by other creatures, so it is never edited: **Make an editable copy** copies it (and
  its extra row) into the project's `creaturedisplayinfo.id` / `creaturedisplayinfoextra.id` ranges (default
  90000-90999) and puts the copy in the template's models; **New character appearance** starts a human.
- New appearances have no baked texture (`BakeName` empty), so the client composites skin, face, hair, underwear
  and armour itself. The editor composites them the same way for its preview (and for every unbaked NPC in the
  world view); `--skin-check` compares its result with Blizzard's bakes.
- Applying writes the rows with the NPC's other edits. Export (or build the patch) puts the two DBCs in
  `out/client/DBFilesClient`, `out/server/dbc` and `out/dbc/*.json` (mod-dbc-patch); copy `out/server/dbc` to the
  server and restart client and worldserver.

On a running worldserver, Apply sends `.reload creature_template <entry>` over SOAP for existing templates; models,
equipment and new templates need a restart (AzerothCore has no reload for them).

Not yet: wireframe / bone view, export.

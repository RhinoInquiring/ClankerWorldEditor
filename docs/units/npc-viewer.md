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

The three parts resize: drag the list's right edge, or the preview's right edge to give the tabs more room (the
layout is remembered). Tabs that do not fit scroll, and the arrow at the tab bar's end lists them all.

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

## Loot

The **Loot** tab edits what the creature drops (`creature_loot_template`, by `lootid`), can be pickpocketed for
(`pickpocketing_loot_template`, `pickpocketloot`) and skinned for (`skinning_loot_template`, `skinloot`).

- Each row is an item (icon, name in its quality colour) or a **reference** to a shared `reference_loot_template`
  (expand it to see what it holds). **Chance** in percent (100 always); **Group** 0 rolls the row on its own, rows of
  group 1 and up drop at most one of them (chance 0 in a group: an equal share of what the others leave); **Min / Max**
  count; **Quest** = only for players on a quest needing it; **Mode** = loot mode mask (1 normal).
- **Add item...** searches every item; **Add reference** adds a reference row by entry.
- A creature with no loot id gets **Give it loot of its own** (the id becomes its entry). A loot id shared with other
  creatures is flagged; **Give it loot of its own (copy)** copies the rows to this creature's own id first.
- Groups whose chances add up past 100% are flagged.
- Apply writes the rows with the NPC's other edits and sends `.reload creature_loot_template` (or the pickpocketing /
  skinning table) over SOAP: a running server drops the new loot at once.

## Dialogue

The **Dialogue** tab has two parts.

**Gossip** edits what the creature says when a player talks to it: its menu (`gossip_menu_id`), the menu's texts
(`npc_text`) and options (`gossip_menu_option`), and the conditions on them.

- A preview drawn like the game's gossip window shows the text and the options with the game's own icons. Click an
  option that opens a menu to follow it; the trail above goes back.
- **Create a gossip menu** makes a menu and its first text in the project's `gossip_menu.id` / `npc_text.id` ranges
  (default 9000000-9099999) and turns on the Gossip NPC flag. A menu or text other creatures use is flagged.
- Texts: one text to men (and everyone when the second is empty), one to women; `$N` name, `$C` class, `$R` race,
  `$B` new line. A menu may hold several texts: the server shows the last (by id) whose conditions hold, so
  **+ another text** with a condition replaces the default while it holds.
- Options: icon, text, what choosing it does (gossip submenu, vendor, trainer, innkeeper, banker ...; the NPC flag it
  needs is set on the option and offered on the creature), **+ submenu** (a new menu the option opens), **Ask first**
  (a confirmation box, optionally with a price in copper).
- Conditions (**+ show only when...**): quest rewarded / taken / complete / not taken / state, level, class, race,
  gender, team, reputation, items, auras, spells, skills, achievements, titles, game events, zone / area / map; each
  can be negated, and else groups combine them (all of one group, any group).

**Barks** edits the creature's `creature_text` lines: group, text, say / yell / emote / whisper (and boss variants),
range and chance. Scripts say a group (SmartAI's Talk action, C++ `Talk(group)`); one line of it is picked by chance.

Applying reloads `gossip_menu`, `gossip_menu_option`, `conditions` and `creature_text` on a running server over SOAP.
`npc_text` has no reload: restart worldserver (and players may need to clear their cache) to see changed texts.

On a running worldserver, Apply sends `.reload creature_template <entry>` over SOAP for existing templates; models,
equipment and new templates need a restart (AzerothCore has no reload for them).

Not yet: wireframe / bone view, export.

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

Not yet: editing the template, its models or equipment (next step), wireframe / bone view, export.

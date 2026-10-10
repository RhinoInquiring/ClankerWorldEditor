# Races

[Back to the README](../../README.md) · View > Races

The races of every source: the project's client and each compare source (View > Sources). Each client's tables are
read in its own layout, 1.12 or 3.3.5. The window shows what each race is and what its characters can look like,
with a preview, and a race of another client can be imported into the project.

Its three panels, **Race list**, **Race preview** and **Race details**, can be moved like any other panel: drag a tab to
dock it elsewhere, tab panels together or float them. View > Reset panel layout puts them back.

| Part | What it shows |
| --- | --- |
| Source (top left) | The client whose races are listed. A source made of patch MPQs without race tables says so. |
| List (Race list) | Every row of the client's `ChrRaces`: id, name, team, skin colours male / female. **new** marks a race whose name the project's client does not have. |
| Details (Race details) | File string, prefix, female and male names, faction template (with its faction's name), base language, displays, the character model per sex, the classes it can be (`CharBaseInfo`), and how many skins, faces, hair styles, hair colours and facial hair it has per sex |
| Warnings | A file string another race of the project's client uses, or an id that is another race there: an import of the race will need its own |
| Preview (Race preview) | The character as the character creator shows it, naked, standing. The arrows step through skin, face, facial hair, hair style and hair colour. Left drag turns, right drag pans, the wheel zooms. A project race imported from a source shows with that source's files. |
| Import into the project (another source's race) | **Race id** (the first free one in the project's `race.id` range, 22-31 by default), **Alliance / Horde**, **Faction template**, **Classes**, **Starting outfits of added classes from**, and **Import as race N** |
| Edit (a race of the project) | The editor below: **Apply** makes the edits one change, **Discard** drops them. **Remove from the project** (an imported race) or **Back to the client's** (a client race the project changed). |

## Importing a race

**Import as race N** copies the race into the project as one change: undo takes all of it back. Its choices start
as the source race has them:

| Choice | What it sets |
| --- | --- |
| Alliance / Horde | `ChrRaces.Alliance`: the side whose character creator lists the race, and who it groups and talks with. A 1.12 race (no such column) starts with the team of a project race with the same faction template, else Horde. |
| Faction template | `ChrRaces.FactionID`: the faction a new character belongs to (reputations, which NPCs are friendly). The list shows each playable race's template as `id: faction name (races using it)`, plus the source's own. One the project's client lacks is flagged. |
| Classes | `CharBaseInfo`: the classes it can be. A class left out loses its starting outfits too. |
| Starting outfits of added classes from | For a class the source race has no outfits of: whose `CharStartOutfit` rows it copies (new ids), or none. A race's own outfits of a class always win. |

| What | Becomes |
| --- | --- |
| `ChrRaces` row | race N, its displays the new ones below; a 1.12 race gets the team of a race of the project with the same faction template, else Horde |
| `CharSections`, `CharHairGeosets`, `CharStartOutfit` rows | race N, each with a new id from the project's `charsections.id`, `charhairgeosets.id` and `charstartoutfit.id` ranges |
| `CharacterFacialHairStyles`, `CharBaseInfo` rows | race N (these tables have no id of their own) |
| Male and female `CreatureDisplayInfo` rows | new ids in `creaturedisplayinfo.id` |
| Their `CreatureModelData` rows | the project's client's row for the same model file when it has one, else a new id in `creaturemodeldata.id` |

## Editing a race

Any race of the project can be edited: an imported one, or one of the client's (applying makes it the project's).
Edits stay unapplied ("not applied") until **Apply**; undo takes an applied edit back.

| Tab | What |
| --- | --- |
| Identity | Name, female and male names, **Playable** (ChrRaces flag 0x1 marks an NPC race; an import clears it), file string (picks the login screen's model `UI_<file string>` and the race's sounds; its characters' files come from its displays and `CharSections`), prefix (helmets are found as `<helmet>_<prefix><M/F>.m2`), team, faction template, base language (`Languages.dbc`, by name), intro cinematic |
| Classes | The classes it can be. A class dropped loses its starting outfits; a class it has no outfits of takes those of the race picked (new ids), or starts naked. |
| Looks | Per sex, every skin colour, face, hair style, hair colour and facial hair, each with a texture to tell it apart, and **Remove**. The character creator steps through each choice by index, so the values after a removed one move down: a skin colour takes its faces and underwear with it, a hair style its geosets and scalps, a hair colour its scalps and facial hair of that colour. The preview shows the source's tables. |
| Server | The world database rows its new characters are made from (needs the server link; written as you go, undo takes them back): **Copy its server rows** from a race, the start location of each class (**Here**: the ground under the camera on the open map, facing where the camera looks; **Go**), base stats, extra starting items, and how many skills, spells and action bar buttons it has. See below. |
| Starting items | Per class and sex: the `CharStartOutfit` items (the creator shows them, AzerothCore gives them). **Change** and **Add an item** search `item_template`; an item the server does not have is flagged (nobody gets it). **Add a starting outfit** for a class and sex with none. Needs the server link to search. |

## The server's side

AzerothCore makes a character of a race and class only when `playercreateinfo` has a start for that pair, and (since
its change of 2026-02-28) it takes a race as playable from the server's `ChrRaces.dbc`: a row without flag 0x1. The
Server tab writes the rows; export puts the race into the server's DBCs.

| Table | Key | What **Copy its server rows** gives the race |
| --- | --- | --- |
| `playercreateinfo` | race | The start of each of its classes. A class the source race cannot be takes the start of the first stock race that can. |
| `playercreateinfo_action` | race | Action bar buttons per class, from the same race the start came from |
| `playercreateinfo_item` | race | Extra starting items per class (a negative amount takes an item away) |
| `player_race_stats` | Race | Base stats added to the class's |
| `playercreateinfo_skills` | raceMask | The rows of the source race's own mask bit, now the race's bit; a racial language becomes the race's base language (Common 98, Orcish 109, Darnassian 113, Taurahe 115, Dwarvish 111, Thalassian 137, Gnomish 313, Troll 315, Gutterspeak 673, Draenei 759) |
| `playercreateinfo_spell_custom` | racemask | The rows of the source race's own mask bit |

Rows of shared masks (0 = every race, or several races) stay as they are. A race past 32 has no bit in a 32-bit
mask, so no skills or spells of its own.

The ranges are in project settings. A race id past 32 needs the client's 64-race extension and a server that takes
it. The race's models and textures, and its tables in the patch, come with export (not done yet).

Notes:

- To see another client's races, add its `Data` folder as a compare source.
- Some clients reuse unused race rows for new races. For example, a Void Elf in row 12 that still has Fel Orc's file string.
- `CharSections` often names textures the client does not have; the stock 3.3.5 client does too. The client skips
  them, and the details give their count.
- 1.12 clients (Turtle) are read in their own layout. Their models and textures need converting to 3.3.5
  (wow-upport) before the preview can show them: a source of the converted archives plus the client's tables.

Checks: `--race-check` and `--race-look` ([command-line checks](../development/checks.md)).

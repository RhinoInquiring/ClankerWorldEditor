# Client versions

The editor's output is always for 3.3.5a (build 12340). Data from any other client is converted when it comes in, so
everything past that point (the windows, export, the server) only ever sees 3.3.5 data. A conversion has two parts:

- **Layout**: which column is where, how wide, in which order (WoWDBDefs lists them per build).
- **Meaning**: what a value means. The same column can mean something else in another version, so a layout alone is
  not enough. These rules are the ones that silently break things; every one found goes in the table below.

## What converts today

| From | Data | Where | How |
| --- | --- | --- | --- |
| 1.12 (5875) | Race tables: `ChrRaces`, `CharSections`, `CharHairGeosets`, `CharacterFacialHairStyles`, `CharBaseInfo`, `CharStartOutfit`, `CreatureDisplayInfo`, `CreatureModelData` | `Races.cpp` (`Classic()` layouts, `RaceCatalog::Load`) | Read in the 1.12 layout, named as 3.3.5 names them; see the meaning rules below |
| 1.12 | Models (M2 v256), textures | outside the editor: wow-upport | The converted archives are added as a source (`MPQs-upported\...`) |
| Newer (CASC: Classic, retail) | M2 (MD21), skins, `.anim`, WMO (FileDataIDs), split ADT, WDT | `Downport.cpp`, `MergeSplitAdt`, `DowngradeWdt` (`Formats.cpp`) | On read and on export (`CopyMissingAssets`, `--casc-to-mpq`) |
| Newer | DB2 tables | not yet | |
| 2.4.3, 4.x, 5.x clients (MPQ) | anything | not yet | |

## Meaning rules

| Table / file | Versions | Rule |
| --- | --- | --- |
| `CharSections.Flags` | 1.12 -> 3.3.5 | 1.12: `1` = only NPCs wear it, `0` = players may choose it. 3.3.5: `0x1` = players may choose it. Imported as-is, the creator offers none of the race's looks and dresses it in an NPC skin. `ClassicSectionFlags` converts it; races imported before the fix get a **Convert its looks for 3.3.5** button in the race editor. |
| `CharSections.Flags` 0x10 | any -> 3.3.5 | Every stock player row of a skin, facial hair, hair or underwear has 0x10 besides 0x1 (17); only Death Knight-only rows (5) and faces (1) lack it. A race whose rows have 0x1 alone looks right after login but, when a transform ends (Metamorphosis), comes back with another race's skin and no Features (Turtle's goblin: no head). 1.12 imports convert to 0x11 for those sections, and export gives a project race's 0x1-only rows 0x11. |
| `CharSections` underwear (section 4) | 1.12 -> 3.3.5 | 1.12 takes a player row with no texture (Turtle's female goblin has no underwear art); stock 3.3.5 never has one, and the creator then fails to build the body texture and shows the model's own skin. Export points such rows at a transparent `Character\WowWorldEditor\BlankUnderwear.blp` (128 x 64) it ships in the patch. |
| Character body textures | any -> 3.3.5 | The client composites a character's skin, face, facial hair, scalp and underwear only from palettized BLPs (Blizzard's are; Turtle's goblin skins are DXT1, so the body came out as the model's own skin). Export converts every `CharSections` texture but a hair row's first (drawn on the hair) to a 256-colour palettized BLP with mips (`WriteIndexedBlp`). |
| `CreatureModelData` of a character | 1.12 -> 3.3.5 | 1.12 rows end at `CollisionHeight`: mount height, geometry box and `WorldEffectScale` / `AttachedEffectScale` read as 0, and the client draws attached spell effects (buffs, heals, level-up) at no size while impacts still show. Imports fill them from the client's row of the same model (a renamed `Character\Race<id>\...` counts as its original), else effect scales of 1, and set the player-model flag 0x800 every stock playable model has (`FillPlayerModelRow`; older imports get **Fill its model rows for 3.3.5**). |
| M2 attachment lookup | 1.12 (converted) -> 3.3.5 | The client finds an attachment only through the model's id -> attachment lookup. Turtle's converted goblin keeps a lookup that stops at id 35 though it has 36-38 and 47-49, so spell effects at those points (buffs, heals, level-up) never show. Export rebuilds the lookup of every race model it copies to cover every id, at least the 50 stock characters have (`FixM2AttachmentLookup`). |
| `NameGen` | any -> 3.3.5 | A race with no random names gets the source client's names for it (2.0+ and Turtle share the layout), else its donor's. |
| Player display ids | any -> 3.3.5 + AzerothCore | AzerothCore keeps a player's display id in 16 bits (`PlayerInfo::displayId_m`), so a race display past 65535 shows another model in game (90000 -> 24464, an Ice Troll; 90001 -> a Wisp). Race imports take displays from `race.display.id` (60000-60999); older imports get a **Move its displays under 65536** button. |
| Race ids | any -> 3.3.5 | Past 21 the client's own race tables run out even with WXL's 64-race extension loaded (the body skin and the in-game model come out as another race's). Import new races onto ids 1-21: an unused NPC race row (9 Goblin, 12-21) |
| `CharSections` columns | 1.12 -> 3.3.5 | 1.12 puts variation and colour before the textures; 3.3.5 after the flags |
| `CharacterFacialHairStyles.Geoset` | 1.12 -> 3.3.5 | WoWDBDefs lists six; the first three are unused (`0xCCCCCCCC`), the real ones are the last three |
| `CharStartOutfit` | 1.12 -> 3.3.5 | 12 item slots instead of 24 |
| `ChrRaces` | 1.12 -> 3.3.5 | 1.12 has no `Alliance`, gendered names or `Required_expansion` (they read as 0 / empty) |
| M2 fields and flags | newer -> 3.3.5 | Listed in `Downport.cpp` (texture types, blend modes, sequence and bone flags, cameras, particles dropped) |

## Rules every conversion keeps

- The project's client is the base: tables and glue scripts come from the plain client, never from a pack stacked over
  it (`MpqChain::SetMapsFromLowestLayer`). Another client's data comes in through its own source chain and is converted.
- A source is read in its own layout, found from the file itself (record size / field count), never assumed.
- Add a check for every new version: import something from it and export it, as `--race-export-check` does for 1.12 and
  3.3.5.

## Adding a version

1. Its layouts, from WoWDBDefs (`definitions/<Table>.dbd`, the block whose `BUILD` covers it), named as 3.3.5 names them.
2. How the file tells which layout it is (record size, field count).
3. Its meaning rules: compare a table of it with the same table of 3.3.5 for a race or model both have, then add each
   difference to the table above and to the reader.
4. A check that imports from it.

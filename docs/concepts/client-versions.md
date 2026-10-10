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

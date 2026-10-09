# Races

[Back to the README](../../README.md) · View > Races

The races of every source: the project's client and each compare source (View > Sources). Each client's tables are
read in its own layout, 1.12 or 3.3.5. The window shows what each race is and what its characters can look like,
with a preview. Importing a race into the project comes later; for now the window only shows.

| Part | What it shows |
| --- | --- |
| Source (top left) | The client whose races are listed. A source made of patch MPQs without race tables says so. |
| List | Every row of the client's `ChrRaces`: id, name, team, skin colours male / female. **new** marks a race whose name the project's client does not have. |
| Details | File string, prefix, female and male names, faction template, base language, displays, the character model per sex, the classes it can be (`CharBaseInfo`), and how many skins, faces, hair styles, hair colours and facial hair it has per sex |
| Warnings | A file string another race of the project's client uses, or an id that is another race there: an import of the race will need its own |
| Preview | The character as the character creator shows it, naked, standing. The arrows step through skin, face, facial hair, hair style and hair colour. Left drag turns, right drag pans, the wheel zooms. |

Notes:

- To see another client's races, add its `Data` folder as a compare source.
- Some clients reuse unused race rows for new races. For example, a Void Elf in row 12 that still has Fel Orc's file string.
- `CharSections` often names textures the client does not have; the stock 3.3.5 client does too. The client skips
  them, and the details give their count.
- 1.12 clients (Turtle) are read in their own layout. Their models and textures need converting to 3.3.5
  (wow-upport) before the preview can show them: a source of the converted archives plus the client's tables.

Checks: `--race-check` and `--race-look` ([command-line checks](../development/checks.md)).

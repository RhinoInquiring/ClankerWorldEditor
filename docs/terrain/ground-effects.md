# Ground effects

[Back to the README](../../README.md) · Terrain

Ground effects are the grass, flowers and pebbles the client scatters over the terrain near the camera (detail
doodads). The editor shows them in the viewport and edits which ones grow where. They are client-only: the server
never reads them.

## How the client picks them

- Each texture layer of a chunk names one ground effect: a row of GroundEffectTexture.dbc with up to four models
  (GroundEffectDoodad.dbc, under `World\NoDXT\Detail\`), a weight for each and a density.
- Each chunk is split into 8 x 8 cells (4.2 yd). Per cell the chunk header stores which layer's effect grows there:
  the texture that covers most of the cell. On Blizzard's tiles this rule matches 96% of cells (`--ground-effect-check`).
- A second header field turns cells off entirely: nothing grows there, whatever the texture.

## Preview

**View > Ground effects** (also in every tool's View tab) shows them around the camera. The quality list matches the
client's graphics levels:

| Level | Per cell | Out to |
| --- | --- | --- |
| Low | density x 16 / 128 | 70 yd |
| Fair (default) | density x 40 / 128 | 110 yd |
| Medium | density x 64 / 128 | 160 yd |
| High | density x 96 / 128 | 200 yd |
| Ultra | density x 128 / 128 | 260 yd |

What grows in each cell is exact. The client's own scatter pattern is not known, so single blades sit elsewhere in game.

## Ground effects tool

**Terrain > Ground effects** has two brushes.

| Brush | Left drag | Ctrl+drag |
| --- | --- | --- |
| **Set a texture's effect** | Layers of the chosen texture in the chunks under the brush grow the chosen effect | They grow none |
| **Hide cells** | Cells under the brush grow nothing (red squares) | They grow their texture's effect again |

- **Alt+click** takes the texture and effect under the cursor.
- **Ctrl+wheel** sets the radius.
- **Tile under the camera** and **Every loaded tile** set the effect on every layer of that texture there, as one change.
- **Paint** lists the effects the chosen texture already carries on the loaded tiles, with chunk counts.
- **Effects** lists every effect that has models, searchable by model name or id; (n) is the density.

Each stroke is one undo step.

## Painting, pasting and export

- Painting textures moves the cells with the paint: a painted cell grows the new texture's effect. A new layer takes
  the effect the same texture has elsewhere on the loaded tiles.
- Copies, pastes, rotations and [blueprints](blueprints.md) carry the hidden cells. A blended paste takes them from
  whichever side the chunk mostly shows.
- Export writes the effect ids, the hidden cells and the cell map of every chunk whose layers changed.
- Chunks whose alpha did not change keep Blizzard's own cell map.
- Effect ids this client lacks are dropped on export (they crash the client), with a warning in Problems.

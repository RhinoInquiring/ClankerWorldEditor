# Ghost layers

[Back to the README](../../README.md) · Versions window

A ghost layer shows another version of the map over yours: tinted and see-through, with its objects as outline boxes
in the layer's colour. Versions come from the project's [sources](../concepts/sources.md).

## Versions of a tile

The Versions window lists every version of the tile under the camera: each archive or unpacked folder of every source
that holds it, so patch history shows too (the tile before and after each patch). Identical copies are merged into
one row. Each row shows the mean height difference from your map and its object count; tick a row to add it as a
ghost layer.

**Another map as ghost** shows a different map at the same coordinates, e.g. `Azeroth_Epoch` over `Azeroth` (map
modules ship reworked continents as maps of their own).

## Layers

| Control | Effect |
| --- | --- |
| Colour | Tint and opacity |
| Checkbox | Show or hide |
| Solo | Show only this version, with its full models, as the client would; picking uses its ground |
| Copy from | Ctrl+C copies from this layer: the copy is pinned where it stands in that version |
| Remove | Drop the layer |

Ghost tiles load in the background around the camera, like the map's own.

## See also

- [Compare](compare.md): flip a selected area through every version without setting up layers
- [Differences](differences.md): find every edit a whole other version makes

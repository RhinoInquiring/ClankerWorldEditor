# Blueprints

[Back to the README](../../README.md) · Terrain group (F1)

A blueprint is a saved copy: an area of terrain with its textures, holes, water and objects, a name, notes and a
picture, kept in the project for reuse on any map.

- **Save**: select chunks, then **Ctrl+B** (or **Edit > Save selection as blueprint**, or the button in the Copy tool).
  It is taken from the map, or from the layer chosen as **Copy from** in the Versions window.
- **Use**: Catalog > **Blueprints** (Terrain group). Click a card to place it following the cursor, exactly like a
  copy ([Copy and paste](copy-paste.md)). Right-click: place it where it was taken from, or delete it.
- **Search** by name and notes; the size slider scales the pictures.

Each blueprint is one JSON file in `<project>/blueprints/`, heights stored exactly. Blueprints saved before water was
copied leave the target's water alone when placed.

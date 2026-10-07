# Server data: maps, vmaps, mmaps

[Back to the README](../../README.md) · File > Build server data (or Ctrl+P "Server: build server data")

The worldserver reads its own copy of the world: terrain heights, water and areas (`maps`), buildings and models for line
of sight and collision (`vmaps`) and the navigation mesh creatures walk on (`mmaps`). After terrain or object edits
those go stale: creatures float, fall through new ground or walk through moved buildings. This rebuilds them, only for
the maps the project exports.

## What it does

For each map the project changes (terrain, objects, water, roads):

1. **Stages** a client that holds only that map: its final tiles (the project's export where edited, the client's
   elsewhere), every building and model they place, and the few DBC files the tools read.
2. Runs **AzerothCore's own tools** from the server folder on it: `map_extractor`, `vmap4_extractor`,
   `vmap4_assembler`, then `mmaps_generator` for the edited tiles and their neighbours (or every tile, with
   *Rebuild the whole navmesh*). The tools see only that map, so a continent takes under a minute, not the hours a
   whole-client extraction does.
3. **Installs** only that map's files into the server's `Data` (`maps\<id>*.map`, `vmaps\<id>*` and the models they
   use, `mmaps\<id>*`). Every file it replaces is saved first, once, in the project's `server-build\original`;
   **Restore the server's originals** puts them back.

Restart the worldserver to load the new data.

## Notes

- Set the server folder in File > Server setup (the folder with `worldserver.exe` and the map tools).
- The build exports the project first; a failed step keeps `server-build\work` with each tool's log.
- Gameobject collision (`vmaps\temp_gameobject_models`) is never touched: the staged client has no gameobjects.
- Output matches AzerothCore's full extraction: checked on Deadmines and all of Kalimdor (every `.map` byte for byte,
  every `.vmtile` spawn for spawn; the extractor numbers model instances by the order it reads them, so only those
  numbers differ, consistently within the map).

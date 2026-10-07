# Sound and weather

[Back to the README](../../README.md) · Atmosphere group (F6) · tool key **M**

## Zone sound

The tab shows the area under the camera and, for a sub-area, its zone; choose which one to edit. Each area points at:

| Field | Table | What plays |
| --- | --- | --- |
| Ambience | SoundAmbience | A loop for day and one for night (wind, birds, crowds) |
| Music | ZoneMusic | A day and a night piece, with silences between them |
| Intro | ZoneIntroMusicTable | One piece, on entering |

Pick from the client's sets; **Play** previews a set's day or night sound (its first file), **Stop playing** stops it.
None on a sub-area means the client plays the zone's. Rooms of buildings (WMOAreaTable) have their own sound fields;
they are not edited here yet.

## Weather

The server rolls weather per zone from `game_weather`: for each season, the chance of rain, snow and sandstorm (the rest
is clear). The tab shows the zone under the camera; **Give this zone weather** adds its row, the sliders set the
chances (a season's total turns red past 100%), **Remove** takes the row out. Seasons follow the server's calendar,
spring from March 20.

Needs the world database (File > Server setup); changes write live, with undo. The worldserver reads `game_weather`
only at start: restart it, then try a zone in game with `.wchange 1 0.5` (1 rain, 2 snow, 3 sandstorm; grade 0 to 1).
`.wchange` only works in zones that had weather when the server started.

## Emitters

Sounds placed in the world (SoundEmitters.dbc): a gate creaking, a fog horn. Pick the sound new emitters play (a
searchable list of every SoundEntries row, each with **Play**), then **New emitter** and a click on the ground
(**Shift** keeps adding). Emitters move with the shared handles (**G** to the ground, **Del** deletes, **Alt+click**
moves it there); double-click one in the viewport to hear it. Selected, it shows how far it is heard.

New emitters take ids from the project's `soundemitter.id` range. Northrend also places sounds inside its terrain tiles
(MCSE); those are not shown or edited.

Export writes AreaTable, SoundAmbience, ZoneMusic, ZoneIntroMusicTable and SoundEmitters into the client patch; restart
the client to hear them in game.

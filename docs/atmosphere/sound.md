# Sound

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

## Emitters

Sounds placed in the world (SoundEmitters.dbc): a gate creaking, a fog horn. Pick the sound new emitters play (a
searchable list of every SoundEntries row, each with **Play**), then **New emitter** and a click on the ground
(**Shift** keeps adding). Emitters move with the shared handles (**G** to the ground, **Del** deletes, **Alt+click**
moves it there); double-click one in the viewport to hear it. Selected, it shows how far it is heard.

New emitters take ids from the project's `soundemitter.id` range. Northrend also places sounds inside its terrain tiles
(MCSE); those are not shown or edited.

Export writes AreaTable, SoundAmbience, ZoneMusic, ZoneIntroMusicTable and SoundEmitters into the client patch; restart
the client to hear them in game.

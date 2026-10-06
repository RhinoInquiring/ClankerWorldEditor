# Lights

[Back to the README](../../README.md) · Atmosphere group (F6) · tool key **L**

The sky, fog and the colour of sunlight come from light volumes (Light.dbc). Every map has a default light that covers
it everywhere; local lights are spheres: inside the inner radius they are all there is, between the inner and outer
radius they blend into the light around them.

## Game lighting

**View > Game lighting** (or the Preview tab) shows the light where the camera stands: sky gradient, fog, sunlight
and ambient colour on terrain and models. The **Time** slider picks the time of day; **Weather** picks which of a
light's sets shows (clear, storm, under water, dead). Not drawn in the editor: the skybox model, clouds, the sun and
moon, and water colours.

## Lights tab

Every light of the map, nearest first, the map's default at the top. Click selects, double-click flies there.
**New light**, then a click on the ground, adds one (**Shift** keeps adding). A new light starts as the place looks
now: it copies the colour set of the strongest light there, so its colours are its own from the start.

## Selected

A light moves with the shared handles (**1** move, **3** scale both radii, **G** to the ground, **Del** deletes,
**Alt+click** moves it there); the radii also have sliders. The map's default light has no place or radius.

A light points at up to eight colour sets (LightParams), one per weather. Sets are often shared: the panel says by how
many lights. Editing a shared set changes all of them; **Give this light its own copy** makes a private one first
(ids from the project's `lightparams.id` range; new lights from `light.id`).

## Colours and fog

Each colour and fog value is a list of keys over the day (up to 16), and the client blends between them. Editing a
value sets it at the current time: the key within 15 minutes changes, otherwise a new key goes in. Right-click a
colour to see its keys, jump to one, remove the key at this time, or keep one colour all day.

| Colour | What it lights |
| --- | --- |
| Diffuse | Sunlight on the ground and models |
| Ambient | Light from everywhere, the shadow side |
| Sky top ... Sky at horizon | The sky gradient, from overhead down |
| Fog and far mountains | Fog colour; also tints weather effects |
| Sun and halo, cloud colours | The sun and the cloud layers |
| Ocean / River shallow, deep | Water colours |

Fog distance is where everything disappears; fog start is where it begins, as a share of that distance. The whole set
also holds the skybox model and how see-through rivers and oceans are.

Export writes the five tables (Light, LightParams, LightIntBand, LightFloatBand, LightSkybox) into the client patch;
restart the client to see them in game.

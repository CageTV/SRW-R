# Savage Requiem World (Requiem build), 0.1.0 pre-release

The same animal and creature AI, factions, NPCs, survival foods, wild horse herds and spawn points as the non-Requiem build (https://github.com/CageTV/SRW), with the Requiem build's numbers (skills, health, AI, level and speed of 231 NPCs and 2 races) in the one plugin, and the Creature Behaviour SKSE plugin with a Requiem layer.

## What is in this repository
- `Savage Requiem World.esp` and `Savage Requiem World_LLOS.ini`: the plugin with Requiem's numbers. Requiem is not a master: with Requiem.esp loaded, the DLL gives the races and creatures Requiem's keywords (blood, sneak-attack and knockdown immunity, ...), race spells (poison resistance, fire weakness, ...), the armor penetration and other perks, and adds Requiem's own creatures to the creature lists. Without Requiem the plugin works as the non-Requiem build.
- `SKSE/Plugins/`: Creature Behaviour 0.2.0 (SKSE DLL, GPL-3.0), its rule file `enforce.json` and the spawn points.
- `Optional/`: the Wolf Pups add-on.
- `Source/CreatureBehaviour/`: source of the DLL (GPL-3.0).

## Requirements
- Skyrim Special Edition or Anniversary Edition 1.6.1170 or newer (including 1.7.x), SKSE64, Address Library for SKSE Plugins. The DLL is the new build only; there is no build for older Skyrim versions or Skyrim VR.
- The official masters (Skyrim, Update, Dawnguard, Hearthfires, Dragonborn). No Creation Club file, no Unofficial Patch, no other mod is a master.
- `Savage Requiem World.bsa` next to the plugin (the plugin and the archive share the name). It is not in this repository (1.69 GB).
- Savage Skyrim: the bear, wolf and fox coat variants use meshes that come with it. Without it those variants have no mesh.
- A new game: the horse herds moved, so an older save keeps the old herd positions.
- Requiem 6.x for the Requiem layer (optional).

## Optional
- `Optional/Savage World - Wolf Pups.esp`: no-stagger attack data for 36 young animals. Needs SkyTEST, Animallica, Savage Skyrim Std and Skyrim Immersive Creatures SE as masters. The base plugin does not need it.
- The lists the old `Savage World - Creature Lists.esp` made (creatures of Animallica, Skyrim Immersive Creatures, SkyTEST and SRW added to the vanilla creature lists) are now rules in the DLL: they apply to the mods you have and skip the others. Do not use the old plugin with this version.

## Licence and credits
The DLL and its source are GPL-3.0 (see `SKSE/Plugins/CreatureBehaviour-LICENSE.txt`). See `CREDITS.txt`.

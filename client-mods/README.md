# Client Mods

Third-party client mods distributed to players through the launcher manifest.

The mod binaries themselves are not kept in this repository: they are not our code, and the launcher
fetches them from the central `updates/` folder along with everything else it syncs. What lives here
is the configuration we ship as the default, so the settings players start with are version
controlled and reviewable.

A mod is distributed by placing its files in the portal's `updates/` folder. The portal rebuilds the
manifest from that folder's contents, and the launcher downloads anything whose hash does not match
on the next run. No launcher or portal code change is required to add one.

## Active Mods

The following mods are currently delivered to players via the update manifest:

| Mod | Distributed File | Target Location | Description |
|---|---|---|---|
| **Ground Tooltip** | `groundtooltip_loa.dsdll`, `groundtooltip.ini` | Root (`DSLOA.exe` folder) | Shows item tooltips when hovering over ground item labels. |
| **BuffedBuffs** | `BuffedBuffsMod.dsres` | `DSLOA/` | Extends buff durations to 30 minutes and improves orb spell replacement. |
| **Unlocked Loot** | `Unlocked Loot.dsres` | `DSLOA/` | Restores 80+ cut and bugged weapons and armor pieces to monster drop tables. |
| **Difficulty Patch** | `DS1_Difficulty_Patch.dsres` | `DSLOA/` | Adjusts multiplayer encounter scaling and difficulty progression. |
| **UberUI** | `UberUI_LoA_v0.02.dsres` | `DSLOA/` | Quality-of-life interface tweaks and layout improvements for Legends of Aranna. |
| **Fairy Companion Fix** | `fairyfix.dsres` | `Resources/` | Fixes pathing and state issues for fairy companion summons. |
| **SeeFar ResolutionFix** | `sf_ResolutionFix.dsres` | `Resources/` | Corrects paperdoll positions and interface scaling at high modern resolutions. |

## Ground Tooltip 1.1.2

Shows the normal item tooltip when the mouse is over an item label on the ground.

| | |
|---|---|
| Files | `groundtooltip_loa.dsdll`, `groundtooltip.ini` |
| Installed to | the game folder, beside `DSLOA.exe` |
| Requires | Legends of Aranna 1.50 (the standalone 1.11 executables are not supported) |
| Toggle | numpad `*` cycles always on, inventory only, off |
| Per-player settings | `%LOCALAPPDATA%\GroundTooltip` |

`groundtooltip.ini` in the game folder is the shipped default and is read once; the mod does not
write back to it, so the launcher will not re-download it on every run. Per-player changes are made
in the copy under `%LOCALAPPDATA%\GroundTooltip`.

The distributed archive contains two builds of the DLL. The one at the archive root is the correct
one for this deployment; the build under `DSLOA Mod\` is for installs driven by `DSLOAMod.exe`, which
this stack does not use.

Verified alongside SiegeHost 1.3.0: the mod loads, a client joins and plays normally, and no
SiegeHost feature is affected.

## BuffedBuffs (Spell Duration Rebalance)

Extends Nature Magic and Combat Magic buff durations and adjusts orb spell replacement behavior.

| | |
|---|---|
| Files | `BuffedBuffsMod.dsres` (distributed binary), sources in `buffedbuffs/` |
| Installed to | `DSLOA/` |
| Requires | Legends of Aranna 1.50 |
| Scope | Spell duration rebalance and orb replacement logic |

Replaces default short spell buff durations (typically 30–60 seconds) with extended durations to reduce repetitive buff recasting during exploration and multiplayer sessions.

The mod includes:
- `spell_orb.skrit`: Component script ensuring newly cast orb spells replace active ones.
- `spl_spell.gas` & `dsx_spl_spell.gas`: Spell template definitions updating duration parameters across base game and expansion buff spells.

The compiled `.dsres` archive is distributed to players via the launcher update manifest. Source scripts and templates are preserved in `buffedbuffs/` for review and maintenance.

## Unlocked Loot

Restores unused, cut, and bugged items into the active game drop tables.

| | |
|---|---|
| Files | `Unlocked Loot.dsres` |
| Installed to | `DSLOA/` |
| Requires | Legends of Aranna 1.50 |
| Scope | Drop table and pcontent template assignments |

Restores over 80 weapons, armor pieces, shields, and jewelry items that existed in the game assets but were never assigned to drop containers or enemy loot lists in retail Legends of Aranna. 

The mod integrates these items seamlessly into the world's standard progression curves, allowing them to drop naturally from monsters, chests, and breakables in both single-player and multiplayer sessions without altering base weapon damage formulas.

## DS1 Difficulty Patch

Rebalances multiplayer monster scaling and party difficulty curves.

| | |
|---|---|
| Files | `DS1_Difficulty_Patch.dsres` |
| Installed to | `DSLOA/` |
| Requires | Legends of Aranna 1.50 |
| Scope | Multiplayer world monster health, damage, and armor formulas |

Vanilla Dungeon Siege multiplayer scaling increases monster health and damage aggressively as additional players join, frequently causing sharp difficulty spikes or turning encounters into tedious damage sponges.

This patch smooths the scaling curves, ensuring multiplayer encounters remain challenging yet fair for varying group sizes and solo adventurers exploring multiplayer maps.

## UberUI (Legends of Aranna)

Interface enhancements and quality-of-life UI improvements.

| | |
|---|---|
| Files | `UberUI_LoA_v0.02.dsres` |
| Installed to | `DSLOA/` |
| Requires | Legends of Aranna 1.50 |
| Scope | In-game HUD, party status panels, and UI layout definitions |

Provides interface refinements specifically tailored for Legends of Aranna:
- Enhanced health and mana bar visibility with clearer text readouts.
- Improved party member status frame alignment and buff icon positioning.
- Cleaner inventory grid rendering and compact spellbook slot borders.

## Fairy Companion Fix

Resolves pathing and animation state freezes for summoned fairy companions.

| | |
|---|---|
| Files | `fairyfix.dsres` |
| Installed to | `Resources/` |
| Requires | Legends of Aranna 1.50 |
| Scope | Fairy actor templates and companion AI state logic |

In retail Dungeon Siege, Nature Magic fairy summons could freeze permanently in mid-air, enter an unrecoverable animation loop, or crash the game client when transitioning between world cells or teleporting.

This fix patches the fairy actor state machine so summoned fairies follow the caster reliably across elevator transitions, regional boundaries, and town portals.

## SeeFar ResolutionFix

Corrects widescreen interface scaling and paperdoll alignment at modern display resolutions.

| | |
|---|---|
| Files | `sf_ResolutionFix.dsres` |
| Installed to | `Resources/` |
| Requires | Legends of Aranna 1.50 |
| Scope | UI configuration and inventory paperdoll coordinates |

The retail game interface was built for 4:3 resolutions (up to 1024x768). Running at modern 16:9 or 16:10 resolutions (such as 1920x1080 or 2560x1440) causes paperdoll alignment offsets and inventory screen placement bugs.

`sf_ResolutionFix` (part of the SeeFar mod suite) corrects the paperdoll anchor coordinates and layout parameters across all playable character races, ensuring inventory screens render properly at high widescreen resolutions.


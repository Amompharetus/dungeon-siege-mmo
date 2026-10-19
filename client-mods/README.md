# Client Mods

Third-party client mods distributed to players through the launcher manifest.

The mod binaries themselves are not kept in this repository: they are not our code, and the launcher
fetches them from the central `updates/` folder along with everything else it syncs. What lives here
is the configuration we ship as the default, so the settings players start with are version
controlled and reviewable.

A mod is distributed by placing its files in the portal's `updates/` folder. The portal rebuilds the
manifest from that folder's contents, and the launcher downloads anything whose hash does not match
on the next run. No launcher or portal code change is required to add one.

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
| Installed to | `DSLOA/` or `Resources/` |
| Requires | Legends of Aranna 1.50 |
| Scope | Spell duration rebalance and orb replacement logic |

Replaces default short spell buff durations (typically 30–60 seconds) with extended durations to reduce repetitive buff recasting during exploration and multiplayer sessions.

The mod includes:
- `spell_orb.skrit`: Component script ensuring newly cast orb spells replace active ones.
- `spl_spell.gas` & `dsx_spl_spell.gas`: Spell template definitions updating duration parameters across base game and expansion buff spells.

The compiled `.dsres` archive is distributed to players via the launcher update manifest. Source scripts and templates are preserved in `buffedbuffs/` for review and maintenance.


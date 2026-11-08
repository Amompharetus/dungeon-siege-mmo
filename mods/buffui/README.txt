--------------------------
Mod:     BufUI / BufUI_LoA
Version: 3.00
Status:  Stable
Author:  Lokiswaq (original)
Update:  Sabedig
Date:    16-Jan-2026
--------------------------
Essential info
--------------------------

1. INTRODUCTION

BufUI is a GUI enhancement mod for Dungeon Siege that displays a character’s active buffs in a clear, readable panel. Originally created as a learning exercise in skrit scripting, it evolved into a fully featured and widely used UI mod.

Version 3.00 is a community-driven update that modernizes buff visibility and usability while preserving the original design philosophy.

2. FUNCTIONALITY

BufUI adds a panel to the GUI that shows the selected character’s active buffs, their count, and their remaining duration.

Buff Display Improvements (v3.00)

- Visible buff icons increased from 12 to 24
- Buff icon slots now dynamically expand:
  - First 12 buffs are shown by default
  - An additional row of 12 appears automatically when more buffs are active
  - Extra slots hide again when no longer needed to preserve screen space
- Buff icons are now sorted by remaining duration
  - Buffs expiring soonest are shown first for better combat awareness

Existing Features

- Two panel views:
  - Icon view (default)
  - Text view (shows buff descriptions)
- Toggle views via the extra GUI button next to the health/mana potions
- Remaining buff duration shown as:
  - h:mm:ss in text view
  - mm:ss in icon view
- Timers turn red when 10 seconds or less remain
- Buffs exceeding 100 minutes show as "99:++" in icon view

Note: Version 3.00 changes apply to the icon-based display only. The text-based view remains unchanged.

3. INSTRUCTIONS

Place the appropriate .dsres file into the Resources folder of your Dungeon Siege installation:

- BufUI_v3.00.dsres for Dungeon Siege
- BufUI_LoA_v3.00.dsres for Legends of Aranna

Do not use both at the same time.

In-game controls:
- Left-click the white spellbook button to show/hide the panel
- Right-click the button to toggle icon/text view

Hotkeys

You can assign a hotkey under:
Options → Input → Hotkeys → User Interface

- Default primary hotkey: ;
- Assign SHIFT-; as the secondary hotkey to toggle BufUI together with the game timer

4. COMPATIBILITY

BufUI has been tested with:
- Dungeon Siege v1.11
- Dungeon Siege: Legends of Aranna v1.50
- Single-player and multiplayer

BufUI does not alter hero templates and is compatible with most mods. However, it remains incompatible with mods that modify data_bar.gas or spell-related skrit files (e.g. spell_status_effect.skrit). See FILES.txt for details.

BufUI works correctly with restored save games created without BufUI installed.

5. BONUS

BufUI fixes a bug in the original spell_mass_enchant.skrit that caused incorrect behavior when stackable spells were applied in a specific order.

6. LICENSE

You may examine the code freely. If you wish to include BufUI or parts of it in another mod, please ask permission first.

Redistribution is allowed provided the archive and its contents remain unchanged.

7. ACKNOWLEDGEMENTS

Original author and concept:
- Greg "Lokiswaq" Papangeles

Community update (v3.00):
- Sabedig – dynamic buff expansion and duration-based sorting

Special thanks to:
- Jim "Xaa" Farris
- Kaikopere (Team Elemental)
- Jason "Unkle Ernsie" Ernsdorff
- Jason "Ikkyo" Gripp
- Monsoon
- TomCat39

Enjoy :)

Greg "Lokiswaq" Papangeles
Community update maintained by xaeroneo

# BuffUI (Active Buffs Monitor)

A graphical user interface enhancement for Dungeon Siege: Legends of Aranna that displays the player's active spell buffs, potion enhancements, and custom MMO blessings in a clean, readable on-screen panel.

---

## Overview

Originally created by **Greg "Lokiswaq" Papangeles** and updated to **v3.00** by **Sabedig**, BuffUI provides critical combat awareness by surfacing active magical enhancements with live countdown timers and duration-based sorting.

In this MMO deployment, BuffUI has been further upgraded with a **Native C++ Engine** integrated directly into SiegeHost (`characterabilities.cpp`), eliminating the legacy 2004 Skrit hacks that previously modified dozens of spell templates and generated network traffic.

| | |
|---|---|
| **Files** | `BufUI_LoA_v3.00.dsres` (standalone) or integrated within `Z_CharacterAbilities_UI.dsres` |
| **Installed to** | `DSLOA/` |
| **Requires** | Legends of Aranna 1.50 |
| **Scope** | Client-Only UI & Native SiegeHost Memory Driver |
| **Primary Hotkey** | `Shift + ;` (single-press toggle) |
| **Data Bar Button** | White Spellbook icon (`b_gui_ig_i_it_magic-book-17`) on bottom dockbar |

---

## Key Features

### 1. Dynamic 12 to 24 Buff Slot Expansion (v3.00)
- The panel displays **12 buff slots** by default to minimize screen clutter.
- If more than 12 buffs are active simultaneously, a second row of 12 slots automatically expands into view (up to 24 active buffs).
- Extra slots automatically collapse once active buffs expire or are removed.

### 2. Duration-Based Sorting
- Active buffs are sorted dynamically by remaining duration.
- Buffs expiring soonest are displayed first, allowing players to instantly identify expiring protections during intense battles.

### 3. Dual Panel Views
- **Icon View (Default):** Compact graphical view showing spell icons with `mm:ss` countdown timers.
  - Timers switch to **red text** when 10 seconds or less remain.
  - Buffs exceeding 100 minutes (such as extended MMO blessings) display as `99:++`.
- **Text View:** Expanded textual list displaying full spell names and formatted `h:mm:ss` remaining durations.
- **View Toggle:** Right-click the white spellbook button on the data bar to toggle between Icon and Text views.

### 4. Native SiegeHost C++ Engine
- **Zero Skrit File Overrides:** Completely eliminates the need to override `spell_status_effect.skrit`, `spell_orb.skrit`, or individual spell templates. `Buffed Spells.dsres` and other balance mods remain 100% separate and untangled.
- **Zero Network Overhead:** Eliminates network synchronization messages (`GameAuditor.RCSet`), preventing network spam in multiplayer.
- **Custom MMO Buff Support:** The native engine hooks custom abilities like **Aegis of Renewal** (`+armor` party blessing) and displays them seamlessly alongside standard spell buffs.
- **Accurate Character Name:** Resolves and displays the active hero's actual character name in the panel title.
- **Native Audio Feedback:** Plays the standard UI element sound effect (`s_e_gui_element_small`) on open and close.
- **Single-Press Hotkey:** The `Shift + ;` shortcut responds on a single press without requiring double-tap inputs.

---

## In-Game Controls & Hotkeys

| Action | Control | Description |
|---|---|---|
| **Toggle Panel** | Left-Click Data Bar Button | Click the white spellbook icon on the bottom data bar |
| **Toggle Panel** | `Shift + ;` | Single-press keyboard shortcut |
| **Switch View** | Right-Click Data Bar Button | Toggles between Icon View and Text List View |

---

## Directory Structure

```text
buffui/
├── README.md                                  # This documentation
├── CHANGELOG.txt                              # Version history (v0.97 - v3.00)
├── README.txt                                 # Upstream release documentation
├── build_buffui_tank.py                       # Standalone .dsres compiler script
├── art/                                       # UI panel textures and frames
│   └── bitmaps/gui/in_game/menus/
│       ├── b_gui_ig_mnu_bufui.gas
│       ├── b_gui_ig_mnu_bufui.raw
│       ├── b_gui_ig_mnu_bufui_empty.gas
│       └── b_gui_ig_mnu_bufui_empty.raw
├── ui/                                        # Layout configurations
│   ├── interfaces/backend/buffui/
│   │   └── buffui.gas                         # Clean UI dialog with 24 buff slots
│   ├── interfaces/backend/data_bar/
│   │   └── data_bar_buffui_button.gas         # Data bar toggle button definition
│   └── user_education_tooltips.gas            # Button rollover help string
└── legacy_skrit/                              # Original Skrit implementation (reference)
    ├── lokiswaq_bufui.gas
    ├── lokiswaq_bufui_manager.skrit
    ├── lokiswaq_bufui_manager.gas
    ├── k_inc_spl_lokiswaq_bufui.skrit
    └── trs_book_spell2.gas
```

---

## Building the Tank

To compile a standalone `BufUI_LoA_v3.00.dsres` tank:

```bash
python build_buffui_tank.py
```

*Note: In the live game distribution, BuffUI assets are packaged into `Z_CharacterAbilities_UI.dsres` alongside the top-left Character Abilities HUD and Shared Stash controls.*

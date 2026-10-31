# Character Abilities & Shared Stash UI (`Z_CharacterAbilities_UI.dsres`)

Provides the top-left HUD action dock for Legends of Aranna multiplayer:
- Docks 4 ability action slots ([Q] Whirling Cleave, [W] Shrapnel Blast, [E] Aegis of Renewal, [R] Flame Nova) directly beside the weapon active slots panel in the top-left HUD.
- Includes active numeric cooldown timers and hover highlight frames.
- Integrates the Shared Stash button (`awp_stash_btn_1`) using the Golden Star Key icon (`b_gui_ig_i_it_key-star`), replacing the duplicate collect loot icon.
- Features native golden illuminated hover highlight frames for both ability buttons and the shared stash button matching the weapon slots dock.
- Injects tooltip descriptions into `user_education_tooltips.gas` for automatic rollover feedback.

## Building the Tank

Run `python build_ability_ui_tank.py` to compile `character_awp.gas` and `user_education_tooltips.gas` into `Z_CharacterAbilities_UI.dsres`.

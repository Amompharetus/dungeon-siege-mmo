# build_ability_ui_tank.py
import sys, os
sys.path.append(r'Dungeon Siege 1\03-analysis\tools')
import tank_read, tank_build

def validate_gas(gas_text, name):
    depth = 0
    for i, line in enumerate(gas_text.splitlines()):
        clean = line.split('//')[0]
        for ch in clean:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth < 0:
                    raise ValueError(f"Extra closing brace in {name} at line {i+1}")
    if depth != 0:
        raise ValueError(f"Unclosed block in {name}, final depth {depth}")
    print(f"{name} GAS validation PASSED (perfectly balanced).")

# 1. Base character_awp from UberUI
uber_path = r'Dungeon Siege 1\01-client\Dungeon Siege 1\DSLOA\UberUI_LoA_v0.02.dsres'
if os.path.exists(uber_path):
    t_uber = tank_read.load(uber_path)
    awp_text = t_uber['ui/interfaces/backend/character_awp/character_awp.gas'].decode('latin1')
    print("Loaded pristine character_awp.gas from UberUI_LoA_v0.02.dsres")
else:
    awp_path = r'c:\Users\Amompharetus\Desktop\Dungeon Siege\extracted_awp.gas'
    awp_text = open(awp_path, 'r', encoding='latin1').read()
    print("Loaded character_awp.gas from extracted_awp.gas")

option1_gas = """
  [t:window,n:awp_ability_subpanel_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    consumable = true;
    draw_order = 27;
    rect = 148,3,232,40;
    texture = b_gui_ig_mnu_awp;
    uvcoords = 0.250000,0.710938,0.578125,1.000000;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:window,n:awp_ability_icon_0]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 829;
    rect = 152,6,168,25;
    texture = b_gui_ig_mnu_combat;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    visible = true;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:button,n:awp_ability_btn_0]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    consumable = true;
    draw_order = 850;
    rect = 152,6,168,38;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
    rollover_help = awp_ability_0;
    [messages]
    {
      onbuttonpress = notify(use_ability_0);
      onrollover = notify(ability_rollover_0);
      onrolloff = notify(ability_rolloff_0);
    }
  }
  [t:window,n:awp_ability_hover_0]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 6629;
    rect = 152,6,168,38;
    texture = b_gui_ig_mnu_awp;
    uvcoords = 0.757813,0.710938,0.835938,0.992188;
    visible = false;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_key_0]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 835;
    rect = 152,26,168,38;
    text = "Q";
    font_color = 0xffcccccc;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_cd_0]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 840;
    rect = 146,8,174,23;
    text = "";
    font_color = 0xffffffff;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:window,n:awp_ability_icon_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 829;
    rect = 172,6,188,25;
    texture = b_gui_ig_mnu_ranged;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    visible = true;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:button,n:awp_ability_btn_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    consumable = true;
    draw_order = 850;
    rect = 172,6,188,38;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
    rollover_help = awp_ability_1;
    [messages]
    {
      onbuttonpress = notify(use_ability_1);
      onrollover = notify(ability_rollover_1);
      onrolloff = notify(ability_rolloff_1);
    }
  }
  [t:window,n:awp_ability_hover_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 6629;
    rect = 172,6,188,38;
    texture = b_gui_ig_mnu_awp;
    uvcoords = 0.757813,0.710938,0.835938,0.992188;
    visible = false;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_key_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 835;
    rect = 172,26,188,38;
    text = "W";
    font_color = 0xffcccccc;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_cd_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 840;
    rect = 166,8,194,23;
    text = "";
    font_color = 0xffffffff;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:window,n:awp_ability_icon_2]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 829;
    rect = 192,6,208,25;
    texture = b_gui_ig_mnu_nature-magic;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    visible = true;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:button,n:awp_ability_btn_2]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    consumable = true;
    draw_order = 850;
    rect = 192,6,208,38;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
    rollover_help = awp_ability_2;
    [messages]
    {
      onbuttonpress = notify(use_ability_2);
      onrollover = notify(ability_rollover_2);
      onrolloff = notify(ability_rolloff_2);
    }
  }
  [t:window,n:awp_ability_hover_2]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 6629;
    rect = 192,6,208,38;
    texture = b_gui_ig_mnu_awp;
    uvcoords = 0.757813,0.710938,0.835938,0.992188;
    visible = false;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_key_2]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 835;
    rect = 192,26,208,38;
    text = "E";
    font_color = 0xffcccccc;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_cd_2]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 840;
    rect = 186,8,214,23;
    text = "";
    font_color = 0xffffffff;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:window,n:awp_ability_icon_3]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 829;
    rect = 212,6,228,25;
    texture = b_gui_ig_mnu_combat-magic;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    visible = true;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:button,n:awp_ability_btn_3]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    consumable = true;
    draw_order = 850;
    rect = 212,6,228,38;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
    rollover_help = awp_ability_3;
    [messages]
    {
      onbuttonpress = notify(use_ability_3);
      onrollover = notify(ability_rollover_3);
      onrolloff = notify(ability_rolloff_3);
    }
  }
  [t:window,n:awp_ability_hover_3]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 6629;
    rect = 212,6,228,38;
    texture = b_gui_ig_mnu_awp;
    uvcoords = 0.757813,0.710938,0.835938,0.992188;
    visible = false;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_key_3]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 835;
    rect = 212,26,228,38;
    text = "R";
    font_color = 0xffcccccc;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:text,n:awp_ability_cd_3]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 840;
    rect = 206,8,234,23;
    text = "";
    font_color = 0xffffffff;
    font_size = 0;
    font_type = b_gui_fnt_12p_copperplate-light;
    justify = center;
    pass_through = true;
    visible = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
  }
  [t:button,n:awp_stash_btn_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    consumable = true;
    draw_order = 829;
    rect = 238,6,270,26;
    texture = b_gui_ig_i_it_gold-pile-lg;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
    rollover_help = awp_shared_stash;
    [messages]
    {
      onbuttonpress = notify(open_shared_stash);
    }
  }
"""

marker = "[t:window,n:window_slots_panel_1]"
idx = awp_text.find(marker)
if idx != -1:
    end_bracket = awp_text.find("}", idx)
    awp_text = awp_text[:end_bracket+1] + "\n" + option1_gas + awp_text[end_bracket+1:]
    print("Option 1 added to character_awp.gas successfully.")
else:
    raise RuntimeError("ERROR: could not find window_slots_panel_1 in character_awp.gas")

# 2. Load base user_education_tooltips.gas from Expansion.dsres and add [gui_rollover] entries
exp_path = r'Dungeon Siege 1\01-client\Dungeon Siege 1\DSLOA\Expansion.dsres'
t_exp = tank_read.load(exp_path)
uet_text = t_exp['ui/user_education_tooltips.gas'].decode('latin1')

rollover_additions = (
    '\r\n'
    '\t\t// Character Abilities & Shared Stash\r\n'
    '\t\tawp_ability_0 = "[Q] Whirling Cleave\\nPress Q to use Whirling Cleave";\r\n'
    '\t\tawp_ability_1 = "[W] Shrapnel Blast\\nPress W to use Shrapnel Blast";\r\n'
    '\t\tawp_ability_2 = "[E] Aegis of Renewal\\nPress E to use Aegis of Renewal";\r\n'
    '\t\tawp_ability_3 = "[R] Flame Nova\\nPress R to use Flame Nova";\r\n'
    '\t\tawp_shared_stash = "Shared Stash\\nClick or type /stash to open Shared Stash";\r\n'
)

anchor = 'button_auto_sell_list = "Click and hold to choose from a list of auto sell options.";'
anchor_idx = uet_text.find(anchor)
if anchor_idx == -1:
    raise RuntimeError("ERROR: could not find button_auto_sell_list anchor in user_education_tooltips.gas")

insert_pos = anchor_idx + len(anchor)
uet_text = uet_text[:insert_pos] + rollover_additions + uet_text[insert_pos:]
print("Added Character Abilities & Shared Stash rollover help entries to user_education_tooltips.gas.")

# 3. Validate GAS syntax
validate_gas(awp_text, "character_awp.gas")
validate_gas(uet_text, "user_education_tooltips.gas")

# 4. Build tank
out_tank = r'Dungeon Siege 1\01-client\Dungeon Siege 1\DSLOA\Z_CharacterAbilities_UI.dsres'
files = {
    '/ui/interfaces/backend/character_awp/character_awp.gas': awp_text.encode('latin1'),
    '/ui/user_education_tooltips.gas': uet_text.encode('latin1'),
}
nfiles, nbytes = tank_build.build_tank(
    files,
    out_tank,
    title="Character Abilities UI (Top-Left AWP Sub-bar)",
    author="Antigravity",
    description="Character Abilities UI in Top-Left HUD beside weapon slots with Shared Stash button and rollover info"
)
print(f"Tank built: {out_tank} ({nfiles} files, {nbytes} bytes)")

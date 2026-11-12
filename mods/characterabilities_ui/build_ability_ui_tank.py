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
    awp_path = os.environ.get('EXTRACTED_AWP_PATH', 'extracted_awp.gas')
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
    rect = 238,6,270,38;
    texture = b_gui_ig_i_it_key-star;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    visible = true;
    wrap_mode = clamp;
    group = character_1_max;
    dock_group = dockbar_character_1;
    rollover_help = awp_shared_stash;
    [messages]
    {
      onbuttonpress = notify(open_shared_stash);
      onrollover = notify(stash_rollover);
      onrolloff = notify(stash_rolloff);
    }
  }
  [t:window,n:awp_stash_hover_1]
  {
    alpha = 1.000000;
    common_control = false;
    common_template = ;
    draw_order = 6629;
    rect = 238,6,270,38;
    texture = b_gui_ig_mnu_awp;
    uvcoords = 0.757813,0.710938,0.835938,0.992188;
    visible = false;
    wrap_mode = clamp;
    pass_through = true;
    group = character_1_max;
    dock_group = dockbar_character_1;
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
    '\t\tbuffui_button = "Click to show/hide active buffs panel (Hotkey: Shift+;)\\nRight-click to toggle graphical/text view";\r\n'
)

anchor = 'button_auto_sell_list = "Click and hold to choose from a list of auto sell options.";'
anchor_idx = uet_text.find(anchor)
if anchor_idx == -1:
    raise RuntimeError("ERROR: could not find button_auto_sell_list anchor in user_education_tooltips.gas")

insert_pos = anchor_idx + len(anchor)
uet_text = uet_text[:insert_pos] + rollover_additions + uet_text[insert_pos:]
print("Added Character Abilities, Shared Stash & BuffUI rollover help entries to user_education_tooltips.gas.")

# 3. Load BufUI artwork and layout from BufUI_LOA_v3.0.0.dsres and build clean UI-only BuffUI
bufui_src_path = r'Dungeon Siege 1\10-mods\BufUI LoA 3.0-167-3-0-1768604822\BufUI_LOA_v3.0.0.dsres'
t_bufui = tank_read.load(bufui_src_path)

raw_bufui_panel = t_bufui['ui/interfaces/backend/lokiswaq_bufui/lokiswaq_bufui.gas'].decode('latin1')
skrit_end_idx = raw_bufui_panel.find(']];')
if skrit_end_idx == -1:
    raise RuntimeError("ERROR: could not find end of skrit block in lokiswaq_bufui.gas")

panel_widgets = raw_bufui_panel[skrit_end_idx + 3:]
# Remove the old [t:dialog_box,n:main] widget with onstartanim = call( my_Fill_Values$ )
main_start = panel_widgets.find('[t:dialog_box,n:main]')
t_bg_start = panel_widgets.find('// Text panel background')
if main_start != -1 and t_bg_start != -1 and t_bg_start > main_start:
    panel_widgets = panel_widgets[t_bg_start:]

panel_widgets = panel_widgets.replace('Lokiswaq_BufUI_text', 'BuffUI_text')
panel_widgets = panel_widgets.replace('Lokiswaq_BufUI_icons', 'BuffUI_icons')
panel_widgets = panel_widgets.replace('Lokiswaq_BufUI', 'BuffUI')
panel_widgets = panel_widgets.replace('something went wrong :(', 'Active buffs')

buffui_gas = (
    '[BuffUI]\r\n'
    '{\r\n'
    '  intended_resolution_height = 600;\r\n'
    '  intended_resolution_width = 800;\r\n'
    '  interface = true;\r\n'
    '\r\n  ' + panel_widgets
)

# Build clean data_bar.gas from Expansion.dsres + BuffUI toggle button
data_bar_text = t_exp['ui/interfaces/backend/data_bar/data_bar.gas'].decode('latin1')
data_bar_buffui_insert = """
  skrit =
  [[
    property bool _server_only$ = false;
    owner = UIWindow;

    void move_button_BuffUI$( void )
    {
      if ( World.IsSinglePlayer )
      {
        UIWindow w$ = UIShell.FindUIWindow( "buffui_button", "data_bar" );
        w$.SetRect( 133, 151, UIShell.ScreenHeight-480+437, UIShell.ScreenHeight-480+472, false );
      }
    }

    void activate_interface_BuffUI$( void )
    {
      UIShell.ActivateInterface( "ui:interfaces:backend:buffui:BuffUI", false );
      move_button_BuffUI$();
    }

    void deactivate_interface_BuffUI$( void )
    {
      UIShell.DeactivateInterface( "BuffUI" );
    }

    void hide_interface_BuffUI$( void )
    {
      UIShell.HideInterface( "BuffUI" );
    }

    void toggle_interface_BuffUI$( void )
    {
      if ( UIShell.IsInterfaceVisible( "BuffUI" ) )
      {
        UIShell.HideInterface( "BuffUI" );
      }
      else
      {
        UIShell.ShowInterface( "BuffUI" );
      }
    }

    void swap_view_BuffUI$( void )
    {
      if ( UIShell.IsInterfaceVisible( "BuffUI" ) )
      {
        if ( UIShell.FindUIWindow( "t_background", "BuffUI" ).Visible == true )
        {
          UIShell.ShowGroup( "BuffUI_text", false, true, "BuffUI" );
          UIShell.ShowGroup( "BuffUI_icons", true, true, "BuffUI" );
        }
        else
        {
          UIShell.ShowGroup( "BuffUI_text", true, true, "BuffUI" );
          UIShell.ShowGroup( "BuffUI_icons", false, true, "BuffUI" );
        }
      }
    }
  ]];

  [t:checkbox,n:buffui_button]
  {
    f alpha = 0.800000;
    b common_control = false;
    common_template = ;
    dock_group = information_dockbar;
    i draw_order = 2;
    group = information_dockbar;
    is_bottom_anchor = true;
    bottom_anchor = 43;
    rect = 97,437,115,472;
    texture = b_gui_ig_i_it_magic-book-17;
    uvcoords = 0.000000,0.000000,1.000000,1.000000;
    wrap_mode = clamp;
    rollover_help = buffui_button;
    leftright_button = true;
    [messages]
    {
      oncreated = call( activate_interface_BuffUI$ );
      ondestroyed = call( deactivate_interface_BuffUI$ );
      onhidden = call( hide_interface_BuffUI$ );
      oncheck = call( toggle_interface_BuffUI$ );
      onuncheck = call( toggle_interface_BuffUI$ );
      onrbuttondown = setalpha(0.8) & call( swap_view_BuffUI$ );
      onrbuttonup = setalpha(1.0);
      onlbuttondown = setalpha(0.8);
      onlbuttonup = setalpha(1.0);
      onlbuttondownchecked = setalpha(0.8);
      onlbuttonupchecked = setalpha(1.0);
      onrolloff = setalpha(0.8);
      onrollover = setalpha(1.0);
      onrolloffchecked = setalpha(0.8);
      onrolloverchecked = setalpha(1.0);
    }
  }
"""

db_anchor = 'b interface = true;'
db_idx = data_bar_text.find(db_anchor)
if db_idx == -1:
    raise RuntimeError("ERROR: could not find 'b interface = true;' in data_bar.gas")
db_insert_pos = db_idx + len(db_anchor)
data_bar_text = data_bar_text[:db_insert_pos] + "\r\n" + data_bar_buffui_insert.replace('\n', '\r\n') + data_bar_text[db_insert_pos:]
print("Added BuffUI toggle button to data_bar.gas.")

# 4. Validate GAS syntax
validate_gas(awp_text, "character_awp.gas")
validate_gas(uet_text, "user_education_tooltips.gas")
validate_gas(buffui_gas, "buffui.gas")
validate_gas(data_bar_text, "data_bar.gas")

# 5. Build tank
out_tank = r'Dungeon Siege 1\01-client\Dungeon Siege 1\DSLOA\Z_CharacterAbilities_UI.dsres'
files = {
    '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui.gas': t_bufui['art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui.gas'],
    '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui.raw': t_bufui['art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui.raw'],
    '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui_empty.gas': t_bufui['art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui_empty.gas'],
    '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui_empty.raw': t_bufui['art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui_empty.raw'],
    '/ui/interfaces/backend/buffui/buffui.gas': buffui_gas.encode('latin1'),
    '/ui/interfaces/backend/character_awp/character_awp.gas': awp_text.encode('latin1'),
    '/ui/interfaces/backend/data_bar/data_bar.gas': data_bar_text.encode('latin1'),
    '/ui/user_education_tooltips.gas': uet_text.encode('latin1'),
}
nfiles, nbytes = tank_build.build_tank(
    files,
    out_tank,
    title="Character Abilities & BuffUI HUD",
    author="Antigravity",
    description="Character Abilities UI in Top-Left HUD beside weapon slots with Shared Stash button and Native BuffUI"
)
print(f"Tank built: {out_tank} ({nfiles} files, {nbytes} bytes)")

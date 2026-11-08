# build_buffui_tank.py
# Compiles the clean, native BuffUI (v3.00 MMO Edition) into a standalone .dsres tank.
# Zero legacy Skrit overrides: fully driven in real-time by SiegeHost C++ memory hooks.

import sys, os

# Support importing tank tools from project repository or fallback
tools_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '03-analysis', 'tools'))
if os.path.exists(tools_dir):
    sys.path.append(tools_dir)

try:
    import tank_build, tank_read
except ImportError:
    print("Warning: tank_build / tank_read tools not found in path. Tank build will only be available in developer environment.")
    tank_build = None
    tank_read = None

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

def main():
    if tank_build is None:
        print("tank_build module unavailable. Exiting.")
        return 1

    base_dir = os.path.dirname(os.path.abspath(__file__))
    
    # 1. Load clean buffui.gas
    buffui_gas_path = os.path.join(base_dir, 'ui', 'interfaces', 'backend', 'buffui', 'buffui.gas')
    with open(buffui_gas_path, 'r', encoding='latin1') as f:
        buffui_gas = f.read()
    validate_gas(buffui_gas, "buffui.gas")

    # 2. Art assets
    art_dir = os.path.join(base_dir, 'art', 'bitmaps', 'gui', 'in_game', 'menus')
    with open(os.path.join(art_dir, 'b_gui_ig_mnu_bufui.gas'), 'rb') as f:
        art_gas_1 = f.read()
    with open(os.path.join(art_dir, 'b_gui_ig_mnu_bufui.raw'), 'rb') as f:
        art_raw_1 = f.read()
    with open(os.path.join(art_dir, 'b_gui_ig_mnu_bufui_empty.gas'), 'rb') as f:
        art_gas_2 = f.read()
    with open(os.path.join(art_dir, 'b_gui_ig_mnu_bufui_empty.raw'), 'rb') as f:
        art_raw_2 = f.read()

    # 3. Base data_bar.gas from Expansion.dsres if available, or generate standalone hook
    exp_path = os.path.abspath(os.path.join(base_dir, '..', '..', '..', '01-client', 'Dungeon Siege 1', 'DSLOA', 'Expansion.dsres'))
    if os.path.exists(exp_path) and tank_read is not None:
        t_exp = tank_read.load(exp_path)
        data_bar_text = t_exp['ui/interfaces/backend/data_bar/data_bar.gas'].decode('latin1')
        with open(os.path.join(base_dir, 'ui', 'interfaces', 'backend', 'data_bar', 'data_bar_buffui_button.gas'), 'r', encoding='latin1') as f:
            button_insert = f.read()
        db_anchor = 'b interface = true;'
        db_idx = data_bar_text.find(db_anchor)
        if db_idx != -1:
            db_insert_pos = db_idx + len(db_anchor)
            data_bar_text = data_bar_text[:db_insert_pos] + "\r\n" + button_insert.replace('\n', '\r\n') + data_bar_text[db_insert_pos:]
        
        uet_text = t_exp['ui/user_education_tooltips.gas'].decode('latin1')
        with open(os.path.join(base_dir, 'ui', 'user_education_tooltips.gas'), 'r', encoding='latin1') as f:
            tooltip_insert = f.read()
        anchor = 'button_auto_sell_list = "Click and hold to choose from a list of auto sell options.";'
        anchor_idx = uet_text.find(anchor)
        if anchor_idx != -1:
            uet_add = '\r\n\t\tbuffui_button = "Click to show/hide active buffs panel (Hotkey: Shift+;)\\nRight-click to toggle graphical/text view";\r\n'
            insert_pos = anchor_idx + len(anchor)
            uet_text = uet_text[:insert_pos] + uet_add + uet_text[insert_pos:]
    else:
        # Fallback to local snippet
        with open(os.path.join(base_dir, 'ui', 'interfaces', 'backend', 'data_bar', 'data_bar_buffui_button.gas'), 'r', encoding='latin1') as f:
            data_bar_text = "[data_bar]\n{\n" + f.read() + "\n}"
        with open(os.path.join(base_dir, 'ui', 'user_education_tooltips.gas'), 'r', encoding='latin1') as f:
            uet_text = f.read()

    validate_gas(data_bar_text, "data_bar.gas")
    validate_gas(uet_text, "user_education_tooltips.gas")

    # 4. Pack into standalone tank
    out_tank = os.path.join(base_dir, 'BufUI_LoA_v3.00.dsres')
    files = {
        '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui.gas': art_gas_1,
        '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui.raw': art_raw_1,
        '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui_empty.gas': art_gas_2,
        '/art/bitmaps/gui/in_game/menus/b_gui_ig_mnu_bufui_empty.raw': art_raw_2,
        '/ui/interfaces/backend/buffui/buffui.gas': buffui_gas.encode('latin1'),
        '/ui/interfaces/backend/data_bar/data_bar.gas': data_bar_text.encode('latin1'),
        '/ui/user_education_tooltips.gas': uet_text.encode('latin1'),
    }

    nfiles, nbytes = tank_build.build_tank(
        files,
        out_tank,
        title="BufUI LoA v3.00 Native Edition",
        author="Sabedig, Lokiswaq & MMO Engine",
        description="Active Buffs Display with dynamic expansion and native C++ memory hook"
    )
    print(f"Tank compiled successfully: {out_tank} ({nfiles} files, {nbytes} bytes)")
    return 0

if __name__ == '__main__':
    sys.exit(main())

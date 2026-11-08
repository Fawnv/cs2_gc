"""Set up the cs2_gc demo mod in <data>\\mods\\cs2gc_demo (reads the game files only).

    python tools\\make_demo_mod.py --cs2 "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive" ^
        --data release\\cs2_gc --add-item --enable

What it puts in <data>\\mods\\cs2gc_demo:
  mod.txt, items_game.txt, localization.txt     from examples\\mods\\cs2gc_demo (paint kit 20001 "cs2gc_demo")
  files\\weapons\\paints\\cs2gc\\cs2gc_demo.vcompmat_c   copy of AK-47 | Inheritance's material
  files\\panorama\\images\\econ\\default_generated\\weapon_ak47_cs2gc_demo_*_png.vtex_c   its icons
  files\\sounds\\music\\<kit>\\mainmenu.vsnd_c           the CS:GO 2012 menu theme over the CS2 default
                                                     kit and The Verkkars kit

--add-item appends an AK-47 with paint kit 20001 to <data>\\inventory.txt (only while cs2.exe isn't
running, the gc would overwrite the file). --enable sets "mods" "1" in <data>\\config.txt.
These are copies of Valve's files for a local test, don't redistribute them.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vpk_extract import read_file, read_tree  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = "cs2gc_demo"
DEMO_PAINT_KIT = 20001

COPIES = [
    ("weapons/paints/community/community_33/ak_porcelain.vcompmat_c", "weapons/paints/cs2gc/cs2gc_demo.vcompmat_c"),
    ("sounds/music/valve_01/mainmenu.vsnd_c", "sounds/music/valve_cs2_01/mainmenu.vsnd_c"),
    # the music kit the test inventory has equipped (The Verkkars - EZ4ENCE)
    ("sounds/music/valve_01/mainmenu.vsnd_c", "sounds/music/theverkkars_01/mainmenu.vsnd_c"),
]
for size in ("light", "medium", "heavy"):
    COPIES.append((f"panorama/images/econ/default_generated/weapon_ak47_ak_porcelain_{size}_png.vtex_c",
                   f"panorama/images/econ/default_generated/weapon_ak47_cs2gc_demo_{size}_png.vtex_c"))


def cs2_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq cs2.exe"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "cs2.exe" in out.lower()


def add_item(inventory_path):
    text = open(inventory_path, encoding="utf-8").read()
    if re.search(r'"6"\s+"%d"' % DEMO_PAINT_KIT, text):
        print("inventory already has the demo AK-47")
        return
    ids = [int(x) for x in re.findall(r'^\t"(\d+)"\s*$', text, re.M)]
    new_id = max(ids, default=0) + 1
    block = (
        f'\t"{new_id}"\n\t{{\n'
        f'\t\t"inventory"\t\t"{new_id}"\n\t\t"def_index"\t\t"7"\n\t\t"level"\t\t"1"\n'
        f'\t\t"quality"\t\t"4"\n\t\t"flags"\t\t"0"\n\t\t"origin"\t\t"8"\n\t\t"in_use"\t\t"0"\n'
        f'\t\t"rarity"\t\t"6"\n\t\t"attributes"\n\t\t{{\n'
        f'\t\t\t"6"\t\t"{DEMO_PAINT_KIT}"\n\t\t\t"7"\t\t"1"\n\t\t\t"8"\t\t"0.05"\n\t\t}}\n\t}}\n'
    )
    end = text.rstrip().rfind("}")
    text = text[:end] + block + text[end:]
    shutil.copyfile(inventory_path, inventory_path + ".before_demo_mod")
    open(inventory_path, "w", encoding="utf-8", newline="\n").write(text)
    print(f"added AK-47 | cs2_gc demo as item {new_id} (backup: inventory.txt.before_demo_mod)")


def enable(config_path):
    text = open(config_path, encoding="utf-8").read() if os.path.exists(config_path) else ""
    text = re.sub(r'\n?//[^\n]*\n"custom_content"\s+"\d+"\n?', "\n", text)
    text = re.sub(r'"custom_content"\s+"\d+"\n?', "", text)
    if re.search(r'"mods"\s+"\d+"', text):
        text = re.sub(r'"mods"\s+"\d+"', '"mods"\t"1"', text)
    else:
        text = text.rstrip("\n") + '\n\n// CSForge (mod loader): cs2_gc\\mods\\<mod> (see README, "CSForge")\n"mods"\t"1"\n'
    open(config_path, "w", encoding="utf-8", newline="\n").write(text)
    print("mods enabled in", config_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cs2", required=True)
    ap.add_argument("--data", required=True, help="cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--add-item", action="store_true")
    ap.add_argument("--enable", action="store_true")
    args = ap.parse_args()

    mod = os.path.join(args.data, "mods", MOD)
    files = os.path.join(mod, "files")
    os.makedirs(files, exist_ok=True)

    for name in ("mod.txt", "items_game.txt", "localization.txt"):
        shutil.copyfile(os.path.join(ROOT, "examples", "mods", MOD, name), os.path.join(mod, name))
        print("wrote", os.path.join(mod, name))

    vpk = os.path.join(args.cs2, "game", "csgo", "pak01_dir.vpk")
    dir_data, data_start, entries = read_tree(vpk)
    for src, dst in COPIES:
        blob = read_file(vpk, entries[src], dir_data, data_start)
        out = os.path.join(files, *dst.split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(blob)
        print(f"{src} -> mods\\{MOD}\\files\\{dst} ({len(blob)} bytes)")

    if args.add_item:
        if cs2_running():
            print("cs2.exe is running, not touching inventory.txt (close the game and run again)")
        else:
            add_item(os.path.join(args.data, "inventory.txt"))

    if args.enable:
        enable(os.path.join(args.data, "config.txt"))


if __name__ == "__main__":
    main()

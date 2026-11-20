r"""Give a CSForge mod new ids where the game or another mod already has its ids.

    python tools\renumber_mod.py --data release\cs2_gc some_mod
    python tools\renumber_mod.py --data release\cs2_gc some_mod --dry-run
    python tools\renumber_mod.py --data release\cs2_gc some_mod --move-items

A mod's skin, music kit, sticker, charm or item with an id the game (or a mod loaded before it) already
has isn't loaded: CSForge skips it and the Mods tab says so ("paint_kits/20001 isn't added, the mod low
already has it"). This tool finds those ids in the mod's items_game.txt and gives them free ones, in the
mod's items_game.txt and in what the tools rebuild it from (skins.json of the skin editor, cases.json of
make_case_mod.py), so a rebuild keeps them. A case's "set supply crate series" follows its loot list.

The items already in inventory.txt with an old id show the other mod's skin (that one was loaded); with
--move-items they get the new id too, use it when they were made for this mod. The game must be closed
then. Ids that only a mod loaded after this one has are left alone: that mod loses them, not this one.
"""
import argparse
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_invedit_catalog import parse_kv, to_dict  # noqa: E402
from make_music_mod import cs2_running  # noqa: E402
from vpk_extract import read_file, read_tree  # noqa: E402

# sections whose keys are ids, and the inventory attribute (or item field) that refers to them
ID_SECTIONS = {
    "paint_kits": ("attribute", 6),
    "sticker_kits": ("attribute", 113),
    "music_definitions": ("attribute", 166),
    "keychain_definitions": ("attribute", 299),
    "items": ("def_index", None),
    "revolving_loot_lists": (None, None),
}


def find_cs2():
    from skin_editor import find_cs2 as find
    return find()


def tokens(text):
    """(kind, start, end) of the KeyValues text: "s" a string (quotes included), "{" or "}"."""
    at, n = 0, len(text)
    while at < n:
        c = text[at]
        if c.isspace():
            at += 1
        elif text.startswith("//", at):
            nl = text.find("\n", at)
            at = n if nl < 0 else nl
        elif c in "{}":
            yield c, at, at + 1
            at += 1
        elif c == '"':
            end = at + 1
            while end < n and text[end] != '"':
                end += 2 if text[end] == "\\" and end + 1 < n and text[end + 1] in '"\\' else 1
            yield "s", at, min(end + 1, n)
            at = end + 1
        else:
            end = at
            while end < n and not text[end].isspace() and text[end] not in '{}"':
                end += 1
            yield "s", at, end
            at = end


def unquote(text, start, end):
    s = text[start:end]
    return s[1:-1] if s.startswith('"') else s


def id_keys(text):
    """[(section, id, key start, key end)] for the direct children of the ID_SECTIONS in the root block."""
    found, stack, pending = [], [], None
    for kind, start, end in tokens(text):
        if kind == "{":
            stack.append(pending)
            pending = None
        elif kind == "}":
            if stack:
                stack.pop()
            pending = None
        elif pending is None:
            pending = unquote(text, start, end).lower()
            if len(stack) == 2 and stack[1] in ID_SECTIONS and pending.isdigit():
                found.append((stack[1], int(pending), start, end))
        else:
            pending = None  # a key and its string value
    return found


def mod_info(path):
    info = {"enabled": "1", "priority": "0"}
    if os.path.exists(path):
        try:
            info.update(to_dict(parse_kv(open(path, encoding="utf-8", errors="replace").read())).get("mod", {}))
        except Exception:
            pass
    return info.get("enabled", "1") != "0", int(str(info.get("priority", "0")).lstrip("+") or 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cs2", help="CS2 folder (found through Steam when not given)")
    ap.add_argument("--data", required=True, help="the cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("mod", help="the mod's folder in <data>\\mods")
    ap.add_argument("--dry-run", action="store_true", help="only say what would change")
    ap.add_argument("--move-items", action="store_true", help="give the inventory's items with an old id the new one")
    args = ap.parse_args()

    mods_dir = os.path.join(args.data, "mods")
    mod_dir = os.path.join(mods_dir, args.mod)
    items_path = os.path.join(mod_dir, "items_game.txt")
    if not os.path.exists(items_path):
        sys.exit(f"{items_path} isn't there: the mod adds no ids")
    cs2 = args.cs2 or find_cs2()
    if not cs2:
        sys.exit("CS2 not found, pass --cs2")

    vpk = os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")
    dir_data, data_start, entries = read_tree(vpk)
    game_text = read_file(vpk, entries["scripts/items/items_game.txt"], dir_data, data_start).decode("utf-8", "replace")

    # who has which ids: the game, then the mods in load order (priority, then folder name)
    used = {section: set() for section in ID_SECTIONS}
    before = {section: set() for section in ID_SECTIONS}  # what this mod loses to
    for section, number, _, _ in id_keys(game_text):
        used[section].add(number)
        before[section].add(number)
    me = mod_info(os.path.join(mod_dir, "mod.txt"))
    order = []
    for name in os.listdir(mods_dir):
        path = os.path.join(mods_dir, name, "items_game.txt")
        if name == args.mod or name.startswith(("_", ".")) or not os.path.exists(path):
            continue
        enabled, priority = mod_info(os.path.join(mods_dir, name, "mod.txt"))
        order.append(((priority, name) < (me[1], args.mod) and enabled, name, path))
    for loads_before, name, path in order:
        for section, number, _, _ in id_keys(open(path, encoding="utf-8", errors="replace").read()):
            used[section].add(number)
            if loads_before:
                before[section].add(number)

    text = open(items_path, encoding="utf-8").read()
    mine = id_keys(text)
    for section, number, _, _ in mine:
        used[section].add(number)
    changes = {}  # (section, old) -> new
    for section, number, _, _ in mine:
        if number in before[section] and (section, number) not in changes:
            new = max(used[section]) + 1
            used[section].add(new)
            changes[(section, number)] = new
    if not changes:
        print(f"{args.mod}: none of its ids is taken, nothing to change")
        return
    for (section, old), new in sorted(changes.items()):
        print(f"{section}/{old} -> {new}")
    if args.dry_run:
        return

    # items_game.txt: the keys, back to front so the offsets stay right
    for section, number, start, end in sorted(mine, key=lambda m: -m[2]):
        if (section, number) in changes:
            text = text[:start] + f'"{changes[(section, number)]}"' + text[end:]
    # a case's crate series is a value of its item
    series = {old: new for (section, old), new in changes.items() if section == "revolving_loot_lists"}
    if series:
        def swap(m):
            return m.group(1) + str(series.get(int(m.group(2)), m.group(2))) + m.group(3)
        text = re.sub(r'("supply_crate_series"\s*"value"\s*")(\d+)(")', swap, text)
    backup = items_path + ".before_renumber"
    if not os.path.exists(backup):
        shutil.copyfile(items_path, backup)
    open(items_path, "w", encoding="utf-8", newline="\n").write(text)
    print(f"wrote {items_path} (backup {os.path.basename(backup)})")

    for manifest, fields in (("skins.json", {"id": "paint_kits"}), ("cases.json", {"def_index": "items", "series": "revolving_loot_lists"})):
        path = os.path.join(mod_dir, manifest)
        if not os.path.exists(path):
            continue
        data = json.load(open(path, encoding="utf-8"))
        touched = 0
        for entry in data.values():
            for field, section in fields.items():
                new = changes.get((section, entry.get(field)))
                if new:
                    entry[field] = new
                    touched += 1
        if touched:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(data, f, ensure_ascii=False, indent=1)
            print(f"{manifest}: {touched} ids changed")

    if args.move_items:
        inventory = os.path.join(args.data, "inventory.txt")
        if cs2_running():
            print("the game is running, inventory.txt not touched (close it and run with --move-items again)")
            return
        inv = open(inventory, encoding="utf-8").read()
        moved = 0
        for (section, old), new in changes.items():
            kind, attribute = ID_SECTIONS[section]
            if kind == "attribute":
                pattern = r'(\n\t\t\t"%d"\t\t")%d(?:\.0+)?(")' % (attribute, old)
            elif kind == "def_index":
                pattern = r'(\n\t\t"def_index"\t\t")%d(")' % old
            else:
                continue
            inv, count = re.subn(pattern, lambda m, new=new: m.group(1) + str(new) + m.group(2), inv)
            moved += count
        if moved:
            backup = inventory + ".before_renumber"
            if not os.path.exists(backup):
                shutil.copyfile(inventory, backup)
            open(inventory, "w", encoding="utf-8", newline="\n").write(inv)
        print(f"inventory.txt: {moved} items moved to the new ids")
    print("applies at the next game start")


if __name__ == "__main__":
    main()

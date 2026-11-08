"""Print the knife/glove loot lists that new CS2 cases are missing, for items_extra.txt.

Valve doesn't ship the "*_unusual" loot lists (the rare special items of a case), so a
new case can't be opened until its list is added to items_extra.txt. This finds every
"*_unusual" list the game's loot lists reference but neither items_game.txt nor
items_extra.txt define, and builds it from ByMykel/CSGO-API crates.json (contains_rare).

    python tools\\gen_unusual_lists.py --cs2 "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive" ^
        --crates crates.json --extra release\\cs2_gc\\items_extra.txt

crates.json: https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/crates.json
Paste the printed blocks into a "client_loot_lists" block of items_extra.txt.
Reads the game files only.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_invedit_catalog import Schema, load_localization, parse_kv, to_dict  # noqa: E402
from vpk_extract import read_file, read_tree  # noqa: E402


def vpk_text(vpk, name):
    dir_data, data_start, entries = read_tree(vpk)
    return read_file(vpk, entries[name], dir_data, data_start)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cs2", required=True, help="CS2 install folder")
    ap.add_argument("--crates", required=True, help="ByMykel crates.json")
    ap.add_argument("--extra", help="items_extra.txt, lists already there are skipped")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")

    vpk = os.path.join(args.cs2, "game", "csgo", "pak01_dir.vpk")
    ig = to_dict(parse_kv(vpk_text(vpk, "scripts/items/items_game.txt").decode("utf-8", "replace")))["items_game"]
    english = load_localization(vpk_text(vpk, "resource/csgo_english.txt"))
    schema = Schema(ig, english, english)

    defined = set(ig.get("client_loot_lists", {}))
    if args.extra:
        extra = to_dict(parse_kv(open(args.extra, encoding="utf-8").read()))
        defined |= set(extra.get("items_game", extra).get("client_loot_lists", {}))

    # case loot list -> missing unusual list names
    missing = {}
    for name, entries in ig.get("client_loot_lists", {}).items():
        for entry in entries:
            if entry.endswith("_unusual") and entry not in defined:
                missing.setdefault(name, set()).add(entry)

    # crate def index -> its case loot list
    crate_lists = {}
    for defindex, item in ig["items"].items():
        if not isinstance(item, dict):
            continue
        series = schema.resolve(item).get("attributes", {}).get("set supply crate series", {})
        if isinstance(series, dict) and series.get("value") in ig.get("revolving_loot_lists", {}):
            crate_lists[defindex] = ig["revolving_loot_lists"][series["value"]]

    # english display name -> class name for knives and gloves
    classes = {}
    for item in ig["items"].values():
        if isinstance(item, dict):
            resolved = schema.resolve(item)
            name = resolved.get("name", "")
            if name.startswith(("weapon_knife", "weapon_bayonet")) or name.endswith(("_gloves", "_handwraps")):
                classes[schema.loc(resolved.get("item_name", ""))] = name
    paint_names = {k: v["name"] for k, v in ig["paint_kits"].items() if isinstance(v, dict) and "name" in v}

    crates = {c["id"]: c for c in json.load(open(args.crates, encoding="utf-8"))}
    done = set()
    for defindex, list_name in sorted(crate_lists.items(), key=lambda kv: int(kv[0])):
        for unusual in sorted(missing.get(list_name, ())):
            if unusual in done:
                continue
            crate = crates.get(f"crate-{defindex}")
            if not crate or not crate.get("contains_rare"):
                print(f"// {unusual}: no contains_rare for crate {defindex} in crates.json", file=sys.stderr)
                continue
            entries = []
            for rare in crate["contains_rare"]:
                base = rare["name"].split(" | ")[0].replace("\u2605", "").strip()
                cls = classes.get(base)
                kit = paint_names.get(str(rare.get("paint_index"))) if rare.get("paint_index") else "default"
                if not cls or not kit:
                    print(f"// skipped {rare['name']}", file=sys.stderr)
                    continue
                entry = f"[{kit}]{cls}"
                if entry not in entries:
                    entries.append(entry)
            print(f"\t\t// {crate['name']} ({defindex})")
            print(f'\t\t"{unusual}"\n\t\t{{')
            for entry in entries:
                print(f'\t\t\t"{entry}"\t\t"1"')
            print("\t\t}")
            done.add(unusual)

    if not done:
        print("// nothing missing", file=sys.stderr)


if __name__ == "__main__":
    main()

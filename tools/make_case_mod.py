r"""A case with your own skins in it (CSForge mod "cases").

    python tools\make_case_mod.py --data release\cs2_gc --name "My case"
    python tools\make_case_mod.py --data release\cs2_gc --name "My case" --skins cs2gc_dragon_ak47 "cs2gc_*_awp" --add-item 5
    python tools\make_case_mod.py --data release\cs2_gc --name "My case" --icon case.png --like crate_community_33
    python tools\make_case_mod.py --data release\cs2_gc --list
    python tools\make_case_mod.py --data release\cs2_gc --remove my_case

Takes the skins of a skin mod (mods\skins, made by skin_editor.py / make_skin_mod.py; all of them, or the kits
named by --skins, wildcards allowed) and writes mods\cases: a case item that needs no key, its loot lists by
rarity and a collection ("item set") of its skins, so they show their collection and work in trade ups. Knife
skins become the case's rare special items: their list goes into the mod's items_extra.txt, which only the gc
reads, the same way the game's own cases keep their knives out of the client's files. Skins of grenades and the
C4 are left out (the game doesn't draw them as paint).

The chance of a rarity is the gc's usual one (config "rarity_weights"), split between the rarities the case
has: a case of only Covert skins always gives a Covert one.

The case looks like the --like case (its 3D model in the opening and its icon) unless --icon gives a picture
for the icon. --add-item N puts N cases into inventory.txt (the game has to be closed). Running the tool again
with the same --name rebuilds that case; the game files are only read.
"""
import argparse
import fnmatch
import json
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_skin_mod as msm  # noqa: E402
import source2 as s2  # noqa: E402
from gen_invedit_catalog import painted_rarity, parse_kv, to_dict  # noqa: E402
from make_music_mod import add_inventory_item, cs2_running, kv_string  # noqa: E402

FIRST_DEF = 40000  # the game's item definitions end at about 30100
FIRST_SERIES = 10001  # "set supply crate series", the game's are below 500
ICON_W, ICON_H = 256, 198  # the size of the game's case icons
RARITY_NAMES = {1: "common", 2: "uncommon", 3: "rare", 4: "mythical", 5: "legendary", 6: "ancient", 7: "ancient"}
SECTIONS = ("items", "revolving_loot_lists", "client_loot_lists", "item_sets")


def find_cs2():
    from skin_editor import find_cs2 as find
    return find()


def is_knife(weapon):
    return weapon.startswith("weapon_knife") or weapon == "weapon_bayonet"


class Cases:
    """mods\\<name>: cases.json is the source, the mod's other files are made from it."""

    def __init__(self, data, name):
        self.data = data
        self.dir = os.path.join(data, "mods", name)
        self.manifest_path = os.path.join(self.dir, "cases.json")
        self.cases = {}
        if os.path.exists(self.manifest_path):
            self.cases = json.load(open(self.manifest_path, encoding="utf-8"))

    def icon_file(self, case):
        return os.path.join(self.dir, "files", "panorama", "images", "econ", "weapon_cases", f"crate_{case}_png.vtex_c")

    def other_mods(self):
        """The item definitions and crate series the other mods use."""
        defs, series = set(), set()
        mods = os.path.join(self.data, "mods")
        for name in os.listdir(mods) if os.path.isdir(mods) else []:
            path = os.path.join(mods, name, "items_game.txt")
            if os.path.join(mods, name) == self.dir or not os.path.exists(path):
                continue
            try:
                tree = to_dict(parse_kv(open(path, encoding="utf-8", errors="replace").read())).get("items_game", {})
            except Exception:
                continue
            defs |= {int(k) for k in tree.get("items", {}) if k.isdigit()}
            series |= {int(k) for k in tree.get("revolving_loot_lists", {}) if k.isdigit()}
        return defs, series

    def skins_of(self, case):
        """[(kit, skin)] of a case that its skin mod still has."""
        path = os.path.join(self.data, "mods", case["skin_mod"], "skins.json")
        skins = json.load(open(path, encoding="utf-8")) if os.path.exists(path) else {}
        return [(kit, skins[kit]) for kit in case["skins"] if kit in skins]

    def save(self, game, mod_name, priority, log=print):
        os.makedirs(self.dir, exist_ok=True)
        with open(self.manifest_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.cases, f, ensure_ascii=False, indent=1)

        items = game.items_game.get("items", {})
        sections = {name: [] for name in SECTIONS}
        extra, tokens = [], []
        in_a_set = set()  # an item is in one collection: the first case that has it
        for name, case in sorted(self.cases.items(), key=lambda kv: kv[1]["def_index"]):
            by_rarity, knives = {}, []
            for kit, skin in self.skins_of(case):
                entry = f'[{kit}]{skin["weapon"]}'
                if is_knife(skin["weapon"]):
                    knives.append(entry)
                    continue
                item = items.get(str(skin["def_index"]), {})
                weapon_rarity = game.rarity_value(game.resolved(item, "item_rarity") or "common")
                rarity = max(1, painted_rarity(weapon_rarity, game.rarity_value(skin["rarity"])))
                by_rarity.setdefault(RARITY_NAMES[rarity], []).append(entry)
            missing = len(case["skins"]) - len(knives) - sum(len(v) for v in by_rarity.values())
            if missing:
                log(f'{name}: {missing} of its skins are no longer in mods\\{case["skin_mod"]}, left out')

            like = next((i for i in items.values() if i.get("name") == case["like"]), {})
            icon = f"econ/weapon_cases/crate_{name}" if os.path.exists(self.icon_file(name)) \
                else game.resolved(like, "image_inventory") or "econ/weapon_cases/weapon_case_generic"
            model = like.get("model_player")
            unusual = f"set_{name}_unusual"

            sections["items"].append(
                f'\t\t"{case["def_index"]}"\n\t\t{{\n'
                f'\t\t\t"name"\t\t"crate_{name}"\n'
                f'\t\t\t"item_name"\t\t"#CSGO_crate_{name}"\n'
                f'\t\t\t"item_description"\t\t"#CSGO_crate_{name}_desc"\n'
                f'\t\t\t"image_inventory"\t\t"{icon}"\n'
                + (f'\t\t\t"model_player"\t\t"{model}"\n' if model else "")
                # no key: the prefab of the game's collection packages, without the case key of "weapon_case"
                + '\t\t\t"prefab"\t\t"weapon_case_selfopening_collection"\n'
                '\t\t\t"attributes"\n\t\t\t{\n\t\t\t\t"set supply crate series"\n\t\t\t\t{\n'
                '\t\t\t\t\t"attribute_class"\t\t"supply_crate_series"\n'
                f'\t\t\t\t\t"value"\t\t"{case["series"]}"\n\t\t\t\t}}\n\t\t\t}}\n'
                '\t\t\t"tags"\n\t\t\t{\n\t\t\t\t"ItemSet"\n\t\t\t\t{\n'
                f'\t\t\t\t\t"tag_value"\t\t"set_{name}"\n\t\t\t\t\t"tag_text"\t\t"#CSGO_set_{name}"\n'
                '\t\t\t\t\t"tag_group"\t\t"ItemSet"\n\t\t\t\t\t"tag_group_text"\t\t"#SFUI_InvTooltip_SetTag"\n'
                '\t\t\t\t}\n\t\t\t}\n\t\t}\n')
            sections["revolving_loot_lists"].append(f'\t\t"{case["series"]}"\t\t"crate_{name}_lootlist"\n')

            lists = ""
            order = [r for r in ("common", "uncommon", "rare", "mythical", "legendary", "ancient") if r in by_rarity]
            for rarity in order:
                lists += f'\t\t"crate_{name}_{rarity}"\n\t\t{{\n' + "".join(f'\t\t\t"{e}"\t\t"1"\n' for e in by_rarity[rarity]) + "\t\t}\n"
            lists += f'\t\t"crate_{name}_lootlist"\n\t\t{{\n' + "".join(f'\t\t\t"crate_{name}_{r}"\t\t"1"\n' for r in order)
            if knives:
                lists += f'\t\t\t"{unusual}"\t\t"1"\n'
            sections["client_loot_lists"].append(lists + "\t\t}\n")

            own = [e for r in order for e in by_rarity[r] if e not in in_a_set]
            in_a_set.update(own)
            item_set = (f'\t\t"set_{name}"\n\t\t{{\n\t\t\t"name"\t\t"#CSGO_set_{name}"\n'
                        f'\t\t\t"set_description"\t\t"#CSGO_set_{name}_desc"\n\t\t\t"is_collection"\t\t"1"\n'
                        '\t\t\t"items"\n\t\t\t{\n' + "".join(f'\t\t\t\t"{e}"\t\t"1"\n' for e in own) + "\t\t\t}\n")
            if knives:
                item_set += f'\t\t\t"unusuals"\n\t\t\t{{\n\t\t\t\t"unique"\t\t"{unusual}"\n\t\t\t\t"strange"\t\t"{unusual}"\n\t\t\t}}\n'
                extra.append(f'\t\t"{unusual}"\n\t\t{{\n' + "".join(f'\t\t\t"{e}"\t\t"1"\n' for e in knives) + "\t\t}\n")
            sections["item_sets"].append(item_set + "\t\t}\n")

            title, description = kv_string(case["title"]), kv_string(case.get("description", ""))
            tokens += [(f"CSGO_crate_{name}", title), (f"CSGO_crate_{name}_desc", description),
                       (f"CSGO_set_{name}", title), (f"CSGO_set_{name}_desc", description)]

        note = "// made by tools\\make_case_mod.py from cases.json (edits here get overwritten)\n"
        with open(os.path.join(self.dir, "items_game.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(note + '"items_game"\n{\n')
            for section in SECTIONS:
                f.write(f'\t"{section}"\n\t{{\n' + "".join(sections[section]) + "\t}\n")
            f.write("}\n")
        extra_path = os.path.join(self.dir, "items_extra.txt")
        if extra:
            with open(extra_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(note + "// only the gc reads this file: the knives of the cases, which the client isn't told\n")
                f.write('"items_game"\n{\n\t"client_loot_lists"\n\t{\n' + "".join(extra) + "\t}\n}\n")
        elif os.path.exists(extra_path):
            os.remove(extra_path)
        with open(os.path.join(self.dir, "localization.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("// made by tools\\make_case_mod.py\n\"lang\"\n{\n\t\"Tokens\"\n\t{\n")
            f.write("".join(f'\t\t"{key}"\t\t"{text}"\n' for key, text in tokens) + "\t}\n}\n")
        path = os.path.join(self.dir, "mod.txt")
        if not os.path.exists(path):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("// made by tools\\make_case_mod.py\n\"mod\"\n{\n")
                f.write(f'\t"name"\t\t"{kv_string(mod_name)}"\n\t"description"\t"cases with custom skins"\n')
                f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{priority}"\n}}\n')


def encode_icon(picture, template_blob):
    """A picture as a case icon: fitted into 256x198 on a transparent ground, BGRA with one smaller mip."""
    img = Image.open(picture).convert("RGBA")
    img.thumbnail((ICON_W, ICON_H), Image.LANCZOS)
    canvas = Image.new("RGBA", (ICON_W, ICON_H), (0, 0, 0, 0))
    canvas.paste(img, ((ICON_W - img.width) // 2, (ICON_H - img.height) // 2))
    rgba = np.asarray(canvas)
    mips = []
    for level in range(2):
        w, h = ICON_W >> level, ICON_H >> level
        m = np.asarray(canvas.resize((w, h), Image.LANCZOS)) if level else rgba
        mips.append((m[..., [2, 1, 0, 3]].tobytes(), w, h))
    red2 = s2.Resource.parse(template_blob).block("RED2") if template_blob else None
    return s2.build_texture(None, s2.FMT_BGRA8888, mips, s2.VTEX_NO_LOD, s2.fallback_bits(rgba), red2)


def pick_skins(skins, patterns, log=print):
    """The kits of a skin mod for a case: all, or those matching the patterns; grenade and C4 skins never."""
    picked = []
    for kit, skin in sorted(skins.items(), key=lambda kv: kv[1]["id"]):
        if patterns and not any(fnmatch.fnmatch(kit, p) or fnmatch.fnmatch(skin.get("name", ""), p) for p in patterns):
            continue
        if skin.get("replace"):
            log(f"{kit}: a grenade / C4 skin, not put into the case")
            continue
        picked.append(kit)
    return picked


def make_case(game, cases, args, log=print):
    slug = msm.slug(args.case_name or args.name)
    same_title = next((n for n, c in cases.cases.items() if c["title"] == args.name), None)
    if same_title and not args.case_name:
        name = same_title  # the same title again rebuilds that case
    elif slug == "skin":  # nothing Latin in the title
        name = next(f"cs2gc_case{i}" for i in range(1, 1000) if f"cs2gc_case{i}" not in cases.cases)
    else:
        name = "cs2gc_" + slug
    skins_path = os.path.join(args.data, "mods", args.skin_mod, "skins.json")
    if not os.path.exists(skins_path):
        sys.exit(f"no {skins_path}: make skins first (skin_editor.py or make_skin_mod.py)")
    picked = pick_skins(json.load(open(skins_path, encoding="utf-8")), args.skins, log)
    if not picked:
        sys.exit("no skins for the case" + (f" match {' '.join(args.skins)}" if args.skins else f" in mods\\{args.skin_mod}"))

    items = game.items_game.get("items", {})
    if not any(i.get("name") == args.like for i in items.values()):
        sys.exit(f"the game has no case named {args.like} (the \"name\" of its item in items_game.txt)")

    old = cases.cases.get(name, {})
    other_defs, other_series = cases.other_mods()
    used_defs = {int(k) for k in items if k.isdigit()} | other_defs | {c["def_index"] for c in cases.cases.values()}
    used_series = {int(k) for k in game.items_game.get("revolving_loot_lists", {}) if k.isdigit()} | other_series \
        | {c["series"] for c in cases.cases.values()}
    def_index = old.get("def_index") or next(i for i in range(FIRST_DEF, 65000) if i not in used_defs)
    series = old.get("series") or next(i for i in range(FIRST_SERIES, 60000) if i not in used_series)

    cases.cases[name] = {"def_index": def_index, "series": series, "title": args.name, "description": args.description,
                         "like": args.like, "skin_mod": args.skin_mod, "skins": picked}
    icon_file = cases.icon_file(name)
    if args.icon:
        like = next(i for i in items.values() if i.get("name") == args.like)
        template = (game.resolved(like, "image_inventory") or "") + "_png.vtex_c"
        template = "panorama/images/" + template
        blob = encode_icon(args.icon, game.read(template) if template in game.entries else None)
        os.makedirs(os.path.dirname(icon_file), exist_ok=True)
        with open(icon_file, "wb") as f:
            f.write(blob)
    knives = sum(1 for kit, skin in cases.skins_of(cases.cases[name]) if is_knife(skin["weapon"]))
    log(f'{"rebuilt" if old else "new"} case {name} "{args.name}": item {def_index}, series {series}, '
        f'{len(picked) - knives} skins' + (f" and {knives} knives as the rare special item" if knives else ""))
    return name, def_index


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cs2", help="CS2 folder (found through Steam when not given)")
    ap.add_argument("--data", required=True, help="the cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--name", help="the case's name in game")
    ap.add_argument("--case-name", help="internal name (Latin letters, digits, _), by default made from --name")
    ap.add_argument("--description", default="", help="the text under the name")
    ap.add_argument("--skins", nargs="*", default=[], metavar="KIT", help="kits (or their names in game) to put in, wildcards allowed; default all")
    ap.add_argument("--skin-mod", default="skins", help="the mod the skins are in (mods\\skins)")
    ap.add_argument("--like", default="crate_community_33", help="the game's case to look like (default the Kilowatt Case)")
    ap.add_argument("--icon", help="a picture for the case's icon instead of the --like case's")
    ap.add_argument("--add-item", type=int, nargs="?", const=1, default=0, metavar="N", help="put N of the case into inventory.txt")
    ap.add_argument("--mod", default="cases", help="mod folder (mods\\cases)")
    ap.add_argument("--mod-name", default="Custom cases")
    ap.add_argument("--priority", type=int, default=20)
    ap.add_argument("--list", action="store_true", help="the cases of the mod")
    ap.add_argument("--remove", metavar="CASE", help="delete a case (its internal name, see --list)")
    args = ap.parse_args()

    cases = Cases(args.data, args.mod)
    if args.list:
        for name, case in sorted(cases.cases.items(), key=lambda kv: kv[1]["def_index"]):
            print(f'{name}: "{case["title"]}", item {case["def_index"]}, {len(case["skins"])} skins of mods\\{case["skin_mod"]}')
        if not cases.cases:
            print(f"no cases in {cases.dir}")
        return
    if not args.remove and not args.name:
        ap.error("--name (or --list, --remove)")

    cs2 = args.cs2 or find_cs2()
    if not cs2:
        sys.exit("CS2 not found, pass --cs2")
    game = msm.Game(cs2)

    if args.remove:
        if args.remove not in cases.cases:
            sys.exit(f"no case {args.remove} in {cases.dir} (see --list)")
        del cases.cases[args.remove]
        if os.path.exists(cases.icon_file(args.remove)):
            os.remove(cases.icon_file(args.remove))
        cases.save(game, args.mod_name, args.priority)
        print(f"removed {args.remove}; its items in inventory.txt stay and show as unknown, delete them in the editor")
        return

    name, def_index = make_case(game, cases, args)
    cases.save(game, args.mod_name, args.priority)
    print(f"wrote {cases.dir}")

    if args.add_item:
        if cs2_running():
            print("the game is running, not touching inventory.txt (close the game and run again with --add-item)")
        else:
            inventory = os.path.join(args.data, "inventory.txt")
            ids = [add_inventory_item(inventory, def_index, 4, 1, [], ".before_case_mod") for _ in range(args.add_item)]
            print(f"added {len(ids)} of the case to the inventory (items {ids[0]}..{ids[-1]}, backup: inventory.txt.before_case_mod)")
    print('start the game through run_cs2_gc.cmd (config "mods" "1"); the case opens without a key')


if __name__ == "__main__":
    main()

r"""A sticker from any picture (CSForge mod "stickers").

    python tools\make_sticker_mod.py --data release\cs2_gc --image cat.png --name "Cat"
    python tools\make_sticker_mod.py --data release\cs2_gc --image cat.png --name "Cat" --rarity legendary --add-item 3
    python tools\make_sticker_mod.py --data release\cs2_gc --image cat.png --name "Cat (Holo)" --finish holo --mask cat_eyes.png
    python tools\make_sticker_mod.py --data release\cs2_gc --list
    python tools\make_sticker_mod.py --data release\cs2_gc --remove cs2gc_cat

The picture (a PNG with transparency works best: what's transparent stays bare gun) is fitted into the
square sticker texture. --finish picks the kind of sticker:

    paper    a plain paper sticker
    holo     holographic: a rainbow that shifts with the viewing angle
    glitter  glitter that sparkles
    foil     metallic: the picture's colors on shiny metal
    gold     metallic, the picture in gold

The finishes are switches of the game's sticker shader in the compiled material (g_bHolographicSticker0,
g_bGlitterSticker0, g_bMetallicSticker0), which these tools don't rewrite. So each finish starts from a
sticker of the game that has it (FINISHES) and the mod's copy of its material gets our textures in place
of that sticker's: the picture (g_tSticker0); a mask of where the effect is (g_tSfxMaskSticker0: holo reads
red for the amount and green/blue for where in the rainbow, glitter reads alpha), by default the whole
sticker, or --mask (white = the effect, the same size as the picture); a flat normal with the finish's
roughness (g_tNormalRoughnessSticker0: red/green the normal, blue the roughness). The rest (the rainbow,
the glitter flakes, scratches) is the game's. The inventory icon only hints at the finish: the game's icons
are rendered with the shader, this one is drawn from the picture.

The mod has the material copy, the textures, the icon and the sticker kit with its name. --add-item N puts N
of the sticker into inventory.txt (the game must be closed); apply it to a weapon in the game as usual. The
game files are only read. Not checked in the game yet.
"""
import argparse
import json
import os
import re
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_skin_mod as msm  # noqa: E402
import mesh_inputs as mi  # noqa: E402
import source2 as s2  # noqa: E402
from gen_invedit_catalog import parse_kv, to_dict  # noqa: E402
from make_music_mod import add_inventory_item, cs2_running, kv_string  # noqa: E402

FIRST_ID = 30000  # the game's sticker kits end at about 11800
SIZE = 1024  # the game's sticker textures
ICON_W, ICON_H = 256, 192
STICKER_DEF = 1209  # the "sticker" item, attribute 113 is its kit
RARITIES = ["common", "uncommon", "rare", "mythical", "legendary", "ancient"]
DEFAULT_TEMPLATE = "broken_fang_ancient_marauder"

# per finish: the game's sticker whose material is copied, the textures we put in its place, the roughness
# of the flat normal map (blue, as in the game's maps of such stickers) and the usual rarity
FINISHES = {
    "paper": {"template": DEFAULT_TEMPLATE, "swap": ("g_tSticker0",), "roughness": None, "rarity": "rare"},
    "holo": {"template": "community2021_cyber_romanov_holo", "swap": ("g_tSticker0", "g_tSfxMaskSticker0", "g_tNormalRoughnessSticker0"),
             "roughness": 28, "rarity": "mythical"},
    "glitter": {"template": "community2022_snow_glitter", "swap": ("g_tSticker0", "g_tSfxMaskSticker0", "g_tNormalRoughnessSticker0"),
                "roughness": 27, "rarity": "mythical"},
    "foil": {"template": "community2021_ace_devil_foil", "swap": ("g_tSticker0", "g_tNormalRoughnessSticker0"),
             "roughness": 50, "rarity": "legendary"},
    "gold": {"template": "community2021_ace_devil_foil", "swap": ("g_tSticker0", "g_tNormalRoughnessSticker0"),
             "roughness": 40, "rarity": "ancient"},
}
GOLD = np.array([1.0, 0.80, 0.45])  # the tone of the game's gold stickers, times the picture's brightness


def find_cs2():
    from skin_editor import find_cs2 as find
    return find()


class Stickers:
    def __init__(self, data, name):
        self.data = data
        self.dir = os.path.join(data, "mods", name)
        self.manifest_path = os.path.join(self.dir, "stickers.json")
        self.stickers = {}
        if os.path.exists(self.manifest_path):
            self.stickers = json.load(open(self.manifest_path, encoding="utf-8"))

    def write_file(self, path, blob):
        out = os.path.join(self.dir, "files", *path.split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(blob)

    def remove_files(self, kit):
        for path in self.stickers.get(kit, {}).get("files", []):
            full = os.path.join(self.dir, "files", *path.split("/"))
            if os.path.exists(full):
                os.remove(full)

    def other_ids(self):
        ids = set()
        mods = os.path.join(self.data, "mods")
        for name in os.listdir(mods) if os.path.isdir(mods) else []:
            path = os.path.join(mods, name, "items_game.txt")
            if os.path.join(mods, name) == self.dir or not os.path.exists(path):
                continue
            try:
                tree = to_dict(parse_kv(open(path, encoding="utf-8", errors="replace").read()))
            except Exception:
                continue
            ids |= {int(k) for k in tree.get("items_game", {}).get("sticker_kits", {}) if k.isdigit()}
        return ids

    def save(self, mod_name, priority):
        os.makedirs(self.dir, exist_ok=True)
        with open(self.manifest_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.stickers, f, ensure_ascii=False, indent=1)
        kits = sorted(self.stickers.items(), key=lambda kv: kv[1]["id"])
        note = "// made by tools\\make_sticker_mod.py from stickers.json (edits here get overwritten)\n"
        with open(os.path.join(self.dir, "items_game.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(note + '"items_game"\n{\n\t"sticker_kits"\n\t{\n')
            for kit, s in kits:
                f.write(f'\t\t"{s["id"]}"\n\t\t{{\n\t\t\t"name"\t\t"{kit}"\n'
                        f'\t\t\t"item_name"\t\t"#StickerKit_{kit}"\n\t\t\t"description_string"\t\t"#StickerKit_desc_{kit}"\n'
                        f'\t\t\t"sticker_material"\t\t"cs2gc/{kit}"\n\t\t\t"item_rarity"\t\t"{s["rarity"]}"\n\t\t}}\n')
            f.write("\t}\n}\n")
        with open(os.path.join(self.dir, "localization.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("// made by tools\\make_sticker_mod.py\n\"lang\"\n{\n\t\"Tokens\"\n\t{\n")
            for kit, s in kits:
                f.write(f'\t\t"StickerKit_{kit}"\t\t"{kv_string(s["name"])}"\n')
                f.write(f'\t\t"StickerKit_desc_{kit}"\t\t"{kv_string(s.get("description", ""))}"\n')
            f.write("\t}\n}\n")
        path = os.path.join(self.dir, "mod.txt")
        if not os.path.exists(path):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("// made by tools\\make_sticker_mod.py\n\"mod\"\n{\n")
                f.write(f'\t"name"\t\t"{kv_string(mod_name)}"\n\t"description"\t"stickers made from pictures"\n')
                f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{priority}"\n}}\n')


def fit(picture, w, h, box):
    """The picture fitted into a box x box square in the middle of a transparent w x h image."""
    img = Image.open(picture).convert("RGBA")
    img.thumbnail((box, box), Image.LANCZOS)
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    canvas.paste(img, ((w - img.width) // 2, (h - img.height) // 2))
    return np.asarray(canvas)


def template_params(game, template):
    """{texture param: path} of a sticker material of the game."""
    kv = s2.KV3(s2.Resource.parse(game.read(template + "_c")).block("DATA")).decode()
    return {t["m_name"]: t["m_pValue"] for t in kv["m_textureParams"]}


def sticker_material(game, template, textures, material_path):
    """The template's material with the textures {param: path} and its own name swapped (KV3 and RERL),
    everything else (the shader's switches and values) as the game has it."""
    res = s2.Resource.parse(game.read(template + "_c"))
    kv = s2.KV3(res.block("DATA"))
    before = kv.decode()
    params = {t["m_name"]: t["m_pValue"] for t in before["m_textureParams"]}
    swaps = {}
    for param, path in textures.items():
        old = params.get(param)
        if not old or [n for n, p in params.items() if p == old] != [param]:
            raise ValueError(f"{template}: its {param} can't be swapped on its own")
        swaps[old] = path
    if before.get("m_materialName") == template:
        swaps[template] = material_path
    if kv.replace_strings(swaps) != len(swaps):
        raise ValueError(f"{template}: the strings to swap aren't there once each")
    res.set_block("DATA", kv.build())
    if res.blocks[0][0] != "RERL":
        raise ValueError(f"{template}: RERL isn't the first block")
    refs = [swaps.get(p, p) for _, p in s2.read_rerl(res.block("RERL"))]
    res.set_block("RERL", s2.build_rerl(refs, 16 + 12 * len(res.blocks)))
    blob = res.build()
    after = s2.KV3(s2.Resource.parse(blob).block("DATA")).decode()
    params.update(textures)
    if {t["m_name"]: t["m_pValue"] for t in after["m_textureParams"]} != params or \
            any(after[k] != before[k] for k in before if k not in ("m_textureParams", "m_materialName")):
        raise ValueError(f"{template}: the swapped material didn't come out right")
    return blob


def effect_mask(rgba, mask_picture):
    """Where the effect is, 0..1: the mask picture's brightness (fitted like the picture) inside the
    sticker, or the whole sticker."""
    alpha = rgba[..., 3].astype(np.float64) / 255
    if not mask_picture:
        return alpha
    m = fit(mask_picture, SIZE, SIZE, SIZE).astype(np.float64) / 255
    return (m[..., :3] @ [0.299, 0.587, 0.114]) * m[..., 3] * alpha


def finish_textures(rgba, finish, mask_picture):
    """{param: RGBA uint8 array} of the textures the finish needs, made from the picture."""
    spec = FINISHES[finish]
    color = rgba.copy()
    lum = rgba[..., :3].astype(np.float64) @ [0.299, 0.587, 0.114] / 255
    if finish == "gold":
        color[..., :3] = np.clip(np.rint(GOLD * (0.35 + 0.65 * lum[..., None]) * 255), 0, 255)
    out = {"g_tSticker0": color}
    effect = effect_mask(rgba, mask_picture)
    if finish == "holo":
        # red: how much rainbow, green and blue: where in it (the picture's brightness, so its parts shift apart)
        mask = np.zeros_like(rgba)
        mask[..., 0] = np.rint(effect * 255)
        mask[..., 1] = mask[..., 2] = np.rint(lum * 255)
        mask[..., 3] = 255
        out["g_tSfxMaskSticker0"] = mask
    elif finish == "glitter":
        mask = np.zeros_like(rgba)
        mask[..., 3] = np.rint(effect * 255)
        out["g_tSfxMaskSticker0"] = mask
    if spec["roughness"] is not None:
        normal = np.zeros((64, 64, 4), np.uint8)
        normal[..., 0] = normal[..., 1] = 128
        normal[..., 2] = spec["roughness"]
        normal[..., 3] = 255
        out["g_tNormalRoughnessSticker0"] = normal
    return out, effect


def encode_plain(rgba, red2):
    """Uncompressed BGRA with all mips: the flat normal map has to stay exactly 128 (DXT1 makes it 132)."""
    img = Image.fromarray(np.ascontiguousarray(rgba), "RGBA")
    w, h = img.size
    mips = []
    while True:
        m = np.asarray(img.resize((w, h), Image.BILINEAR))
        mips.append((m[..., [2, 1, 0, 3]].tobytes(), w, h))
        if w <= 4 or h <= 4:
            break
        w, h = w // 2, h // 2
    return s2.build_texture(None, s2.FMT_BGRA8888, mips, 0, s2.fallback_bits(rgba), red2)


def icon_look(rgba, finish, effect):
    """The picture with a hint of the finish for the inventory icon (the game's icons are renders)."""
    img = rgba.astype(np.float64)
    h, w = img.shape[:2]
    y, x = np.mgrid[0:h, 0:w] / max(h, w)
    e = effect[..., None]
    if finish == "gold":
        img[..., :3] = finish_textures(rgba, "gold", None)[0]["g_tSticker0"][..., :3]
    if finish in ("holo",):
        hue = (x + y) * 1.5 + rgba[..., :3].mean(-1) / 255
        rainbow = 0.5 + 0.5 * np.cos(2 * np.pi * (hue[..., None] + np.array([0, 1 / 3, 2 / 3])))
        img[..., :3] = img[..., :3] * (1 - 0.45 * e) + rainbow * 255 * 0.45 * e
    if finish in ("foil", "gold"):
        shine = np.clip(1 - np.abs((x - y) * 3 - 0.2), 0, 1) ** 2
        img[..., :3] = img[..., :3] * 0.85 + shine[..., None] * 255 * 0.35
    if finish == "glitter":
        # flakes of 8 texels, so they're still there in the 192 pixel icon
        rng = np.random.default_rng(1)
        flakes = rng.random(((h + 7) // 8, (w + 7) // 8)) ** 6
        flakes = np.kron(flakes, np.ones((8, 8)))[:h, :w] * e[..., 0]
        img[..., :3] = img[..., :3] * (1 - 0.8 * flakes[..., None]) + 255 * 0.8 * flakes[..., None]
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


def make_sticker(game, stickers, args, used, log=print):
    kit = "cs2gc_" + (msm.slug(args.kit_name or args.name) if (args.kit_name or msm.slug(args.name) != "skin") else "sticker")
    if kit == "cs2gc_sticker" or (kit in stickers.stickers and not args.kit_name and stickers.stickers[kit]["name"] != args.name):
        kit = next(f"{kit}{i}" for i in range(1, 10000) if f"{kit}{i}" not in stickers.stickers)
    if not re.fullmatch(r"[a-z0-9_]{1,60}", kit):
        sys.exit(f"bad internal name {kit}: Latin letters, digits, _")

    finish = getattr(args, "finish", None) or "paper"
    if finish not in FINISHES:
        sys.exit(f"no finish {finish}: {', '.join(FINISHES)}")
    template_name = args.template if finish == "paper" and getattr(args, "template", None) else FINISHES[finish]["template"]
    kits = game.items_game.get("sticker_kits", {})
    template_kit = next((k for k in kits.values() if k.get("name") == template_name), None)
    if not template_kit:
        sys.exit(f"the game has no sticker kit named {template_name}")
    template = "stickers/" + template_kit["sticker_material"] + ".vmat"

    old = stickers.stickers.get(kit)
    if old:
        stickers.remove_files(kit)
    sticker_id = old["id"] if old else next(i for i in range(FIRST_ID, 65000) if i not in used)
    used.add(sticker_id)

    material_path = f"stickers/cs2gc/{kit}.vmat"
    icon_path = f"panorama/images/econ/stickers/cs2gc/{kit}_png.vtex"
    params = template_params(game, template)
    template_icon = "panorama/images/econ/stickers/" + template_kit["sticker_material"] + "_png.vtex_c"
    names = {"g_tSticker0": "psd", "g_tSfxMaskSticker0": "mask_psd", "g_tNormalRoughnessSticker0": "normal_psd"}

    rgba = fit(args.image, SIZE, SIZE, SIZE)
    made, effect = finish_textures(rgba, finish, getattr(args, "mask", None))
    files, textures = {}, {}
    for param in FINISHES[finish]["swap"]:
        path = f"stickers/cs2gc/{kit}_{names[param]}.vtex"
        red2 = s2.Resource.parse(game.read(params[param] + "_c")).block("RED2")
        encode = encode_plain if param == "g_tNormalRoughnessSticker0" else mi.encode_color
        files[path + "_c"] = encode(made[param], red2)
        textures[param] = path
    files[material_path + "_c"] = sticker_material(game, template, textures, material_path)
    icon = icon_look(rgba, finish, effect)
    icon_img = Image.fromarray(icon, "RGBA")
    icon_img.thumbnail((ICON_H, ICON_H), Image.LANCZOS)
    canvas = Image.new("RGBA", (ICON_W, ICON_H), (0, 0, 0, 0))
    canvas.paste(icon_img, ((ICON_W - icon_img.width) // 2, (ICON_H - icon_img.height) // 2))
    files[icon_path + "_c"] = msm.encode_icon(np.asarray(canvas), game.read(template_icon) if template_icon in game.entries else None)
    for path, blob in files.items():
        stickers.write_file(path, blob)
    stickers.stickers[kit] = {"id": sticker_id, "name": args.name, "description": args.description, "rarity": args.rarity,
                              "finish": finish, "image": os.path.abspath(args.image), "template": template_name,
                              "files": sorted(files)}
    if getattr(args, "mask", None):
        stickers.stickers[kit]["mask"] = os.path.abspath(args.mask)
    log(f'{"rebuilt" if old else "new"} {finish} sticker {kit} "{args.name}": kit {sticker_id}, '
        f'{sum(len(b) for b in files.values()) // 1024} KB')
    return kit, sticker_id


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cs2", help="CS2 folder (found through Steam when not given)")
    ap.add_argument("--data", required=True, help="the cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--image", help="the picture")
    ap.add_argument("--name", help="the sticker's name in game")
    ap.add_argument("--kit-name", help="internal name (Latin letters, digits, _), by default made from --name")
    ap.add_argument("--description", default="")
    ap.add_argument("--finish", default="paper", choices=list(FINISHES))
    ap.add_argument("--mask", help="holo and glitter: where the effect is (white), the same size as the picture; default the whole sticker")
    ap.add_argument("--rarity", choices=RARITIES, help="default the finish's usual one (paper rare, holo and glitter mythical, foil legendary, gold ancient)")
    ap.add_argument("--template", default=DEFAULT_TEMPLATE, help="paper: a paper sticker of the game whose material is copied")
    ap.add_argument("--add-item", type=int, nargs="?", const=1, default=0, metavar="N")
    ap.add_argument("--mod", default="stickers")
    ap.add_argument("--mod-name", default="Custom stickers")
    ap.add_argument("--priority", type=int, default=10)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--remove", metavar="KIT")
    args = ap.parse_args()

    stickers = Stickers(args.data, args.mod)
    if args.list:
        for kit, s in sorted(stickers.stickers.items(), key=lambda kv: kv[1]["id"]):
            print(f'{kit}: "{s["name"]}", {s.get("finish", "paper")}, kit {s["id"]}, {s["rarity"]}')
        if not stickers.stickers:
            print(f"no stickers in {stickers.dir}")
        return
    if args.remove:
        if args.remove not in stickers.stickers:
            sys.exit(f"no sticker {args.remove} (see --list)")
        stickers.remove_files(args.remove)
        del stickers.stickers[args.remove]
        stickers.save(args.mod_name, args.priority)
        print(f"removed {args.remove}; stickers of it in inventory.txt stay and show as unknown")
        return
    if not args.image or not args.name:
        ap.error("--image and --name (or --list, --remove)")
    args.rarity = args.rarity or FINISHES[args.finish]["rarity"]

    cs2 = args.cs2 or find_cs2()
    if not cs2:
        sys.exit("CS2 not found, pass --cs2")
    game = msm.Game(cs2)
    used = {int(k) for k in game.items_game.get("sticker_kits", {}) if k.isdigit()} | stickers.other_ids() \
        | {s["id"] for s in stickers.stickers.values()}
    kit, sticker_id = make_sticker(game, stickers, args, used)
    stickers.save(args.mod_name, args.priority)
    print(f"wrote {stickers.dir}")
    if args.add_item:
        if cs2_running():
            print("the game is running, not touching inventory.txt (close it and run again with --add-item)")
        else:
            rarity = game.rarity_value(args.rarity)
            inventory = os.path.join(args.data, "inventory.txt")
            ids = [add_inventory_item(inventory, STICKER_DEF, 4, rarity, [(113, sticker_id)], ".before_sticker_mod")
                   for _ in range(args.add_item)]
            print(f"added {len(ids)} of the sticker to the inventory (backup: inventory.txt.before_sticker_mod)")
    print('start the game through run_cs2_gc.cmd (config "mods" "1")')


if __name__ == "__main__":
    main()

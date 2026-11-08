"""Generate the CS2 catalogue for the inventory editor (editor/catalog.json + editor/icons.json).

    python tools/gen_invedit_catalog.py --cs2 "<CS2 install dir>" [--lang russian] [--icons <dir>|--no-icons]

Reads scripts/items/items_game.txt and resource/csgo_<lang>.txt straight out of
game\\csgo\\pak01_dir.vpk (read only). The output follows the catalogue format of
mikkokko's csgo_invedit (formatVersion 1), extended with charms (keychains).

Icons: CS2 keeps item icons as compiled GPU textures, so the editor uses Steam
economy image urls instead. They come from ByMykel's CSGO-API json files
(https://github.com/ByMykel/CSGO-API). With --icons <dir> they are read from a
folder, otherwise downloaded. Rerun after CS2 updates.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vpk_extract  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BYMYKEL = "https://raw.githubusercontent.com/ByMykel/CSGO-API/main/public/api/en/"

# item ids the GC templates use
DEF_STICKER = 1209
DEF_GRAFFITI = 1348
DEF_PATCH = 4609
DEF_MUSIC = 1314

ATTR_PAINT, ATTR_SEED, ATTR_WEAR = 6, 7, 8
ATTR_KILLEATER, ATTR_KILLEATER_TYPE = 80, 81
ATTR_STICKER, ATTR_MUSIC, ATTR_SPRAY_TINT = 113, 166, 233
ATTR_KEYCHAIN = 299

INVENTORY_NEW = 0x40000005  # same "unacknowledged" value csgo_invedit used
ORIGIN_CRATE = 8

# --------------------------------------------------------------- keyvalues

TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|(\{)|(\})|//[^\n]*|\[[^\]\n]*\]|([^\s"{}\[\]]+)', re.S)


def parse_kv(text):
    """KeyValues1 -> nested lists of (key, value) pairs (duplicates kept)."""
    stack = [[]]
    key = None
    for m in TOKEN.finditer(text):
        quoted, opening, closing, bare = m.groups()
        if opening:
            node = []
            stack[-1].append((key, node))
            stack.append(node)
            key = None
        elif closing:
            if len(stack) > 1:
                stack.pop()
            key = None
        elif quoted is not None or bare is not None:
            value = quoted if quoted is not None else bare
            if quoted is not None:
                value = value.replace('\\"', '"').replace("\\n", "\n").replace("\\\\", "\\")
            if key is None:
                key = value
            else:
                stack[-1].append((key, value))
                key = None
        # comments and [$CONDITIONALS] are skipped
    return stack[0]


def to_dict(pairs):
    """Merge duplicate keys: blocks merge recursively, strings: last wins."""
    out = {}
    for key, value in pairs:
        if isinstance(value, list):
            value = to_dict(value)
            if isinstance(out.get(key), dict):
                out[key] = deep_merge(out[key], value)
                continue
        out[key] = value
    return out


def deep_merge(base, over):
    result = dict(base)
    for key, value in over.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = deep_merge(result[key], value)
        else:
            result[key] = value
    return result


def load_localization(raw):
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("utf-8-sig", "replace")
    root = to_dict(parse_kv(text))
    lang = next((v for v in root.values() if isinstance(v, dict)), {})
    tokens = lang.get("Tokens", lang.get("tokens", {}))
    return {k.lower(): v for k, v in tokens.items() if isinstance(v, str)}


# ------------------------------------------------------------------ schema

class Schema:
    def __init__(self, items_game, loc, loc_en):
        self.g = items_game
        self.loc_tokens = loc
        self.loc_en = loc_en
        self.prefabs = items_game.get("prefabs", {})
        self._resolved_prefabs = {}

    def loc(self, token, english=False):
        if not token:
            return ""
        key = token.lstrip("#").lower()
        if not english and key in self.loc_tokens:
            return self.loc_tokens[key]
        return self.loc_en.get(key, token.lstrip("#") if not token.startswith("#") else "")

    def prefab(self, name):
        if name not in self._resolved_prefabs:
            self._resolved_prefabs[name] = {}  # recursion guard
            self._resolved_prefabs[name] = self.resolve(self.prefabs.get(name, {}))
        return self._resolved_prefabs[name]

    def resolve(self, item):
        base = {}
        chain = []
        for name in str(item.get("prefab", "")).split():
            parent = self.prefab(name)
            chain += parent.get("_chain", []) + [name]
            base = deep_merge(base, parent)
        result = deep_merge(base, item)
        result["_chain"] = chain
        return result


def rarity_value(schema, name, default=1):
    entry = schema.g.get("rarities", {}).get(name)
    return int(entry["value"]) if entry and "value" in entry else default


def quality_value(schema, name, default=4):
    entry = schema.g.get("qualities", {}).get(name)
    return int(entry["value"]) if entry and "value" in entry else default


def painted_rarity(item_rarity, paint_rarity):
    # same as the GC (PaintedItemRarity)
    value = item_rarity - 1 + paint_rarity
    if value < 0:
        return 0
    if value > 6:
        return 7 if paint_rarity == 7 else 6
    return value


def classify(defindex, item):
    name = item.get("name", "")
    item_class = item.get("item_class", "")
    chain = set(item.get("_chain", []))
    tool = item.get("tool", {}) if isinstance(item.get("tool"), dict) else {}
    tool_type = tool.get("type", "")
    if name.startswith("weapon_knife") or name == "weapon_bayonet" or "melee_unusual" in chain:
        return "knives"
    if item_class == "wearable_item" and ("hands" in chain or "gloves" in name or "handwraps" in name):
        return "gloves"
    if name.startswith("customplayer_") or "customplayertradable" in chain:
        return "agents"
    if name.startswith("musickit"):
        return "music"
    if defindex == DEF_STICKER or tool_type == "sticker":
        return "stickers"
    if tool_type == "keychain" or name == "keychain":
        return "keychains"
    if tool_type in ("spray", "spray_tint") or name.startswith("spray"):
        return "graffiti"
    if tool_type == "patch" or name == "patch":
        return "patches"
    if item_class.startswith("weapon_"):
        return "weapons"
    if item_class == "supply_crate":
        return "containers"
    if tool_type == "decoder_ring":
        return "keys"
    if item_class == "collectible_item" or "collectible" in name.lower() or "coin" in chain or "pin" in name.lower():
        return "collectibles"
    if item_class == "tool":
        return "tools"
    return "other"


def norm(text):
    return re.sub(r"[\s_|#]+", " ", str(text).lower()).strip()


def template(defindex, quality, rarity, attributes=(), inventory=INVENTORY_NEW, origin=ORIGIN_CRATE):
    return {
        "attributes": [{"defIndex": a, "value": str(v)} for a, v in attributes],
        "defIndex": defindex, "equips": [], "flags": 0, "inUse": 0,
        "inventory": inventory, "level": 1, "origin": origin, "quality": quality, "rarity": rarity,
    }


def record(key, name, english, defindex, category, kind, kit, rarity, quality, internal, tmpl, extra=None):
    aliases = sorted({str(kit), str(defindex), name, english, *internal})
    rec = {
        "key": key, "name": name, "aliases": aliases, "defIndex": defindex, "category": category,
        "kind": kind, "kitId": kit, "rarity": rarity, "quality": quality, "template": tmpl,
        "searchText": norm(" ".join(aliases)), "creationPolicy": "crate",
    }
    if extra:
        rec.update(extra)
    return rec


# ---------------------------------------------------------------- building

def build_catalog(schema, icon_files):
    g = schema.g
    items = {int(k): schema.resolve(v) for k, v in g.get("items", {}).items() if k.isdigit()}
    definitions, variants = [], []

    def item_name(defindex, english=False):
        item = items.get(defindex, {})
        return schema.loc(item.get("item_name", ""), english) or item.get("name", str(defindex))

    by_name = {}
    for defindex, item in sorted(items.items()):
        category = classify(defindex, item)
        name, english = item_name(defindex), item_name(defindex, True)
        hidden = not schema.loc(item.get("item_name", "")) or item.get("hidden") == "1" or category == "other"
        base_item = item.get("baseitem") == "1"
        quality = quality_value(schema, item.get("item_quality", "unique"), 4)
        rarity = rarity_value(schema, item.get("item_rarity", "common"), 1)
        if category in ("knives", "gloves"):
            quality, rarity = 3, 6
        by_name[item.get("name", "")] = defindex
        aliases = sorted({str(defindex), name, english, item.get("name", ""), schema.loc(item.get("item_type_name", ""))} - {""})
        definitions.append({
            "key": f"def:{defindex}", "defIndex": defindex, "name": name, "internalName": item.get("name", ""),
            "category": category, "advancedOnly": bool(hidden), "baseItem": base_item, "aliases": aliases,
            "searchText": norm(" ".join(aliases)), "typeName": schema.loc(item.get("item_type_name", "")),
            "creationPolicy": "base" if base_item else "dropped",
            "template": template(defindex, 0 if base_item else quality, rarity,
                                 inventory=0x40000000 if base_item else 0x40000001, origin=22 if base_item else 0),
        })

    # paint kits
    paint_kits = {}
    for pid, kit in g.get("paint_kits", {}).items():
        if not pid.isdigit():
            continue
        tag = kit.get("description_tag", "")
        paint_kits[kit.get("name", "")] = {
            "id": int(pid), "internalName": kit.get("name", ""),
            "name": schema.loc(tag) or kit.get("name", ""), "english": schema.loc(tag, True) or kit.get("name", ""),
            "wearMin": float(kit.get("wear_remap_min", 0.0)), "wearMax": float(kit.get("wear_remap_max", 1.0)),
        }
    paint_rarity = {k: rarity_value(schema, v, 1) for k, v in g.get("paint_kits_rarity", {}).items() if isinstance(v, str)}

    # every weapon+paint combo with a generated icon exists in game
    weapon_names = sorted((n for n, d in by_name.items() if n and classify(d, items[d]) in ("weapons", "knives", "gloves")), key=len, reverse=True)
    combos = set()
    for path in icon_files:
        m = re.match(r"panorama/images/econ/default_generated/(.+)_light_png\.vtex_c$", path)
        if not m:
            continue
        base = m.group(1)
        for weapon in weapon_names:
            if base.startswith(weapon + "_") and base[len(weapon) + 1:] in paint_kits:
                combos.add((by_name[weapon], base[len(weapon) + 1:]))
                break
    # collections list combos too
    for set_def in g.get("item_sets", {}).values():
        for entry in (set_def.get("items", {}) if isinstance(set_def, dict) else {}):
            m = re.match(r"\[([^\]]+)\](.+)", entry)
            if m and m.group(2) in by_name and m.group(1) in paint_kits:
                combos.add((by_name[m.group(2)], m.group(1)))

    for defindex, paint_name in sorted(combos):
        item, kit = items[defindex], paint_kits[paint_name]
        category = classify(defindex, item)
        star = "★ " if category in ("knives", "gloves") else ""
        name = f"{star}{item_name(defindex)} | {kit['name']}"
        english = f"{star}{item_name(defindex, True)} | {kit['english']}"
        if category in ("knives", "gloves"):
            quality, rarity = 3, 6
        else:
            quality = 4
            rarity = painted_rarity(rarity_value(schema, item.get("item_rarity", "common"), 1), paint_rarity.get(paint_name, 1))
        attrs = [(ATTR_PAINT, kit["id"]), (ATTR_SEED, 0), (ATTR_WEAR, kit["wearMin"])]
        tmpl = template(defindex, quality, rarity, attrs)
        extra = {"wearMin": kit["wearMin"], "wearMax": kit["wearMax"], "statTrakAvailable": category != "gloves"}
        if category != "gloves":
            extra["statTrakTemplate"] = template(defindex, quality if category == "knives" else 9, rarity,
                                                 attrs + [(ATTR_KILLEATER, 0), (ATTR_KILLEATER_TYPE, 0)])
        variants.append(record(f"variant:{defindex}:paint:{kit['id']}", name, english, defindex, category, "paint",
                               kit["id"], rarity, quality, [item.get("name", ""), paint_name], tmpl, extra))

    # stickers, graffiti, patches
    sticker_kits = []
    for sid, kit in g.get("sticker_kits", {}).items():
        if not sid.isdigit() or sid == "0" or not isinstance(kit, dict):
            continue
        token = kit.get("item_name", "")
        lower = token.lower()
        if lower.startswith("#spraykit"):
            kind, defindex, category, extra_attrs = "graffiti", DEF_GRAFFITI, "graffiti", [(ATTR_SPRAY_TINT, 1)]
        elif lower.startswith("#patchkit"):
            kind, defindex, category, extra_attrs = "patch", DEF_PATCH, "patches", []
        else:
            kind, defindex, category, extra_attrs = "sticker", DEF_STICKER, "stickers", []
        kit_name, kit_en = schema.loc(token) or kit.get("name", ""), schema.loc(token, True) or kit.get("name", "")
        rarity = rarity_value(schema, kit.get("item_rarity", "common"), 1)
        sticker_kits.append({"id": int(sid), "kind": kind, "name": kit_name, "internalName": kit.get("name", ""), "rarity": rarity})
        name, english = f"{item_name(defindex)} | {kit_name}", f"{item_name(defindex, True)} | {kit_en}"
        tmpl = template(defindex, 4, rarity, [(ATTR_STICKER, int(sid))] + extra_attrs)
        variants.append(record(f"variant:{defindex}:{kind}:{sid}", name, english, defindex, category, kind,
                               int(sid), rarity, 4, [kit.get("name", "")], tmpl))

    # charms
    keychain_def = by_name.get("keychain")
    keychains = []
    for kid, kc in g.get("keychain_definitions", {}).items():
        if not kid.isdigit() or not isinstance(kc, dict) or keychain_def is None:
            continue
        kit_name, kit_en = schema.loc(kc.get("loc_name", "")) or kc.get("name", ""), schema.loc(kc.get("loc_name", ""), True) or kc.get("name", "")
        rarity = rarity_value(schema, kc.get("item_rarity", "common"), 1)
        keychains.append({"id": int(kid), "kind": "keychain", "name": kit_name, "internalName": kc.get("name", ""), "rarity": rarity})
        name, english = f"{item_name(keychain_def)} | {kit_name}", f"{item_name(keychain_def, True)} | {kit_en}"
        tmpl = template(keychain_def, 4, rarity, [(ATTR_KEYCHAIN, int(kid))])
        variants.append(record(f"variant:{keychain_def}:keychain:{kid}", name, english, keychain_def, "keychains",
                               "keychain", int(kid), rarity, 4, [kc.get("name", "")], tmpl))

    # music kits
    music = []
    for mid, md in g.get("music_definitions", {}).items():
        if not mid.isdigit() or not isinstance(md, dict):
            continue
        kit_name, kit_en = schema.loc(md.get("loc_name", "")) or md.get("name", ""), schema.loc(md.get("loc_name", ""), True) or md.get("name", "")
        music.append({"id": int(mid), "name": kit_name, "internalName": md.get("name", "")})
        name, english = f"{item_name(DEF_MUSIC)} | {kit_name}", f"{item_name(DEF_MUSIC, True)} | {kit_en}"
        attrs = [(ATTR_MUSIC, int(mid))]
        tmpl = template(DEF_MUSIC, 4, 3, attrs)
        extra = {"statTrakAvailable": True,
                 "statTrakTemplate": template(DEF_MUSIC, 9, 3, attrs + [(ATTR_KILLEATER, 0), (ATTR_KILLEATER_TYPE, 1)])}
        variants.append(record(f"variant:{DEF_MUSIC}:music:{mid}", name, english, DEF_MUSIC, "music", "music",
                               int(mid), 3, 4, [md.get("name", "")], tmpl, extra))

    attributes = [{"id": int(k), "name": v.get("name", "")} for k, v in g.get("attributes", {}).items() if k.isdigit() and isinstance(v, dict)]

    catalog = {
        "formatVersion": 1,
        "exporterVersion": 1,
        "game": "cs2",
        "language": None,
        "definitions": definitions,
        "variants": variants,
        "attributes": attributes,
        "paintKits": [{k: v for k, v in kit.items() if k != "english"} for kit in sorted(paint_kits.values(), key=lambda x: x["id"])],
        "stickerKits": sticker_kits + keychains,
        "musicDefinitions": music,
        "rarities": g.get("rarities", {}),
        "colors": g.get("colors", {}),
    }
    return catalog


def build_icons(catalog, icon_dir):
    def load(name):
        if icon_dir:
            path = os.path.join(icon_dir, name + ".json")
            if os.path.exists(path):
                with open(path, encoding="utf-8") as f:
                    return json.load(f)
        print(f"downloading {name}.json")
        with urllib.request.urlopen(BYMYKEL + name + ".json", timeout=300) as response:
            return json.loads(response.read().decode("utf-8"))

    images, image_keys = {}, {}

    def image(url):
        if not url:
            return None
        if url not in image_keys:
            key = f"i{len(image_keys)}"
            image_keys[url] = key
            images[key] = url
        return image_keys[url]

    category_of = {d["defIndex"]: d["category"] for d in catalog["definitions"]}
    definitions = {str(d): {"kind": c, "image": None} for d, c in category_of.items()}
    paint, kits = {}, {}

    def set_def(defindex, url):
        key = str(defindex)
        if key in definitions and not definitions[key]["image"]:
            definitions[key]["image"] = image(url)

    for name in ("base_weapons", "agents", "crates", "collectibles", "keys", "tools"):
        for entry in load(name):
            if str(entry.get("def_index", "")).isdigit():
                set_def(int(entry["def_index"]), entry.get("image"))

    wear_slot = {"SFUI_InvTooltip_Wear_Amount_0": 0, "SFUI_InvTooltip_Wear_Amount_1": 0,
                 "SFUI_InvTooltip_Wear_Amount_2": 1, "SFUI_InvTooltip_Wear_Amount_3": 2, "SFUI_InvTooltip_Wear_Amount_4": 2}
    for entry in load("skins_not_grouped"):
        weapon, paint_index = entry.get("weapon") or {}, entry.get("paint_index")
        if not weapon.get("weapon_id") or not str(paint_index or "").isdigit():
            continue
        slots = paint.setdefault(f"{weapon['weapon_id']}:{int(paint_index)}", [None, None, None])
        slot = wear_slot.get((entry.get("wear") or {}).get("id"), 0)
        if not slots[slot]:
            slots[slot] = image(entry.get("image"))
    for slots in paint.values():
        fallback = next((s for s in slots if s), None)
        for i in range(3):
            slots[i] = slots[i] or fallback

    for name, kind in (("stickers", "sticker"), ("graffiti", "graffiti"), ("patches", "patch"),
                       ("music_kits", "music"), ("keychains", "keychain")):
        for entry in load(name):
            kid = str(entry.get("def_index", ""))
            if kid.isdigit() and f"{kind}:{kid}" not in kits and entry.get("image"):
                kits[f"{kind}:{int(kid)}"] = image(entry["image"])

    # base images for the tool definitions (sticker, charm...) when nothing else matched
    for defindex, cat in category_of.items():
        key = str(defindex)
        if definitions[key]["image"]:
            continue
        sample = {"stickers": "sticker:", "graffiti": "graffiti:", "patches": "patch:",
                  "music": "music:", "keychains": "keychain:"}.get(cat)
        if sample:
            definitions[key]["image"] = next((v for k, v in kits.items() if k.startswith(sample)), None)

    return {"formatVersion": 1, "exporterVersion": 1, "images": images,
            "definitions": definitions, "paint": paint, "kits": kits}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cs2", required=True, help="CS2 install dir (the folder that contains game\\)")
    parser.add_argument("--lang", default="english", help="localization: english, russian, ...")
    parser.add_argument("--out", default=os.path.join(ROOT, "editor"))
    parser.add_argument("--icons", help="folder with ByMykel CSGO-API json files (downloaded otherwise)")
    parser.add_argument("--no-icons", action="store_true")
    args = parser.parse_args()

    vpk = os.path.join(args.cs2, "game", "csgo", "pak01_dir.vpk")
    dir_data, data_start, entries = vpk_extract.read_tree(vpk)

    def vpk_file(path):
        return vpk_extract.read_file(vpk, entries[path], dir_data, data_start)

    print("reading items_game.txt")
    items_game = to_dict(parse_kv(vpk_file("scripts/items/items_game.txt").decode("utf-8", "replace"))).get("items_game", {})
    loc_en = load_localization(vpk_file("resource/csgo_english.txt"))
    loc = loc_en if args.lang == "english" else load_localization(vpk_file(f"resource/csgo_{args.lang}.txt"))

    schema = Schema(items_game, loc, loc_en)
    catalog = build_catalog(schema, entries.keys())
    catalog["language"] = args.lang

    os.makedirs(args.out, exist_ok=True)
    text = json.dumps(catalog, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    blob = text.encode("utf-8")
    with open(os.path.join(args.out, "catalog.json"), "wb") as f:
        f.write(blob)
    sha = hashlib.sha256(blob).hexdigest()

    counts = {}
    for v in catalog["variants"]:
        counts[v["category"]] = counts.get(v["category"], 0) + 1
    print(f"catalog.json: {len(catalog['definitions'])} definitions, {len(catalog['variants'])} variants {counts}, {len(blob) // 1024} KiB")

    if not args.no_icons:
        icons = build_icons(catalog, args.icons)
        icons["catalogSha256"] = sha
        with open(os.path.join(args.out, "icons.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(icons, f, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        covered = sum(1 for v in catalog["variants"] if v["kind"] == "paint" and f"{v['defIndex']}:{v['kitId']}" in icons["paint"])
        kitc = sum(1 for v in catalog["variants"] if v["kind"] != "paint" and f"{v['kind']}:{v['kitId']}" in icons["kits"])
        print(f"icons.json: {len(icons['images'])} images; paint variants with icons {covered}, kit variants with icons {kitc}")


if __name__ == "__main__":
    main()

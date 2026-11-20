r"""Skin editor in the browser: paint on a weapon's texture layout or right on its 3D model, then build
the skin (the same files as make_skin_mod.py --mode uv makes).

    python tools\skin_editor.py
    python tools\skin_editor.py --cs2 "<CS2 folder>" --data release\cs2_gc --port 13002 --no-browser

Opens http://127.0.0.1:13002/ (a local server, Ctrl+C stops it). The game files are only read. "Build the
skin" writes into <data>\mods\skins like make_skin_mod.py and keeps the painted picture in
<data>\mods\skins\sources\<kit>.png, so a skin made here can be opened and painted on again. The 3D model
is the weapon's position map (the 3D point of every texel of the paint layout) turned into a mesh.
"""
import argparse
import base64
import http.server
import io
import itertools
import json
import os
import re
import sys
import threading
import types
import urllib.parse
import webbrowser

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_skin_mod as msm  # noqa: E402
import make_sticker_mod as mst  # noqa: E402
import source2 as s2  # noqa: E402
from gen_invedit_catalog import load_localization  # noqa: E402
from make_music_mod import cs2_running  # noqa: E402

REPO = os.path.dirname(HERE)
CS2_FOLDER = "Counter-Strike Global Offensive"


def find_cs2():
    """The CS2 folder from Steam's library list, like the launcher finds it."""
    roots = []
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            roots.append(winreg.QueryValueEx(key, "SteamPath")[0])
    except OSError:
        pass
    roots.append(r"C:\Program Files (x86)\Steam")
    for steam in roots:
        vdf = os.path.join(steam, "steamapps", "libraryfolders.vdf")
        libraries = [steam]
        if os.path.exists(vdf):
            text = open(vdf, encoding="utf-8", errors="replace").read()
            libraries += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', text)]
        for lib in libraries:
            path = os.path.join(lib, "steamapps", "common", CS2_FOLDER)
            if os.path.exists(os.path.join(path, "game", "csgo", "pak01_dir.vpk")):
                return path
    return None


# --------------------------------------------------------------------- geometry

def geometry(weapon):
    """The weapon as the page draws it: a position per texel of the paint layout, turned so that x runs
    along the weapon the way the game's icon shows it, y is up and z faces the viewer. Never mirrored:
    whatever the page shows is what the game shows."""
    pos = weapon.image("g_tPosition", 0)
    icon = weapon.base_icon()
    frame = msm.Frame(pos, icon.image(0) if icon else None, weapon.knife)
    xyz = pos[..., :3].astype(np.float64)
    inside = frame.inside
    pts = xyz[inside]
    lo, hi = np.percentile(pts, 0.5, axis=0), np.percentile(pts, 99.5, axis=0)
    center = (lo + hi) / 2
    axes = np.eye(3)
    x = axes[frame.length_axis] * (-1 if frame.flip_u else 1)
    y = axes[frame.height_axis] * (-1 if frame.flip_v else 1)
    rot = np.stack([x, y, np.cross(x, y)])
    scale = 2.0 / max(hi[frame.length_axis] - lo[frame.length_axis], 1e-9)  # the length runs -1..1
    view = (xyz - center) @ rot.T * scale
    # texels the layout only pads (copies of a neighbour, no surface of their own) have no area
    du, dv = np.gradient(view, axis=1), np.gradient(view, axis=0)
    area = np.linalg.norm(np.cross(du, dv), axis=-1)
    step = np.linalg.norm(np.diff(view, axis=1), axis=-1)[inside[:, 1:] & inside[:, :-1]]
    typical = float(np.median(step[step > 0])) if (step > 0).any() else 1e-3
    surface = inside & (area > 0.05 * typical * typical)
    view = np.where(inside[..., None], view, np.nan).astype("<f4")
    n = view.shape[0]
    shade, score = shading(weapon, frame, xyz, rot)
    header = json.dumps({"size": n, "typical": typical, "knife": weapon.knife, "surface": float(surface.mean()),
                         "normals": score >= NORMALS_MIN_SCORE, "normalScore": score}).encode()
    header += b" " * (-len(header) % 4)
    return (b"CSGE" + len(header).to_bytes(4, "little") + header + view.tobytes()
            + surface.astype(np.uint8).tobytes() + shade.tobytes())


# every way to reorder and flip three axes (the normal map of a few knives is in other axes)
SIGNED_AXES = [np.array([[s[i] if j == p[i] else 0 for j in range(3)] for i in range(3)], float)
               for p in itertools.permutations(range(3)) for s in itertools.product((1, -1), repeat=3)]
NORMALS_MIN_SCORE = 0.85


def shading(weapon, frame, xyz, rot):
    """RGBA per texel for the page's game-like light: rgb = the baked normal (the composite input
    g_tSurface holds the high-detail model's normals in model space) turned like the positions,
    a = baked ambient occlusion (g_tAmbientOcclusion, green). The normal map's axes are found by
    comparing it with the position map's own normals; when nothing fits (some knives with moving parts)
    rgb stays flat and the page uses the mesh's normals. Returns (array, how well the normals fit)."""
    n = xyz.shape[0]
    ao = weapon.image("g_tAmbientOcclusion", n)[..., 1]
    surf = weapon.image("g_tSurface", n)[..., :3].astype(np.float64) / 255 * 2 - 1
    cross = np.cross(np.gradient(xyz, axis=1), np.gradient(xyz, axis=0))
    length = np.linalg.norm(cross, axis=-1)
    idx = np.flatnonzero((frame.valid & (length > 0)).ravel())[::7]
    mesh_n = cross.reshape(-1, 3)[idx] / length.ravel()[idx, None]  # points inwards on a layout that isn't mirrored
    sample = surf.reshape(-1, 3)[idx]
    sample /= np.maximum(np.linalg.norm(sample, axis=-1, keepdims=True), 1e-9)
    # how well each choice lines up (mirrored parts of the layout flip the mesh normal: compare without sign),
    # then the sign that points outwards on most of the surface
    fits = []
    for m in SIGNED_AXES:
        dots = (sample @ m.T * mesh_n).sum(-1) if len(idx) else np.zeros(1)
        fits.append(float(np.abs(dots).mean()) if dots.mean() < 0 else 0.0)
    best = int(np.argmax(fits))
    rgb = np.full((n, n, 3), 128, np.uint8)
    if fits[best] >= NORMALS_MIN_SCORE:
        normal = surf @ SIGNED_AXES[best].T @ rot.T
        normal /= np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-9)
        rgb = np.clip(np.rint((normal * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8)
    return np.dstack([rgb, ao.astype(np.uint8)]), round(fits[best], 3)


def png_bytes(rgba):
    out = io.BytesIO()
    Image.fromarray(rgba).save(out, "PNG", compress_level=3)
    return out.getvalue()


# ----------------------------------------------------------------------- editor

class Editor:
    def __init__(self, game, data, mod_name):
        self.game, self.data, self.mod_name = game, data, mod_name
        self.weapons = game.weapons()
        self.lock = threading.Lock()
        self.cache = {}
        self.loc = {}

    def cached(self, key, make):
        with self.lock:
            if key not in self.cache:
                self.cache[key] = make()
            return self.cache[key]

    def localization(self, lang):
        if lang not in self.loc:
            path = f"resource/csgo_{lang}.txt"
            if path not in self.game.entries:
                path = "resource/csgo_english.txt"
            raw = msm.read_file(self.game.vpk, self.game.entries[path], self.game.dir_data, self.game.data_start)
            self.loc[lang] = load_localization(raw)
        return self.loc[lang]

    def weapon(self, short):
        if short not in self.weapons:
            raise KeyError(f"no paintable weapon '{short}'")
        return self.weapons[short]

    def mod(self):
        return msm.Mod(self.data, "skins")

    def info(self, lang):
        loc = self.localization(lang)
        weapons = []
        for short, w in self.weapons.items():
            token = str(self.game.resolved(w.item, "item_name") or "").lstrip("#").lower()
            weapons.append({"short": short, "title": loc.get(token) or short, "knife": w.knife, "gear": w.gear,
                            "defIndex": w.def_index})
        weapons.sort(key=lambda w: (w["gear"], w["knife"], w["title"].lower()))
        finishes = {k: {"colors": v["colors"], "roughness": v["roughness"]} for k, v in msm.FINISHES.items()}
        # the game's own rarity names for stickers (Base Grade, High Grade...), in the page's language
        rarity_names = {r: loc.get(f"rarity_{r}") or r for r in mst.RARITIES}
        return {"weapons": weapons, "skins": self.skins(), "modDir": self.mod().dir, "finishes": finishes,
                "rarities": msm.RARITIES, "stickerRarities": mst.RARITIES, "rarityNames": rarity_names,
                "stickerFinishes": {name: spec["rarity"] for name, spec in mst.FINISHES.items()},
                "cs2Running": cs2_running()}

    # -- stickers (make_sticker_mod, mods\stickers) --

    def sticker_mod(self):
        return mst.Stickers(self.data, "stickers")

    def stickers(self):
        st = self.sticker_mod()
        return [{"kit": kit, "id": s["id"], "name": s["name"], "description": s.get("description", ""), "rarity": s["rarity"],
                 "finish": s.get("finish", "paper"), "mask": bool(s.get("mask"))}
                for kit, s in sorted(st.stickers.items(), key=lambda kv: kv[1]["id"])]

    def sticker_image(self, kit, which="image"):
        """PNG of a sticker: its picture or mask as they went in (mods\\stickers\\sources), or its built icon."""
        st = self.sticker_mod()
        s = st.stickers[kit]
        if which == "icon":
            path = os.path.join(st.dir, "files", "panorama", "images", "econ", "stickers", "cs2gc", kit + "_png.vtex_c")
            return png_bytes(s2.Texture(open(path, "rb").read()).image(0))
        own = os.path.join(st.dir, "sources", kit + ("_mask" if which == "mask" else "") + ".png")
        for path in (own, s.get(which, "")):
            if path and os.path.isfile(path):
                buf = io.BytesIO()
                Image.open(path).convert("RGBA").save(buf, "PNG")
                return buf.getvalue()
        raise KeyError(f"the {which} of {kit} isn't there any more")

    def build_sticker(self, body):
        name = str(body.get("name", "")).strip()
        if not name:
            raise ValueError("the sticker needs a name")
        rarity = str(body.get("rarity", "rare"))
        if rarity not in mst.RARITIES:
            raise ValueError(f"bad rarity {rarity}")
        kit = str(body.get("kit", "")).strip().lower()
        if not re.fullmatch(r"cs2gc_[a-z0-9_]{1,54}", kit):
            raise ValueError("internal name: cs2gc_ and Latin letters, digits, _")
        count = max(0, min(int(body.get("count", 0)), 50))
        finish = str(body.get("finish", "paper"))
        if finish not in mst.FINISHES:
            raise ValueError(f"no finish {finish}")
        picture = Image.open(io.BytesIO(base64.b64decode(body["image"])))
        picture.load()
        mask = None
        if body.get("mask") and finish in ("holo", "glitter"):
            mask = Image.open(io.BytesIO(base64.b64decode(body["mask"])))
            mask.load()
        log = []
        with self.lock:
            st = self.sticker_mod()
            sources = os.path.join(st.dir, "sources")
            os.makedirs(sources, exist_ok=True)
            upload = os.path.join(sources, "_upload.png")
            picture.convert("RGBA").save(upload)
            mask_upload = os.path.join(sources, "_upload_mask.png")
            if mask is not None:
                mask.convert("RGBA").save(mask_upload)
            used = {int(k) for k in self.game.items_game.get("sticker_kits", {}) if k.isdigit()} | st.other_ids() \
                | {s["id"] for s in st.stickers.values()}
            args = types.SimpleNamespace(name=name, kit_name=kit.removeprefix("cs2gc_"), description=str(body.get("description", "")),
                                         rarity=rarity, template=mst.DEFAULT_TEMPLATE, image=upload, finish=finish,
                                         mask=mask_upload if mask is not None else None)
            kit, sticker_id = mst.make_sticker(self.game, st, args, used, log.append)
            source = os.path.join(sources, kit + ".png")
            os.replace(upload, source)
            st.stickers[kit]["image"] = source
            mask_source = os.path.join(sources, kit + "_mask.png")
            if mask is not None:
                os.replace(mask_upload, mask_source)
                st.stickers[kit]["mask"] = mask_source
            elif os.path.exists(mask_source):
                os.remove(mask_source)
            st.save("Custom stickers", 10)
            running = cs2_running()
            added = 0
            if count and not running:
                inventory = os.path.join(self.data, "inventory.txt")
                for _ in range(count):
                    mst.add_inventory_item(inventory, mst.STICKER_DEF, 4, self.game.rarity_value(rarity), [(113, sticker_id)],
                                           ".before_sticker_mod")
                added = count
                log.append(f"added {count} of it to the inventory")
        return {"kit": kit, "id": sticker_id, "added": added, "running": bool(count and running), "log": log,
                "stickers": self.stickers()}

    def remove_sticker(self, kit):
        with self.lock:
            st = self.sticker_mod()
            if kit not in st.stickers:
                raise KeyError(f"no sticker '{kit}'")
            st.remove_files(kit)
            for source in (os.path.join(st.dir, "sources", kit + ".png"), os.path.join(st.dir, "sources", kit + "_mask.png")):
                if os.path.exists(source):
                    os.remove(source)
            del st.stickers[kit]
            st.save("Custom stickers", 10)
        return {"stickers": self.stickers()}
    def skins(self):
        mod = self.mod()
        out = []
        for kit, s in sorted(mod.skins.items(), key=lambda kv: kv[1]["id"]):
            finish = s.get("finish", "custom")
            spec = msm.FINISHES[finish]
            out.append({"kit": kit, "id": s["id"], "weapon": s["weapon"].removeprefix("weapon_"), "name": s["name"],
                        "description": s.get("description", ""), "rarity": s["rarity"], "mode": s.get("mode", ""),
                        "durability": s.get("durability", 1.0), "wear": [s["wear_min"], s["wear_max"]],
                        "prefix": s.get("prefix", kit.rsplit("_" + s["weapon"].removeprefix("weapon_"), 1)[0]),
                        "finish": finish, "colors": s.get("colors") or spec["colors"],
                        "roughness": s.get("roughness", spec["roughness"]), "pearlescence": s.get("pearlescence", 0.0),
                        "paintMask": os.path.exists(self.paint_path(mod, kit))})
        return out

    @staticmethod
    def paint_path(mod, kit):
        return os.path.join(mod.dir, "sources", kit + "_paint.png")

    def skin_paint(self, kit):
        """The gunsmith paint mask an editor build kept (white = paint, black = bare metal)."""
        path = self.paint_path(self.mod(), kit)
        if not os.path.exists(path):
            raise KeyError(f"no paint mask for '{kit}'")
        return open(path, "rb").read()

    def skin_texture(self, kit):
        """The picture a skin was made from (editor builds keep it), else its texture decoded."""
        mod = self.mod()
        source = os.path.join(mod.dir, "sources", kit + ".png")
        if os.path.exists(source):
            return open(source, "rb").read()
        s = mod.skins.get(kit)
        if not s:
            raise KeyError(f"no skin '{kit}'")
        tex = s2.Texture(open(os.path.join(mod.files, *(s["texture"] + "_c").split("/")), "rb").read())
        raw, w, h = tex.mip(0)
        rgba = s2.decode_dxt(raw, w, h, tex.format == s2.FMT_DXT5)
        rgba[..., 3] = 255
        return png_bytes(rgba)

    def build(self, body):
        weapon = self.weapon(body["weapon"])
        name = (body.get("name") or "").strip() or "skin"
        prefix = body.get("prefix") or "cs2gc_" + msm.slug(name)
        if not re.fullmatch(r"[a-z0-9_]{1,60}", prefix):
            raise ValueError("bad kit name")
        rarity = body.get("rarity", "ancient")
        if rarity not in msm.RARITIES:
            raise ValueError("bad rarity")
        durability = min(1.0, max(0.0, float(body.get("durability", 1.0))))
        wear = [min(1.0, max(0.0, float(v))) for v in body.get("wearRange", [0.0, 1.0])]
        picture = np.asarray(Image.open(io.BytesIO(base64.b64decode(body["png"]))).convert("RGBA"))
        size = int(body.get("size", 2048))
        if size not in (512, 1024, 2048, 4096):
            raise ValueError("bad size")
        finish = body.get("finish", "custom")
        if finish not in msm.FINISHES:
            raise ValueError("bad finish")
        colors = None
        if msm.FINISHES[finish]["colors"] and body.get("colors"):
            colors = [str(c).lstrip("#").lower() for c in body["colors"]]
            if len(colors) != 4 or not all(re.fullmatch(r"[0-9a-f]{6}", c) for c in colors):
                raise ValueError("bad colors")
        roughness = min(1.0, max(0.0, float(body["roughness"]))) if body.get("roughness") is not None else None
        pearl = min(6.0, max(-6.0, float(body["pearlescence"]))) if body.get("pearlescence") is not None else None
        paint = None
        if finish == "gunsmith" and body.get("paint"):
            paint_img = Image.open(io.BytesIO(base64.b64decode(body["paint"]))).convert("L")
            paint = np.asarray(paint_img, np.float32) / 255

        log = []
        with self.lock:
            mod = self.mod()
            kit = f"{prefix}_{weapon.short}"
            sources = os.path.join(mod.dir, "sources")
            os.makedirs(sources, exist_ok=True)
            source = os.path.join(sources, kit + ".png")
            Image.fromarray(picture).save(source)
            if paint is not None:
                paint_img.save(self.paint_path(mod, kit))
            elif os.path.exists(self.paint_path(mod, kit)):
                os.remove(self.paint_path(mod, kit))
            args = types.SimpleNamespace(kit=prefix, size=size, mode="uv", durability=durability, name=name,
                                         description=body.get("description", ""), rarity=rarity,
                                         wear_range=tuple(wear), id=None, image=source, finish=finish,
                                         colors=colors, roughness=roughness, pearlescence=pearl, paint_mask=paint)
            used = self.game.used_paint_ids() | msm.other_mod_ids(self.data, mod.dir)
            used |= {s["id"] for k, s in mod.skins.items() if k != kit}
            kit, paint_id = msm.make_skin(self.game, mod, weapon, picture, args, used, log.append)
            mod.skins[kit].update(prefix=prefix, editor=True)
            msm.update_gear(self.game, mod)
            mod.save(self.mod_name, 10)
            log.append(f"wrote {mod.dir}")
            if body.get("addItem"):
                if cs2_running():
                    log.append("the game is running: inventory.txt not touched (close the game and build again)")
                else:
                    item_rarity = self.game.rarity_value(self.game.resolved(weapon.item, "item_rarity") or "common")
                    rarity_value = 6 if weapon.knife else msm.painted_rarity(item_rarity, self.game.rarity_value(rarity))
                    msm.add_item(os.path.join(self.data, "inventory.txt"), weapon, paint_id, rarity_value,
                                 float(body.get("wear", 0.01)), int(body.get("seed", 0)), log.append)
        return {"kit": kit, "id": paint_id, "log": log, "skins": self.skins()}

    def case(self, body):
        """A case of all the mod's skins (make_case_mod), with `count` of it put into the inventory."""
        import make_case_mod as mcm
        title = str(body.get("name", "")).strip()
        if not title:
            raise ValueError("the case needs a name")
        count = max(0, min(int(body.get("count", 0)), 50))
        log = []
        with self.lock:
            cases = mcm.Cases(self.data, "cases")
            args = types.SimpleNamespace(name=title, case_name=None, description=str(body.get("description", "")),
                                         skins=[], skin_mod="skins", like="crate_community_33", icon=None, data=self.data)
            name, def_index = mcm.make_case(self.game, cases, args, log.append)
            cases.save(self.game, "Custom cases", 20, log.append)
            skins = cases.skins_of(cases.cases[name])
            knives = sum(1 for _, skin in skins if mcm.is_knife(skin["weapon"]))
            running = cs2_running()
            added = 0
            if count and not running:
                inventory = os.path.join(self.data, "inventory.txt")
                for _ in range(count):
                    mcm.add_inventory_item(inventory, def_index, 4, 1, [], ".before_case_mod")
                added = count
        return {"case": name, "defIndex": def_index, "skins": len(skins) - knives, "knives": knives, "added": added,
                "running": bool(count and running), "log": log}

    def remove(self, kit):
        with self.lock:
            mod = self.mod()
            if kit not in mod.skins:
                raise KeyError(f"no skin '{kit}'")
            mod.remove_files(kit)
            for source in (os.path.join(mod.dir, "sources", kit + ".png"), self.paint_path(mod, kit)):
                if os.path.exists(source):
                    os.remove(source)
            del mod.skins[kit]
            msm.update_gear(self.game, mod)
            mod.save(self.mod_name, 10)
        return {"skins": self.skins()}


def serve(editor, port, open_browser):
    page_path = os.path.join(HERE, "skin_editor.html")
    text_path = os.path.join(HERE, "skin_editor_text.js")

    class Handler(http.server.BaseHTTPRequestHandler):
        def send(self, code, body, content_type):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def send_json(self, obj, code=200):
            self.send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

        def fail(self, e):
            print(f"error: {e}")
            self.send_json({"error": str(e)}, 400)

        def do_GET(self):
            url = urllib.parse.urlparse(self.path)
            query = urllib.parse.parse_qs(url.query)
            parts = [urllib.parse.unquote(p) for p in url.path.strip("/").split("/")]
            try:
                if url.path == "/":
                    self.send(200, open(page_path, "rb").read(), "text/html; charset=utf-8")
                elif url.path == "/text.js":
                    self.send(200, open(text_path, "rb").read(), "text/javascript; charset=utf-8")
                elif url.path == "/api/info":
                    self.send_json(editor.info(query.get("lang", ["english"])[0]))
                elif parts[:2] == ["api", "weapon"] and len(parts) == 4:
                    weapon = editor.weapon(parts[2])
                    if parts[3] == "geometry":
                        blob = editor.cached(("geo", weapon.short), lambda: geometry(weapon))
                        self.send(200, blob, "application/octet-stream")
                    elif parts[3] == "color":
                        size = int(query.get("size", ["2048"])[0])
                        if size not in (512, 1024, 2048, 4096):
                            raise ValueError("bad size")
                        blob = editor.cached(("color", weapon.short, size),
                                             lambda: png_bytes(weapon.image("g_tColor", size)[..., :3].copy()))
                        self.send(200, blob, "image/png")
                    else:
                        self.send(404, b"not found", "text/plain")
                elif parts[:2] == ["api", "skin"] and len(parts) == 4 and parts[3] == "texture":
                    self.send(200, editor.skin_texture(parts[2]), "image/png")
                elif parts[:2] == ["api", "skin"] and len(parts) == 4 and parts[3] == "paint":
                    self.send(200, editor.skin_paint(parts[2]), "image/png")
                elif url.path == "/api/stickers":
                    self.send_json({"stickers": editor.stickers()})
                elif parts[:2] == ["api", "sticker"] and len(parts) == 4 and parts[3] in ("image", "mask", "icon"):
                    self.send(200, editor.sticker_image(parts[2], parts[3]), "image/png")
                elif parts[:2] == ["api", "preview"] and len(parts) == 3:
                    path = os.path.join(editor.mod().dir, "previews", os.path.basename(parts[2]) + ".png")
                    self.send(200, open(path, "rb").read(), "image/png")
                else:
                    self.send(404, b"not found", "text/plain")
            except (KeyError, ValueError, OSError) as e:
                self.fail(e)

        def do_POST(self):
            # only this page may build or delete: another site open in the browser can post here too
            origin = self.headers.get("Origin")
            if origin and urllib.parse.urlparse(origin).hostname not in ("127.0.0.1", "localhost"):
                self.send(403, b"forbidden", "text/plain")
                return
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
                if self.path == "/api/build":
                    result = editor.build(body)
                    print("\n".join(result["log"]))
                    self.send_json(result)
                elif self.path == "/api/remove":
                    self.send_json(editor.remove(body["kit"]))
                elif self.path == "/api/sticker":
                    result = editor.build_sticker(body)
                    print("\n".join(result["log"]))
                    self.send_json(result)
                elif self.path == "/api/sticker/remove":
                    self.send_json(editor.remove_sticker(body["kit"]))
                elif self.path == "/api/case":
                    result = editor.case(body)
                    print("\n".join(result["log"]))
                    self.send_json(result)
                else:
                    self.send(404, b"not found", "text/plain")
            except (KeyError, ValueError, OSError, SystemExit) as e:
                self.fail(e)

        def log_message(self, *a):
            pass

    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        print(f"port {port} is taken (another skin_editor.py still running? close it: it serves its own old code)")
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"skin editor: {url} (Ctrl+C to stop)")
    if open_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cs2", help="CS2 folder (found through Steam when not given)")
    ap.add_argument("--data", default=os.path.join(REPO, "release", "cs2_gc"), help="cs2_gc data folder")
    ap.add_argument("--mod-name", default="Custom skins", help="mod name in mod.txt (for a new mod)")
    ap.add_argument("--port", type=int, default=13002)
    ap.add_argument("--no-browser", action="store_true", help="don't open the page")
    args = ap.parse_args()
    cs2 = args.cs2 or find_cs2()
    if not cs2:
        sys.exit("CS2 not found through Steam: give --cs2 <CS2 folder>")
    if not os.path.isdir(args.data):
        sys.exit(f"no {args.data}: give --data <cs2_gc data folder> (release\\cs2_gc)")
    print(f"CS2: {cs2}\ndata: {os.path.abspath(args.data)}")
    serve(Editor(msm.Game(cs2), os.path.abspath(args.data), args.mod_name), args.port, not args.no_browser)


if __name__ == "__main__":
    main()

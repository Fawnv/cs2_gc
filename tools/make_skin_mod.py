"""Make a weapon skin out of any picture (png, jpg, webp...), as a CSForge mod.

    python tools\\make_skin_mod.py --cs2 "<CS2 folder>" --data release\\cs2_gc --image pic.png --weapon ak47 --name "My AK"
    python tools\\make_skin_mod.py --cs2 "..." --data release\\cs2_gc --image pic.png --weapon ak47 --weapon awp --add-item
    python tools\\make_skin_mod.py --cs2 "..." --template ak47 ak47_uv.png     (the weapon's texture layout to paint on)
    python tools\\make_skin_mod.py --cs2 "..." --data release\\cs2_gc --image ak47_uv.png --mode uv --weapon ak47
    python tools\\make_skin_mod.py --cs2 "..." --data release\\cs2_gc --list | --remove <kit>

Reads the game files only and writes into <data>\\mods\\<mod> (default "skins"). Each weapon gets its own
paint kit (the picture lands differently on every model):
  files\\items\\assets\\paintkits\\cs2gc\\<kit>_png_<hash>.vtex_c    the paint texture (DXT1/DXT5)
  files\\weapons\\paints\\cs2gc\\<kit>.vcompmat_c                    Valve's "Custom Paint Job" workshop example
                                                                  with the texture path swapped
  files\\panorama\\images\\econ\\default_generated\\<weapon>_<kit>_{light,medium,heavy}_png.vtex_c   inventory icons
  items_game.txt, localization.txt, mod.txt, skins.json, previews\\<kit>.png

--finish custom (default) is paint, gunsmith is paint over metal (--paint-mask: white = paint, black = bare
metal), patina makes the texture the color of the metal. --colors, --roughness and --pearlescence change the
values of Valve's workshop example the material is made from.

Grenades and the C4 (--weapon hegrenade, flashbang, smokegrenade, molotov, incgrenade, decoy, c4): the game
draws no paint kit on them, so their skin replaces the grenade's own color texture (a copy of its material
pointing at the painted texture). All grenades of that kind look like that, the newest skin of each is the
one in use. Their maps for icons are baked from the model (mesh_inputs.py). --add-item equips such an item
right away (the game's loadout screen has no place for grenades).

--mode side (default) projects the picture onto both sides of the weapon, the way a wrap would look,
using the weapon's position map (the 3D point of every texel); the rest of the weapon keeps its own
color. --mode uv takes the picture as the texture itself: paint it over the --template image.
Icons are drawn from the same position map, so they show the finished skin from the side.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesh_inputs as mi  # noqa: E402
import source2 as s2  # noqa: E402
from gen_invedit_catalog import parse_kv, painted_rarity, to_dict  # noqa: E402
from make_music_mod import add_inventory_item, cs2_running, kv_string  # noqa: E402
from vpk_extract import read_file, read_tree  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def default_data():
    """The cs2_gc data folder when --data isn't given: release\\cs2_gc of a build, or before one
    (a fresh clone of the public repo, no release folder yet) _data next to the tools."""
    built = os.path.join(REPO, "release", "cs2_gc")
    return built if os.path.isdir(built) else os.path.join(REPO, "_data")


def make_data(path):
    """--data as an absolute path: the default when it isn't given, made if it's the default and
    nothing is there yet (the tools then write their mods into a new folder)."""
    path = os.path.abspath(path or default_data())
    os.makedirs(path, exist_ok=True)
    return path


# finishes that take a texture painted over the weapon's layout: Valve's workshop example of each, the
# paint kit style in items_game and what the example sets. The four colors of gunsmith and patina are
# the metal under the paint: base metal, patina tint, patina wear, grime.
FINISHES = {
    "custom": {"style": 7, "example": "cu_paintkit_example", "texture": "example_custom_paint_tga_1ad43866",
               "source": "example_custom_paint", "colors": None, "roughness": 0.6},
    "gunsmith": {"style": 9, "example": "gs_paintkit_example", "texture": "example_gunsmith_tga_4642a77e",
                 "source": "example_gunsmith", "colors": ["c0afa5", "fff6f2", "ededd2", "c6b28b"], "roughness": 0.4},
    "patina": {"style": 8, "example": "aq_paintkit_example", "texture": "example_patina_tga_ff58dfff",
               "source": "example_patina", "colors": ["ffffff", "ffffff", "bac1c8", "3d3d3d"], "roughness": 0.4},
}
WORKSHOP = "items/assets/paintkits/workshop/"
FIRST_ID = 20100
ICON_W, ICON_H = 512, 384
ICON_GENERATED = "panorama/images/econ/default_generated/{weapon}_{kit}_{wear}_png.vtex_c"
ICON_BASE = "panorama/images/econ/weapons/base_weapons/{weapon}_png.vtex_c"
RARITIES = ["common", "uncommon", "rare", "mythical", "legendary", "ancient"]
ATTR_PAINT, ATTR_SEED, ATTR_WEAR = 6, 7, 8
GEAR_MIN_IOU = 0.3  # below this a grenade keeps the game's own icon


# ------------------------------------------------------------------------- game

class Game:
    def __init__(self, cs2):
        self.vpk = os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")
        if not os.path.exists(self.vpk):
            sys.exit(f"no {self.vpk}")
        self.dir_data, self.data_start, self.entries = read_tree(self.vpk)
        self._items_game = None

    def read(self, path):
        if not path.endswith("_c"):
            path += "_c"
        if path not in self.entries:
            sys.exit(f"{path} isn't in the game files")
        return read_file(self.vpk, self.entries[path], self.dir_data, self.data_start)

    def texture(self, path):
        return s2.Texture(self.read(path))

    @property
    def items_game(self):
        if self._items_game is None:
            raw = read_file(self.vpk, self.entries["scripts/items/items_game.txt"], self.dir_data, self.data_start)
            self._items_game = to_dict(parse_kv(raw.decode("utf-8", "replace")))["items_game"]
        return self._items_game

    def resolved(self, item, key):
        if key in item:
            return item[key]
        for name in str(item.get("prefab", "")).split():
            value = self.resolved(self.items_game.get("prefabs", {}).get(name, {}), key)
            if value is not None:
                return value
        return None

    def weapons(self):
        """{short name: Weapon} for every weapon and knife that has composite inputs (can wear a paint kit),
        plus the grenades and the C4, whose inputs are baked from their models (MeshWeapon)."""
        found = {}
        for def_index, item in self.items_game.get("items", {}).items():
            name = item.get("name", "")
            model = self.resolved(item, "model_player")
            if not name.startswith("weapon_") or not model or not def_index.isdigit():
                continue
            short = name[len("weapon_"):]
            folder = model.rsplit("/", 1)[0] + "/materials/composite_inputs/"
            inputs = [p for p in self.entries if p.startswith(folder) and p.endswith("_composite_inputs.vmat_c")]
            if inputs:
                found[short] = Weapon(self, int(def_index), name, inputs[0], item)
                continue
            slot = str(self.resolved(item, "flexible_loadout_slot") or "")
            if slot in mi.SLOTS and model + "_c" in self.entries:
                found[short] = MeshWeapon(self, int(def_index), name, model, item, mi.SLOTS[slot])
        return found

    def rarity_value(self, name):
        entry = self.items_game.get("rarities", {}).get(name, {})
        return int(entry.get("value", 1)) if isinstance(entry, dict) else 1

    def used_paint_ids(self):
        return {int(k) for k in self.items_game.get("paint_kits", {}) if k.isdigit()}


class Weapon:
    gear = False  # grenades and the C4 (MeshWeapon)

    def __init__(self, game, def_index, name, inputs, item):
        self.game, self.def_index, self.name, self.inputs_path, self.item = game, def_index, name, inputs, item
        self.short = name[len("weapon_"):]
        self.knife = name.startswith("weapon_knife") or name == "weapon_bayonet"
        self._inputs = None

    @property
    def inputs(self):
        if self._inputs is None:
            kv = s2.KV3(s2.Resource.parse(self.game.read(self.inputs_path)).block("DATA")).decode()
            self._inputs = {t["m_name"]: t["m_pValue"] for t in kv.get("m_textureParams", [])}
        return self._inputs

    def image(self, param, size):
        """A composite input as a size x size array (RGBA, or float RGBA for the position map).
        size 0 = the texture's own size."""
        return self.texture_image(self.inputs[param], size)

    def texture_image(self, path, size):
        tex = self.game.texture(path)
        size = size or tex.width
        level = 0
        while level + 1 < tex.mips and (tex.width >> (level + 1)) >= size:
            level += 1
        data, w, h = tex.mip(level)
        if tex.format == s2.FMT_RGBA16F:
            arr = np.frombuffer(data, np.float16).reshape(h, w, 4).astype(np.float32)
            if (w, h) != (size, size):
                arr = np.stack([np.asarray(Image.fromarray(arr[..., c]).resize((size, size), Image.BILINEAR))
                                for c in range(4)], -1)
            return arr
        if tex.format in s2.PILLOW_BCN:
            n = s2.PILLOW_BCN[tex.format]
            mode = {4: "L", 5: "RGB"}.get(n, "RGBA")  # what Pillow's BCn decoder makes of each
            img = Image.frombytes(mode, (w, h), data, "bcn", n).convert("RGBA")
        else:
            img = Image.fromarray(tex.image(level)).convert("RGBA")
        if img.size != (size, size):
            img = img.resize((size, size), Image.LANCZOS)
        return np.asarray(img)

    def base_icon(self):
        path = ICON_BASE.format(weapon=self.name)
        if path not in self.game.entries:
            return None
        return self.game.texture(path)


class MeshWeapon(Weapon):
    """A grenade or the C4: the game has no composite inputs for them, so the maps the editor and the icons
    need are baked from the model (mesh_inputs), and a skin replaces the weapon's own color texture."""
    gear = True

    def __init__(self, game, def_index, name, model, item, slot):
        super().__init__(game, def_index, name, None, item)
        self.mesh = mi.MeshInputs(game, model)
        self.inputs_path = None
        self.slot = slot  # loadout position: grenades aren't in the game's loadout screen, items get equipped here
        users = self.game.resolved(item, "used_by_classes") or {}
        self.classes = sorted(mi.CLASSES[c] for c in users if c in mi.CLASSES)

    @property
    def inputs(self):
        return self.mesh.paths

    def image(self, param, size):
        baked = self.mesh.baked().get(param)
        if baked is None:  # color, roughness / metalness: the weapon's own textures
            return super().image(param, size)
        size = size or baked.shape[0]
        if baked.shape[0] == size:
            return baked
        if baked.dtype == np.float32:
            return np.stack([np.asarray(Image.fromarray(baked[..., c]).resize((size, size), Image.NEAREST))
                             for c in range(4)], -1)
        return np.asarray(Image.fromarray(baked).resize((size, size), Image.LANCZOS))

    def skin_files(self, kit, rgb, paint, finish, size):
        """The game draws no paint kit on a grenade: the skin replaces the color texture of the weapon's
        material (and of its team variants, which keep their own colors where nothing is painted).
        Returns ({texture path_c: blob}, {material: texture path})."""
        main = self.texture_image(self.mesh.paths["g_tColor"], size)
        untouched = (rgb == main[..., :3]).all(-1, keepdims=True)
        files, replace = {}, {}
        for material in self.mesh.materials:
            color = self.mesh.textures(material)["g_tColor"]
            own = self.texture_image(color, size)
            out = np.where(untouched, own[..., :3], rgb).astype(np.float32)
            if finish == "gunsmith" and paint is not None:  # bare metal: the weapon's own texture
                out = out * paint[..., None] + own[..., :3] * (1 - paint[..., None])
            rgba = np.dstack([np.clip(np.rint(out), 0, 255).astype(np.uint8), own[..., 3]])
            name = material.rsplit("/", 1)[1].rsplit(".", 1)[0]
            path = f"{self.mesh.folder}/materials/cs2gc/{kit}_{name}.vtex"
            red2 = s2.Resource.parse(self.game.read(color + "_c")).block("RED2")
            files[path + "_c"] = mi.encode_color(rgba, red2)
            replace[material] = path
        return files, replace


# ------------------------------------------------------------------------ frame

class Frame:
    """Where every texel sits on the weapon: u along the length (0 = muzzle / tip side as the game's icon
    shows it), v top to bottom, d the side (positive = the side the icon shows)."""

    def __init__(self, pos, base_icon_rgba, knife=False):
        self.knife = knife
        xyz = pos[..., :3]
        finite = np.isfinite(xyz).all(-1) & (np.abs(xyz) < 1e4).all(-1)
        xyz = np.where(finite[..., None], xyz, 0)
        # the empty parts of the layout are filled with one value (0,0,0 or 1,1,1 depending on the model)
        flat = xyz.reshape(-1, 3)
        values, counts = np.unique(flat[::7], axis=0, return_counts=True)
        for value in values[counts > len(flat[::7]) * 0.008]:  # real surface values stay under 0.2%
            finite &= ~(xyz == value).all(-1)
        pts = xyz[finite]
        lo = np.percentile(pts, 0.5, axis=0)
        hi = np.percentile(pts, 99.5, axis=0)
        ext = hi - lo
        margin = ext * 0.02
        inside = finite & ((xyz >= lo - margin) & (xyz <= hi + margin)).all(-1)
        # texels next to a seam of the layout jump to another part of the model: no good for drawing
        step_x = np.linalg.norm(np.diff(xyz, axis=1, append=xyz[:, -1:]), axis=-1)
        step_y = np.linalg.norm(np.diff(xyz, axis=0, append=xyz[-1:]), axis=-1)
        steps = np.concatenate([step_x[inside], step_y[inside]])
        typical = np.median(steps[steps > 0]) if (steps > 0).any() else 0.0  # some maps are upscaled: zero steps
        jump = np.maximum(step_x, step_y)
        jump = np.maximum(jump, np.roll(jump, 1, 1))
        jump = np.maximum(jump, np.roll(jump, 1, 0))
        self.inside = inside
        self.valid = inside & (jump < typical * 6 + 1e-9)
        order = np.argsort(-ext)  # length, height, depth
        self.length_axis, self.height_axis, self.depth_axis = order
        norm = (xyz - lo) / np.maximum(ext, 1e-9)
        self.u0 = norm[..., self.length_axis]
        self.v0 = 1 - norm[..., self.height_axis]
        depth = norm[..., self.depth_axis]
        self.d0 = depth - np.median(depth[self.valid])  # the middle plane; side parts don't shift it
        self.aspect = ext[self.length_axis] / max(ext[self.height_axis], 1e-9)
        self.flip_u = self.flip_v = False
        self.side = 1.0
        self.normals = _normals(xyz)
        if base_icon_rgba is not None:
            self._calibrate(base_icon_rgba)

    @property
    def u(self):
        return 1 - self.u0 if self.flip_u else self.u0

    @property
    def v(self):
        return 1 - self.v0 if self.flip_v else self.v0

    @property
    def d(self):
        return self.d0 * self.side

    def at(self, size):
        """(u, v, d) resampled to size x size, nearest texel (no blending across seams)."""
        n = self.u0.shape[0]
        idx = (np.arange(size) * n // size).astype(int)
        pick = np.ix_(idx, idx)
        return self.u[pick], self.v[pick], self.d[pick]

    def _calibrate(self, icon):
        """Place the weapon where the game's icon has it (knives along the silhouette's main axis, guns
        level), then pick the flips whose silhouette matches the icon best. Guns keep their top up."""
        alpha = icon[..., 3] > 24
        if alpha.sum() < 50:
            return
        ys, xs = np.nonzero(alpha)
        icon_pts = np.stack([xs, ys], 1).astype(np.float64)
        best = None
        for fu in (False, True):
            for fv in ((False, True) if self.knife else (False,)):
                self.flip_u, self.flip_v = fu, fv
                self.place = _placement(self._model_points(), icon_pts, self.knife)
                mask = self.splat_mask()
                iou = (mask & alpha).sum() / max((mask | alpha).sum(), 1)
                if best is None or iou > best[0]:
                    best = (iou, fu, fv, self.place)
        self.iou, self.flip_u, self.flip_v, self.place = best

    place = None
    iou = 0.0

    def _model_points(self, count=40000):
        """A sample of the drawable texels in drawing units: (u - 0.5, (v - 0.5) / aspect) * length."""
        idx = np.flatnonzero(self.valid.ravel())
        idx = idx[::max(1, len(idx) // count)]
        u, v = self.u.ravel()[idx], self.v.ravel()[idx]
        return np.stack([u - 0.5, (v - 0.5) / self.aspect], 1)

    def to_icon(self, u, v, scale=1):
        """Icon pixel coordinates of (u, v)."""
        if self.place is None:
            self.place = _placement(self._model_points(), None, self.knife)
        (cx, cy), angle, length = self.place
        x = (u - 0.5) * length
        y = (v - 0.5) * length / self.aspect
        c, s = np.cos(angle), np.sin(angle)
        return (cx + x * c - y * s) * scale, (cy + x * s + y * c) * scale

    def splat_mask(self):
        px, py = self.to_icon(self.u[self.valid], self.v[self.valid])
        px = np.clip(px.astype(int), 0, ICON_W - 1)
        py = np.clip(py.astype(int), 0, ICON_H - 1)
        mask = np.zeros((ICON_H, ICON_W), bool)
        mask[py, px] = True
        return mask


def _principal(pts, rotate):
    """Center, angle, length and width of a 2D point cloud along its main axis (or along x)."""
    c = pts.mean(0)
    axis = np.array([1.0, 0.0])
    if rotate:
        vecs = np.linalg.eigh(np.cov((pts - c).T))[1]
        axis = vecs[:, 1] if vecs[0, 1] >= 0 else -vecs[:, 1]
    perp = np.array([-axis[1], axis[0]])
    along, across = (pts - c) @ axis, (pts - c) @ perp
    lo, hi = np.percentile(along, [0.5, 99.5])
    alo, ahi = np.percentile(across, [0.5, 99.5])
    center = c + axis * (lo + hi) / 2 + perp * (alo + ahi) / 2
    return center, float(np.arctan2(axis[1], axis[0])), hi - lo, ahi - alo


def _placement(model, icon_pts, rotate):
    """(center, angle, length): the similarity that lays the drawn weapon (model points in drawing units)
    over the game icon's weapon - main axis on main axis (knives) or level (guns), same length, centered,
    and never past the icon's edges."""
    margin = 6
    m_center, m_angle, m_len, m_wid = _principal(model, rotate)
    if icon_pts is None:
        i_center, i_angle, i_len, i_wid = np.array([ICON_W / 2, ICON_H / 2]), m_angle, ICON_W * 0.84, ICON_H * 0.8
    else:
        i_center, i_angle, i_len, i_wid = _principal(icon_pts, rotate)
    angle = i_angle - m_angle if rotate else 0.0
    scale = i_len / max(m_len, 1e-9)
    if not rotate:  # the game draws guns in perspective, a bit taller than a side view
        scale = min(scale, i_wid * 1.15 / max(m_wid, 1e-9))
    c, s = np.cos(angle), np.sin(angle)
    rot = np.array([[c, -s], [s, c]])
    # shrink until the drawing fits, keeping its center where the icon's weapon is
    proj = (model - m_center) @ rot.T
    lo, hi = proj.min(0), proj.max(0)
    room = np.array([[i_center[0] - margin, ICON_W - margin - i_center[0]],
                     [i_center[1] - margin, ICON_H - margin - i_center[1]]])
    for k in range(2):
        if -lo[k] * scale > room[k, 0]:
            scale = room[k, 0] / max(-lo[k], 1e-9)
        if hi[k] * scale > room[k, 1]:
            scale = room[k, 1] / max(hi[k], 1e-9)
    center = i_center - rot @ (m_center * scale)
    return (float(center[0]), float(center[1])), float(angle), float(max(scale, 16.0))


def _normals(xyz):
    du = np.gradient(xyz, axis=1)
    dv = np.gradient(xyz, axis=0)
    n = np.cross(du, dv)
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-12)


# ---------------------------------------------------------------------- texture

def load_picture(path):
    img = Image.open(path)
    img.load()
    return np.asarray(img.convert("RGBA"))


def sample(picture, u, v):
    """Bilinear sample of an RGBA picture at normalized u, v (arrays); outside -> alpha 0."""
    h, w = picture.shape[:2]
    inside = (u >= 0) & (u <= 1) & (v >= 0) & (v <= 1)
    x = np.clip(u, 0, 1) * (w - 1)
    y = np.clip(v, 0, 1) * (h - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    p = picture.astype(np.float32)
    out = (p[y0, x0] * (1 - fx) * (1 - fy) + p[y0, x1] * fx * (1 - fy)
           + p[y1, x0] * (1 - fx) * fy + p[y1, x1] * fx * fy)
    out[..., 3] *= inside
    return out


def fit_uv(u, v, aspect_box, aspect_pic, fit, zoom, offset):
    """Map box coords (u, v in 0..1 of a box with aspect_box) to picture coords."""
    if fit == "stretch":
        su = sv = 1.0
    else:
        ratio = aspect_pic / aspect_box
        cover = fit == "cover"
        if (ratio > 1) == cover:  # picture relatively wider: match heights
            su, sv = 1 / ratio, 1.0
        else:
            su, sv = 1.0, ratio
    su, sv = su / zoom, sv / zoom
    pu = (u - 0.5) * su + 0.5 - offset[0]
    pv = (v - 0.5) * sv + 0.5 - offset[1]
    return pu, pv


def side_texture(frame, base, picture, args):
    """Project the picture onto both sides of the weapon over its own color."""
    pic_aspect = picture.shape[1] / picture.shape[0]
    u, v, d = frame.at(base.shape[0])
    if args.mirror:
        u = np.where(d < 0, 1 - u, u)  # the far side reads the right way round too
    pu, pv = fit_uv(u, v, frame.aspect, pic_aspect, args.fit, args.zoom, args.offset)
    color = sample(picture, pu, pv)
    return over(color, base)


def over(color, base):
    a = color[..., 3:4] / 255
    rgb = color[..., :3] * a + base[..., :3].astype(np.float32) * (1 - a)
    return np.clip(np.rint(rgb), 0, 255).astype(np.uint8)


def paint_texture(rgb, durability, paint=None):
    """The RGB texture with alpha = paint durability (custom paint and gunsmith wear low alpha first).
    paint (0..1 per pixel, gunsmith): 0 = no paint there, the bare metal of the finish shows."""
    a = np.full(rgb.shape[:2], durability * 255, np.float32)
    if paint is not None:
        a = a * np.clip(paint, 0, 1)
    return np.concatenate([rgb, np.rint(a).astype(np.uint8)[..., None]], -1)


def load_mask(path, size):
    """A grey picture as 0..1, size x size (white = 1)."""
    img = Image.open(path)
    img.load()
    return np.asarray(img.convert("L").resize((size, size), Image.LANCZOS), np.float32) / 255


def encode_paint(rgba, template_red2):
    alpha = rgba[..., 3].min() < 255
    fmt = s2.FMT_DXT5 if alpha else s2.FMT_DXT1
    h = rgba.shape[0]
    chunks = []
    step = max(4, (65536 * 16) // rgba.shape[1] // 4 * 4)
    for y in range(0, h, step):
        chunks.append(s2.encode_dxt(rgba[y:y + step], alpha))
    data = b"".join(chunks)
    fb = s2.fallback_bits(rgba)
    return s2.build_texture(None, fmt, [(data, rgba.shape[1], rgba.shape[0])], 0, fb, template_red2)


# ------------------------------------------------------------------------- icon

def render_icon(frame, tex_rgb, scale=2):
    """Draw the weapon from the side (orthographic) with the paint texture: every texel of the position
    map is a 2x2 dot, the nearest dot wins, shading from the position map's normals."""
    W, H = ICON_W * scale, ICON_H * scale
    n_tex = frame.u0.shape[0]
    if tex_rgb.shape[0] != n_tex:
        tex_rgb = np.asarray(Image.fromarray(tex_rgb).resize((n_tex, n_tex), Image.BOX))
    valid = frame.valid
    u, v, d = frame.u[valid], frame.v[valid], frame.d[valid]
    px, py = frame.to_icon(u, v, scale)
    px = np.clip(px.astype(int), 0, W - 2)
    py = np.clip(py.astype(int), 0, H - 2)
    n = frame.normals[valid]
    facing = np.abs(n[:, frame.depth_axis])
    top = np.clip(n[:, frame.height_axis] * (1 if frame.flip_v else -1), 0, 1)  # lit a little from above
    light = 0.3 + 0.6 * facing + 0.25 * top
    rgb = np.clip(tex_rgb[valid].astype(np.float32) * light[:, None], 0, 255)
    order = np.argsort(d)  # far first, the near side overwrites
    canvas = np.zeros((H, W, 3), np.float32)
    count = np.zeros((H, W), np.int32)
    for dy in (0, 1):
        for dx in (0, 1):
            canvas[py[order] + dy, px[order] + dx] = rgb[order]
            np.add.at(count, (py + dy, px + dx), 1)
    mask = count >= 2
    img = np.concatenate([canvas, mask[..., None] * 255.0], -1).astype(np.uint8)
    # fill pinholes: a pixel without points takes its neighbours' color when most of them are covered
    pil = Image.fromarray(img, "RGBA")
    filled = np.asarray(pil.filter(ImageFilter.MaxFilter(3))).copy()
    a = img[..., 3] > 0
    near = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.BoxBlur(1))) > 150
    out = np.where((~a & near)[..., None], filled, img)
    return np.asarray(Image.fromarray(out, "RGBA").resize((ICON_W, ICON_H), Image.LANCZOS))


def encode_icon(rgba, template_res_blob):
    """An inventory icon (BGRA, one smaller mip) of the picture's own size: a weapon's, a sticker's..."""
    mips = []
    img = Image.fromarray(rgba, "RGBA")
    for level in range(2):
        w, h = rgba.shape[1] >> level, rgba.shape[0] >> level
        m = np.asarray(img.resize((w, h), Image.LANCZOS)) if level else rgba
        mips.append((m[..., [2, 1, 0, 3]].tobytes(), w, h))
    fb = s2.fallback_bits(rgba)
    red2 = s2.Resource.parse(template_res_blob).block("RED2") if template_res_blob else None
    return s2.build_texture(None, s2.FMT_BGRA8888, mips, s2.VTEX_NO_LOD, fb, red2)


# ------------------------------------------------------------------- material

def material_variables(root):
    """{name: Node} of the material's variables (m_strName ...)."""
    found = {}

    def walk(n):
        if n.t == s2.T_OBJECT:
            name = n.get("m_strName")
            if name is not None and name.t == s2.T_STRING:
                found.setdefault(name.v, n)
            for _, _, child in n.v:
                walk(child)
        elif isinstance(n.v, list):
            for child in n.v:
                walk(child)
    walk(root)
    return found


def hex_rgb(text):
    text = text.strip().lstrip("#")
    if not re.fullmatch(r"[0-9a-fA-F]{6}", text):
        sys.exit(f"'{text}' isn't a color like ff8800")
    return [int(text[i:i + 2], 16) for i in (0, 2, 4)]


def build_material(game, texture_path, finish="custom", colors=None, roughness=None, pearlescence=None):
    """Valve's workshop example of the finish with our texture. Colors (four hex strings), roughness and
    pearlescence (-6..6) replace the example's values; the texture always maps 1:1 onto the layout."""
    spec = FINISHES[finish]
    res = s2.Resource.parse(game.read(f"weapons/paints/workshop/{spec['example']}.vcompmat_c"))
    old_vtex = WORKSHOP + spec["texture"] + ".vtex"
    template_source = WORKSHOP + spec["source"]
    source = texture_path.rsplit("_png_", 1)[0]
    mapping = {old_vtex: texture_path, template_source + ".tga": source + ".png", template_source + ".txt": source + ".txt"}
    for fourcc in ("DATA", "RED2"):
        kv = s2.KV3(res.block(fourcc))
        hits = kv.replace_strings(mapping)
        if fourcc == "DATA" and hits < 1:
            sys.exit("the template material changed: no texture path to replace")
        if fourcc == "DATA":
            root = kv.nodes()
            var = material_variables(root)
            changes = {("g_bIgnoreWeaponSizeScale", "m_bValueBoolean"): True}
            if roughness is not None:
                changes[("g_flPaintRoughness", "m_flValueFloatX")] = min(1.0, max(0.0, float(roughness)))
            if pearlescence is not None:
                changes[("g_flPearlescentScale", "m_flValueFloatX")] = min(6.0, max(-6.0, float(pearlescence)))
            for (name, field), value in changes.items():
                node = var.get(name) and var[name].get(field)
                if node is None:
                    sys.exit(f"the template material changed: no {name}")
                node.set(value)
            for i, color in enumerate(colors or []):
                node = var.get(f"g_vColor{i}") and var[f"g_vColor{i}"].get("m_cValueColor4")
                if node is None:
                    sys.exit(f"{finish} has no colors")
                for channel, value in zip(node.v, hex_rgb(color)):
                    channel.set(value)
            kv.encode(root)
        res.set_block(fourcc, kv.build())
    refs = [p for _, p in s2.read_rerl(res.block("RERL"))]
    refs = [texture_path if p == old_vtex else p for p in refs]
    res.set_block("RERL", s2.build_rerl(refs))
    return res.build()


# ------------------------------------------------------------------------- mod

def slug(text):
    s = re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")
    return s[:40] or "skin"


class Mod:
    def __init__(self, data, name):
        self.dir = os.path.join(data, "mods", name)
        self.files = os.path.join(self.dir, "files")
        self.manifest_path = os.path.join(self.dir, "skins.json")
        self.skins = {}
        if os.path.exists(self.manifest_path):
            self.skins = json.load(open(self.manifest_path, encoding="utf-8"))

    def write_file(self, path, blob):
        out = os.path.join(self.files, *path.split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(blob)

    def remove_files(self, kit):
        """Delete a skin's files. A grenade's replaced material stays while another skin of that grenade
        is there (update_gear points it at that one). "shared": files of older builds."""
        entry = self.skins.get(kit, {})
        still = {p for k, s in self.skins.items() if k != kit for p in s.get("shared", [])}
        still |= {m + "_c" for k, s in self.skins.items() if k != kit for m in s.get("replace", {})}
        mine = entry.get("shared", []) + [m + "_c" for m in entry.get("replace", {})]
        for path in entry.get("files", []) + [p for p in mine if p not in still]:
            full = os.path.join(self.files, *path.split("/"))
            if os.path.exists(full):
                os.remove(full)
        preview = os.path.join(self.dir, "previews", kit + ".png")
        if os.path.exists(preview):
            os.remove(preview)

    def save(self, mod_name, priority):
        os.makedirs(self.dir, exist_ok=True)
        with open(self.manifest_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.skins, f, ensure_ascii=False, indent=1)
        kits = sorted(self.skins.items(), key=lambda kv: kv[1]["id"])
        with open(os.path.join(self.dir, "items_game.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("// made by tools\\make_skin_mod.py from skins.json (edits here get overwritten)\n")
            f.write('"items_game"\n{\n\t"paint_kits"\n\t{\n')
            for kit, s in kits:
                f.write(f'\t\t"{s["id"]}"\n\t\t{{\n\t\t\t"name"\t\t"{kit}"\n')
                f.write(f'\t\t\t"description_string"\t\t"#PaintKit_{kit}"\n\t\t\t"description_tag"\t\t"#PaintKit_{kit}_Tag"\n')
                f.write(f'\t\t\t"style"\t\t"{FINISHES[s.get("finish", "custom")]["style"]}"\n')
                f.write(f'\t\t\t"wear_remap_min"\t\t"{s["wear_min"]:.6f}"\n\t\t\t"wear_remap_max"\t\t"{s["wear_max"]:.6f}"\n')
                f.write(f'\t\t\t"composite_material_path"\t\t"{s["material"]}"\n\t\t}}\n')
            f.write('\t}\n\t"paint_kits_rarity"\n\t{\n')
            for kit, s in kits:
                f.write(f'\t\t"{kit}"\t\t"{s["rarity"]}"\n')
            f.write("\t}\n}\n")
        with open(os.path.join(self.dir, "localization.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("// made by tools\\make_skin_mod.py\n\"lang\"\n{\n\t\"Tokens\"\n\t{\n")
            for kit, s in kits:
                f.write(f'\t\t"PaintKit_{kit}_Tag"\t\t"{kv_string(s["name"])}"\n')
                f.write(f'\t\t"PaintKit_{kit}"\t\t"{kv_string(s.get("description", ""))}"\n')
            f.write("\t}\n}\n")
        path = os.path.join(self.dir, "mod.txt")
        if not os.path.exists(path):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("// made by tools\\make_skin_mod.py\n\"mod\"\n{\n")
                f.write(f'\t"name"\t\t"{kv_string(mod_name)}"\n\t"description"\t"skins made from pictures"\n')
                f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{priority}"\n}}\n')


def update_gear(game, mod):
    """Write the replaced material of every grenade / C4 that has skins, pointing at its active skin's
    texture (the newest one when none is marked). Call before mod.save()."""
    kits = {}
    for kit, s in mod.skins.items():
        if s.get("replace"):
            kits.setdefault(s["weapon"], []).append(kit)
    for weapon, names in kits.items():
        active = [k for k in names if mod.skins[k].get("active")]
        pick = active[-1] if active else max(names, key=lambda k: mod.skins[k]["id"])
        for k in names:
            mod.skins[k]["active"] = k == pick
        for material, texture in mod.skins[pick]["replace"].items():
            mod.write_file(material + "_c", mi.replacement_material(game, material, texture))


def other_mod_ids(data, own_dir):
    ids = set()
    mods = os.path.join(data, "mods")
    if not os.path.isdir(mods):
        return ids
    for name in os.listdir(mods):
        path = os.path.join(mods, name, "items_game.txt")
        if os.path.join(mods, name) == own_dir or not os.path.exists(path):
            continue
        try:
            tree = to_dict(parse_kv(open(path, encoding="utf-8", errors="replace").read()))
        except Exception:
            continue
        ids |= {int(k) for k in tree.get("items_game", {}).get("paint_kits", {}) if k.isdigit()}
    return ids


def add_item(inventory_path, weapon, paint_id, rarity, wear, seed, log=print):
    text = open(inventory_path, encoding="utf-8").read()
    if re.search(r'"def_index"\s+"%d"[^}]*"6"\s+"%d"' % (weapon.def_index, paint_id), text, re.S):
        log(f"  inventory already has {weapon.name} with paint kit {paint_id}")
        return
    quality = 3 if weapon.knife else 4  # knives are ★
    attributes = [(ATTR_PAINT, paint_id), (ATTR_SEED, seed), (ATTR_WEAR, wear)]
    # grenades and the C4 have no place in the game's loadout screen: the item comes equipped in their slot
    equips = [(c, weapon.slot) for c in weapon.classes] if weapon.gear else []
    if equips:
        unequip_slots(inventory_path, equips, ".before_skin_mod")
    new_id = add_inventory_item(inventory_path, weapon.def_index, quality, rarity, attributes, ".before_skin_mod", equips)
    where = f", equipped (slot {weapon.slot})" if equips else ""
    log(f"  added {weapon.name} with paint kit {paint_id} as item {new_id}{where} (backup: inventory.txt.before_skin_mod)")


def unequip_slots(inventory_path, equips, backup_suffix):
    """Take whatever is equipped in these (class, slot) pairs off: items' equipped_state lines and
    "default_equips" entries."""
    text = open(inventory_path, encoding="utf-8").read()
    new = text
    for cls, slot in equips:
        line = re.compile(r'("equipped_state"\s*\{[^{}]*?)\n\s*"%d"\s+"%d"[^\n]*' % (cls, slot))
        while line.search(new):
            new = line.sub(r"\1", new)
        new = re.sub(r'\n\s*"\d+"\s*\{\s*"class_id"\s+"%d"\s*"slot_id"\s+"%d"\s*\}' % (cls, slot), "", new)
    if new != text:
        backup = inventory_path + backup_suffix
        if not os.path.exists(backup):
            shutil.copyfile(inventory_path, backup)
        with open(inventory_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(new)


# ------------------------------------------------------------------------- main

def pick_weapons(game, names):
    weapons = game.weapons()
    picked = []
    for name in names:
        key = name.lower().removeprefix("weapon_")
        if key not in weapons:
            close = [w for w in weapons if key in w]
            hint = f" (did you mean {', '.join(close[:6])}?)" if close else " (--weapons lists them)"
            sys.exit(f"no paintable weapon '{name}'{hint}")
        picked.append(weapons[key])
    return picked


def make_skin(game, mod, weapon, picture, args, used_ids, log=print):
    kit = f"{args.kit}_{weapon.short}"
    size = args.size
    pos = weapon.image("g_tPosition", 0)  # native size
    base = weapon.image("g_tColor", size)
    icon_tex = weapon.base_icon()
    icon_rgba = icon_tex.image(0) if icon_tex else None
    frame = Frame(pos, icon_rgba, weapon.knife or weapon.gear)  # grenades turn like knives to match the icon

    if args.mode == "uv":
        pic = np.asarray(Image.fromarray(picture, "RGBA").resize((size, size), Image.LANCZOS))
        rgb = over(pic.astype(np.float32), base)
    else:
        rgb = side_texture(frame, base, picture, args)
    finish = getattr(args, "finish", "custom")
    spec = FINISHES[finish]
    paint = getattr(args, "paint_mask", None)
    if paint is not None and paint.shape[0] != size:
        paint = np.asarray(Image.fromarray(paint).resize((size, size), Image.BILINEAR))
    rgba = paint_texture(rgb, args.durability, paint if finish == "gunsmith" else None)
    colors = getattr(args, "colors", None) or spec["colors"]
    roughness = getattr(args, "roughness", None)
    pearlescence = getattr(args, "pearlescence", None)

    digest = hashlib.sha1(rgba.tobytes()).hexdigest()[:8]
    texture_path = f"items/assets/paintkits/cs2gc/{kit}_png_{digest}.vtex"
    material_path = f"weapons/paints/cs2gc/{kit}.vcompmat"
    template_red2 = s2.Resource.parse(game.read(WORKSHOP + spec["texture"] + ".vtex_c")).block("RED2")

    old = mod.skins.get(kit)
    paint_id = old["id"] if old else next(i for i in range(args.id or FIRST_ID, 65535) if i not in used_ids)
    used_ids.add(paint_id)
    if old:
        mod.remove_files(kit)

    files = {}
    replace = {}
    if weapon.gear:  # the game won't draw the paint kit: the skin replaces the grenade's own texture
        files, replace = weapon.skin_files(kit, rgb, paint if finish == "gunsmith" else None, finish, size)
    files[texture_path + "_c"] = encode_paint(rgba, template_red2)
    files[material_path + "_c"] = build_material(game, texture_path, finish, colors, roughness, pearlescence)
    icon = render_icon(frame, rgb)
    if weapon.gear and icon_rgba is not None and getattr(frame, "iou", 0) < GEAR_MIN_IOU:
        icon = icon_rgba  # the painted part is a small piece of it (the molotov's rag and cap): the game's icon
    icon_blob = encode_icon(icon, game.read(ICON_BASE.format(weapon=weapon.name)) if icon_tex else None)
    for wear in ("light", "medium", "heavy"):
        files[ICON_GENERATED.format(weapon=weapon.name, kit=kit, wear=wear)] = icon_blob
    for path, blob in files.items():
        mod.write_file(path, blob)

    previews = os.path.join(mod.dir, "previews")
    os.makedirs(previews, exist_ok=True)
    small = Image.fromarray(rgba[..., :3]).resize((ICON_H, ICON_H), Image.LANCZOS).convert("RGBA")
    sheet = Image.new("RGBA", (ICON_W + ICON_H, ICON_H), (40, 40, 40, 255))
    sheet.alpha_composite(Image.fromarray(icon, "RGBA"), (0, 0))
    sheet.alpha_composite(small, (ICON_W, 0))
    sheet.save(os.path.join(previews, kit + ".png"))

    mod.skins[kit] = {
        "id": paint_id, "weapon": weapon.name, "def_index": weapon.def_index, "name": args.name,
        "description": args.description or "", "rarity": args.rarity, "wear_min": args.wear_range[0],
        "wear_max": args.wear_range[1], "material": material_path, "texture": texture_path,
        "image": os.path.abspath(args.image), "mode": args.mode, "files": sorted(files),
        "finish": finish, "durability": args.durability, "colors": colors,
        "roughness": spec["roughness"] if roughness is None else roughness,
        "pearlescence": 0.0 if pearlescence is None else pearlescence,
    }
    if replace:  # the newest skin of a grenade is the one the game shows
        for other in mod.skins.values():
            if other["weapon"] == weapon.name:
                other["active"] = False
        mod.skins[kit].update(replace=replace, active=True)
    kb = sum(len(b) for b in files.values()) // 1024
    log(f"  {weapon.name}: paint kit {paint_id} '{kit}' ({finish}, {size}x{size}, {kb} KB, icon fit {getattr(frame, 'iou', 0):.2f})")
    return kit, paint_id


def export_template(game, weapon, out, size):
    base = weapon.image("g_tColor", size)
    Image.fromarray(base).convert("RGB").save(out)
    print(f"{weapon.name}: {size}x{size} texture layout -> {out}")
    print("paint over it and build with --mode uv (transparent parts keep the weapon's own color)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cs2", required=True, help="CS2 folder (reads game\\csgo\\pak01_dir.vpk)")
    ap.add_argument("--data", help="cs2_gc data folder (by default release\\cs2_gc)")
    ap.add_argument("--image", help="your picture: png, jpg, webp, bmp...")
    ap.add_argument("--weapon", action="append", default=[], help="ak47, awp, deagle, knife_karambit... repeatable")
    ap.add_argument("--weapons", action="store_true", help="list the weapons that take paint kits")
    ap.add_argument("--name", help="skin name shown in game (default: the picture's file name)")
    ap.add_argument("--description", help="text under the name in the inspect panel")
    ap.add_argument("--kit", help="internal paint kit name prefix (default: from the picture's file name)")
    ap.add_argument("--mode", choices=["side", "uv"], default="side",
                    help="side: project the picture onto the weapon's sides (default); uv: the picture is the texture")
    ap.add_argument("--fit", choices=["cover", "contain", "stretch"], default="cover",
                    help="side mode: fill the side and crop (cover), fit whole (contain) or stretch")
    ap.add_argument("--zoom", type=float, default=1.0, help="side mode: picture scale, 2 = twice as big")
    ap.add_argument("--offset", type=float, nargs=2, default=(0.0, 0.0), metavar=("X", "Y"),
                    help="side mode: move the picture, in parts of the weapon's length / height")
    ap.add_argument("--no-mirror", dest="mirror", action="store_false",
                    help="side mode: don't flip the picture on the far side (it then reads mirrored there)")
    ap.add_argument("--size", type=int, default=2048, choices=[512, 1024, 2048, 4096], help="texture size")
    ap.add_argument("--durability", type=float, default=1.0,
                    help="0..1, how well the paint resists wear (lower wears off sooner on worn items)")
    ap.add_argument("--finish", choices=list(FINISHES), default="custom",
                    help="custom (paint), gunsmith (paint over metal: --paint-mask) or patina (colored metal)")
    ap.add_argument("--colors", nargs=4, metavar="HEX",
                    help="gunsmith / patina: base metal, patina tint, patina wear, grime, like c0afa5")
    ap.add_argument("--roughness", type=float, help="0 (glossy) .. 1 (matte), default from the finish")
    ap.add_argument("--pearlescence", type=float, help="-6..6, color shift with the viewing angle (0 = none)")
    ap.add_argument("--paint-mask", help="gunsmith: grey picture over the layout, white = paint, black = bare metal")
    ap.add_argument("--rarity", default="ancient", choices=RARITIES)
    ap.add_argument("--wear-range", type=float, nargs=2, default=(0.0, 1.0), metavar=("MIN", "MAX"))
    ap.add_argument("--id", type=int, help="first paint kit id to try (default 20100)")
    ap.add_argument("--add-item", action="store_true", help="put the skinned weapons into inventory.txt")
    ap.add_argument("--wear", type=float, default=0.01, help="with --add-item: float of the new items")
    ap.add_argument("--seed", type=int, default=0, help="with --add-item: pattern seed")
    ap.add_argument("--mod", default="skins", help="mod folder in <data>\\mods (default skins)")
    ap.add_argument("--mod-name", default="Custom skins", help="mod name in mod.txt (for a new mod)")
    ap.add_argument("--priority", type=int, default=10)
    ap.add_argument("--template", nargs=2, metavar=("WEAPON", "OUT_PNG"), help="save a weapon's texture layout")
    ap.add_argument("--list", action="store_true", help="list the skins in the mod")
    ap.add_argument("--remove", action="append", default=[], metavar="KIT", help="remove a skin (its paint kit name)")
    args = ap.parse_args()

    game = Game(args.cs2)
    if args.weapons:
        for short, w in sorted(game.weapons().items(), key=lambda kv: kv[1].def_index):
            print(f"{w.def_index:5d}  {short}")
        return
    if args.template:
        weapon = pick_weapons(game, [args.template[0]])[0]
        return export_template(game, weapon, args.template[1], args.size)
    args.data = make_data(args.data)
    mod = Mod(args.data, args.mod)
    if args.list:
        for kit, s in sorted(mod.skins.items(), key=lambda kv: kv[1]["id"]):
            print(f"{s['id']:6d}  {kit:40s} {s['weapon']:26s} {s['name']}")
        print(f"{len(mod.skins)} skin(s) in {mod.dir}")
        return
    for kit in args.remove:
        if kit not in mod.skins:
            sys.exit(f"no skin '{kit}' in {mod.dir} (see --list)")
        mod.remove_files(kit)
        del mod.skins[kit]
        print(f"removed {kit}")
    if args.remove:
        update_gear(game, mod)
        mod.save(args.mod_name, args.priority)
        if not args.image:
            return

    if not args.image or not args.weapon:
        sys.exit("give --image and at least one --weapon (or --template / --weapons / --list / --remove)")
    if not 0 <= args.durability <= 1:
        sys.exit("--durability is 0..1")
    stem = os.path.splitext(os.path.basename(args.image))[0]
    args.name = args.name or stem
    args.kit = "cs2gc_" + slug(args.kit or stem)
    weapons = pick_weapons(game, args.weapon)
    picture = load_picture(args.image)
    print(f"{args.image}: {picture.shape[1]}x{picture.shape[0]}, mode {args.mode}, finish {args.finish}")
    if args.colors and not FINISHES[args.finish]["colors"]:
        sys.exit("--colors is for gunsmith and patina")
    for c in args.colors or []:
        hex_rgb(c)
    if args.paint_mask and args.finish != "gunsmith":
        sys.exit("--paint-mask is for --finish gunsmith")
    args.paint_mask = load_mask(args.paint_mask, args.size) if args.paint_mask else None

    used = game.used_paint_ids() | other_mod_ids(args.data, mod.dir)
    used |= {s["id"] for s in mod.skins.values()}  # a skin made again keeps its own id (make_skin)
    made = []
    for weapon in weapons:
        made.append((weapon,) + make_skin(game, mod, weapon, picture, args, used))
    update_gear(game, mod)
    mod.save(args.mod_name, args.priority)
    print(f"wrote {mod.dir} (previews in {os.path.join(mod.dir, 'previews')})")

    if args.add_item:
        if cs2_running():
            print("the game is running, not touching inventory.txt (close the game and run again with --add-item)")
        else:
            paint_rarity = game.rarity_value(args.rarity)
            for weapon, kit, paint_id in made:
                item_rarity = game.rarity_value(game.resolved(weapon.item, "item_rarity") or "common")
                rarity = 6 if weapon.knife else painted_rarity(item_rarity, paint_rarity)
                add_item(os.path.join(args.data, "inventory.txt"), weapon, paint_id, rarity, args.wear, args.seed)
    print('start the game through run_cs2_gc.cmd with "mods" "1" in config.txt')


if __name__ == "__main__":
    main()

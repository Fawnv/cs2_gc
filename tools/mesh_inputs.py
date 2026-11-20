"""Grenades and the C4: their maps baked from the model, and their skins as a replaced color texture.

Guns and knives ship "composite inputs", the maps a paint kit is drawn from: per texel of the paint layout
the 3D position (g_tPosition), the model space normal (g_tSurface), cavity and ambient occlusion
(g_tAmbientOcclusion, red / green). Grenades and the C4 don't, so here they are baked from the model's
mesh (TEXCOORD 0 is the layout, the same one the weapon's textures use) and its own AO texture: the
skin editor draws the 3D model from them and make_skin_mod the inventory icons.

The game doesn't draw a paint kit on them even with such maps (tested: the client never starts the paint
for a grenade), so a skin replaces the weapon's own color texture instead: the mod carries a copy of the
weapon's material with g_tColor pointing at the painted texture. Every such grenade in the game looks
like that, whoever holds it.
"""
import numpy as np
from PIL import Image

import source2 as s2

SIZE = 1024  # the game's own maps are 1024 (position, surface, cavity)
PAD = 8  # texels the islands grow by, like the game's maps (no seams when the result is filtered)
# the game's position maps (checked on the Zeus, Deagle, AK, AWP, Karambit): (y, -x, z) of the model,
# minus the middle of the painted mesh, over the weapon length, plus 0.5. g_tSurface is the mesh normal
# in the same axes, g_tAmbientOcclusion green is the model's own AO texture and red its cavity (about
# 0.62 AO - 0.13 on the game's maps)
AXES = np.array([[0, 1, 0], [-1, 0, 0], [0, 0, 1]], float)
# a loadout slot per flexible_loadout_slot of the items without composite inputs (from client.dll's table)
SLOTS = {"c4": 1, "grenade0": 26, "grenade1": 27, "grenade2": 28, "grenade3": 29, "grenade4": 30, "grenade5": 31}
CLASSES = {"terrorists": 2, "counter-terrorists": 3}


class MeshInputs:
    """The baked maps of one model (what make_skin_mod's Weapon reads from the game for other weapons) and
    its painted material(s)."""

    def __init__(self, game, model_path, size=SIZE):
        self.game, self.model_path, self.size = game, model_path, size
        self.folder = model_path.rsplit("/", 1)[0]
        self._model = self._baked = None
        self._materials = {}

    # -- the model --

    def _load(self):
        if self._model is not None:
            return
        blob = self.game.read(self.model_path + "_c")
        self._model = s2.Model(blob)
        data = s2.KV3(next(d for n, d in s2.Resource.parse(blob).blocks if n == "DATA")).decode()
        # the painted material: the one with the most triangles
        counts = {}
        for d in self._model.draws:
            counts[d["material"]] = counts.get(d["material"], 0) + len(d["triangles"])
        self.material = max(counts, key=counts.get)
        self.draws = [d for d in self._model.draws if d["material"] == self.material]
        # material groups swap it per team (the smoke grenade): a skin replaces those too
        self.materials = [self.material]
        groups = data.get("m_materialGroups") or []
        default = groups[0]["m_materials"] if groups else []
        if self.material in default:
            at = default.index(self.material)
            for group in groups[1:]:
                if at < len(group["m_materials"]) and group["m_materials"][at] not in self.materials:
                    self.materials.append(group["m_materials"][at])

    def textures(self, material=None):
        """{param: texture path} of the painted material (or of one of its team variants)."""
        self._load()
        material = material or self.material
        if material not in self._materials:
            kv = s2.KV3(s2.Resource.parse(self.game.read(material + "_c")).block("DATA")).decode()
            self._materials[material] = {t["m_name"]: t["m_pValue"] for t in kv.get("m_textureParams", [])}
        return self._materials[material]

    @property
    def paths(self):
        """{param: texture path} for the inputs that come straight from the weapon's material."""
        tex = self.textures()
        return {"g_tColor": tex["g_tColor"], "g_tMetalness": tex["g_tMetalness"]}

    # -- baking --

    def baked(self):
        """{param: array} of the baked inputs (float RGBA for the position map, uint8 RGBA else)."""
        if self._baked is not None:
            return self._baked
        self._load()
        n = self.size
        uv = np.concatenate([d["uv"][d["triangles"]] for d in self.draws])
        pos = np.concatenate([d["position"] for d in self.draws])
        lo, hi = pos.min(0), pos.max(0)
        self.length = float((hi - lo).max())
        center = (lo + hi) / 2
        corners = []
        for d in self.draws:
            p = (d["position"] - center) @ AXES.T / self.length + 0.5
            nrm = d["normal"] @ AXES.T if d["normal"] is not None else np.zeros_like(p)
            corners.append(np.concatenate([p, nrm], 1)[d["triangles"]])
        covered, values = rasterize(uv, np.concatenate(corners), n)
        values, grown = dilate(values, covered)
        position = np.zeros((n, n, 4), np.float32)
        position[..., :3] = np.where(grown[..., None], values[..., :3], 0)
        normal = values[..., 3:6]
        normal /= np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-9)
        surface = np.zeros((n, n, 4), np.uint8)
        surface[..., :3] = np.where(grown[..., None], np.clip(np.rint((normal * 0.5 + 0.5) * 255), 0, 255), 128)
        ao = self._ao(n)
        cavity = np.zeros((n, n, 4), np.uint8)
        cavity[..., 0] = np.clip(np.rint(ao * 0.62 - 0.13 * 255), 0, 255)
        cavity[..., 1] = np.rint(ao)
        masks = np.zeros((16, 16, 4), np.uint8)
        masks[..., 0] = 255  # one mask region, like the Zeus
        self.covered = covered
        self._baked = {"g_tPosition": position, "g_tSurface": surface, "g_tAmbientOcclusion": cavity, "g_tMasks": masks}
        return self._baked

    def _ao(self, n):
        path = self.textures().get("g_tAmbientOcclusion")
        if not path:
            return np.full((n, n), 255.0)
        tex = self.game.texture(path)
        level = 0
        while level + 1 < tex.mips and (tex.width >> (level + 1)) >= n:
            level += 1
        img = tex.image(level)
        if img.ndim == 3:
            img = img[..., 0]
        if img.shape != (n, n):
            img = np.asarray(Image.fromarray(img).resize((n, n), Image.BILINEAR))
        return img.astype(np.float64)


def encode_color(rgba, red2):
    """A color texture with all its mips (DXT1, or DXT5 when the alpha isn't all opaque: the weapon
    shader reads the alpha too). Color and alpha are scaled apart (no premultiplying)."""
    alpha = rgba[..., 3].min() < 255
    rgb, a = Image.fromarray(np.ascontiguousarray(rgba[..., :3])), Image.fromarray(np.ascontiguousarray(rgba[..., 3]))
    w, h = rgb.size
    mips = []
    while True:
        level = np.dstack([np.asarray(rgb.resize((w, h), Image.LANCZOS)), np.asarray(a.resize((w, h), Image.LANCZOS))])
        mips.append((s2.encode_dxt(level, alpha), w, h))
        if w <= 4 or h <= 4:
            break
        w, h = w // 2, h // 2
    fmt = s2.FMT_DXT5 if alpha else s2.FMT_DXT1
    return s2.build_texture(None, fmt, mips, 0, s2.fallback_bits(rgba), red2)


def replacement_material(game, material, color_path):
    """The weapon's material with g_tColor swapped for `color_path` (a string swap in its KV3 and RERL,
    everything else as the game has it, dynamic expressions included)."""
    res = s2.Resource.parse(game.read(material + "_c"))
    kv = s2.KV3(res.block("DATA"))
    before = kv.decode()
    params = {t["m_name"]: t["m_pValue"] for t in before["m_textureParams"]}
    old = params["g_tColor"]
    if [n for n, p in params.items() if p == old] != ["g_tColor"] or kv.replace_strings({old: color_path}) != 1:
        raise ValueError(f"{material}: its color texture can't be swapped on its own")
    res.set_block("DATA", kv.build())
    refs = [color_path if p == old else p for _, p in s2.read_rerl(res.block("RERL"))]
    if res.blocks[0][0] != "RERL":
        raise ValueError(f"{material}: RERL isn't the first block")
    res.set_block("RERL", s2.build_rerl(refs, 16 + 12 * len(res.blocks)))
    blob = res.build()
    after = s2.KV3(s2.Resource.parse(blob).block("DATA")).decode()
    params["g_tColor"] = color_path
    if {t["m_name"]: t["m_pValue"] for t in after["m_textureParams"]} != params or             any(after[k] != before[k] for k in before if k != "m_textureParams"):
        raise ValueError(f"{material}: the swapped material didn't come out right")
    return blob


def rasterize(uv, values, size):
    """Draw triangles in layout space. uv: (T, 3, 2) in 0..1, values: (T, 3, C) per corner.
    Returns (covered (size, size) bool, (size, size, C) values interpolated per texel center)."""
    channels = values.shape[-1]
    out = np.zeros((size * size, channels), np.float64)
    covered = np.zeros(size * size, bool)
    p = uv * size - 0.5  # texel centers at integer coordinates
    lo = np.clip(np.floor(p.min(1)).astype(np.int64), 0, size - 1)
    hi = np.clip(np.ceil(p.max(1)).astype(np.int64), 0, size - 1)
    w = hi[:, 0] - lo[:, 0] + 1
    h = hi[:, 1] - lo[:, 1] + 1
    a, b, c = p[:, 0], p[:, 1], p[:, 2]
    det = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    good = np.abs(det) > 1e-12
    order = np.flatnonzero(good)
    # in batches, so the candidate texels of a big model don't need gigabytes
    batch = []
    total = 0
    for t in order:
        batch.append(t)
        total += w[t] * h[t]
        if total > 4_000_000:
            _raster_batch(np.array(batch), p, lo, w, h, det, values, size, out, covered)
            batch, total = [], 0
    if batch:
        _raster_batch(np.array(batch), p, lo, w, h, det, values, size, out, covered)
    return covered.reshape(size, size), out.reshape(size, size, channels)


def _raster_batch(tris, p, lo, w, h, det, values, size, out, covered):
    counts = w[tris] * h[tris]
    tri = np.repeat(tris, counts)
    start = np.repeat(np.cumsum(counts) - counts, counts)
    local = np.arange(len(tri)) - start
    x = lo[tri, 0] + local % w[tri]
    y = lo[tri, 1] + local // w[tri]
    a, b, c = p[tri, 0], p[tri, 1], p[tri, 2]
    l1 = ((x - a[:, 0]) * (c[:, 1] - a[:, 1]) - (y - a[:, 1]) * (c[:, 0] - a[:, 0])) / det[tri]
    l2 = ((b[:, 0] - a[:, 0]) * (y - a[:, 1]) - (b[:, 1] - a[:, 1]) * (x - a[:, 0])) / det[tri]
    l0 = 1 - l1 - l2
    eps = -1e-6
    inside = (l0 >= eps) & (l1 >= eps) & (l2 >= eps)
    tri, x, y = tri[inside], x[inside], y[inside]
    l0, l1, l2 = l0[inside, None], l1[inside, None], l2[inside, None]
    v = values[tri]
    idx = y * size + x
    out[idx] = v[:, 0] * l0 + v[:, 1] * l1 + v[:, 2] * l2
    covered[idx] = True


def dilate(values, covered, steps=PAD):
    """Grow the covered texels outwards by `steps`, each new texel the mean of its covered neighbours."""
    values = values.copy()
    covered = covered.copy()
    for _ in range(steps):
        total = np.zeros_like(values)
        count = np.zeros(covered.shape, np.float64)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
            m = np.roll(covered, (dy, dx), (0, 1))
            total += np.roll(values, (dy, dx), (0, 1)) * m[..., None]
            count += m
        grow = ~covered & (count > 0)
        if not grow.any():
            break
        values[grow] = total[grow] / count[grow, None]
        covered = covered | grow
    return values, covered

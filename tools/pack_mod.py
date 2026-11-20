r"""Pack a CSForge mod into a .zip that the inventory editor's Mods tab installs (Install mod… or drop it there).

    python tools\pack_mod.py --data release\cs2_gc skins
    python tools\pack_mod.py --data release\cs2_gc skins cases --out dist
    python tools\pack_mod.py --data release\cs2_gc skins --with-sources

The zip has the mod's folder at its top (mods\<folder>\... becomes <folder>/... in the zip), so it installs
under the same folder name. The working files of the tools are left out unless --with-sources is given:
sources\ (the skin editor's pictures and layers) and previews\. skins.json and cases.json stay, so the
tools can rebuild the mod after it's installed somewhere else.
"""
import argparse
import os
import sys
import zipfile

WORKING = ("sources", "previews")  # the tools' own files, not loaded by the game
NOT_PACKED = ("__pycache__",)
ZIP_LIMIT = (4 << 30) - (1 << 20)  # the editor's unpacker has no zip64


def pack(mod_dir, out_dir, with_sources=False):
    folder = os.path.basename(os.path.normpath(mod_dir))
    if not os.path.exists(os.path.join(mod_dir, "mod.txt")):
        sys.exit(f"{mod_dir} has no mod.txt: not a CSForge mod")
    entries = []
    for root, dirs, files in os.walk(mod_dir):
        rel_root = os.path.relpath(root, mod_dir)
        top = rel_root.split(os.sep)[0]
        dirs[:] = sorted(d for d in dirs if d not in NOT_PACKED and (with_sources or rel_root != "." or d not in WORKING))
        if rel_root != "." and not with_sources and top in WORKING:
            continue
        for name in sorted(files):
            path = os.path.join(root, name)
            inside = os.path.relpath(path, mod_dir).replace(os.sep, "/")
            entries.append((f"{folder}/{inside}", path))
    total = sum(os.path.getsize(p) for _, p in entries)
    if total > ZIP_LIMIT or len(entries) >= 65535:
        sys.exit(f"{folder}: {total / 1e9:.1f} GB in {len(entries)} files, more than a zip without zip64 holds")

    os.makedirs(out_dir, exist_ok=True)
    zip_path = os.path.join(out_dir, folder + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=False) as z:
        for inside, path in entries:
            # compiled textures and sounds hardly shrink: store them, the editor unpacks stored entries fastest
            stored = inside.endswith(("_c", ".png", ".mp3", ".webm"))
            z.write(path, inside, compress_type=zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED)
    print(f"{zip_path}: {len(entries)} files, {total / 1048576:.1f} MB unpacked, {os.path.getsize(zip_path) / 1048576:.1f} MB")
    return zip_path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, help="the cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("mods", nargs="+", help="mod folders in <data>\\mods")
    ap.add_argument("--out", default=".", help="where the zips go (default here)")
    ap.add_argument("--with-sources", action="store_true", help="pack sources\\ and previews\\ too")
    args = ap.parse_args()
    for name in args.mods:
        pack(os.path.join(args.data, "mods", name), args.out, args.with_sources)


if __name__ == "__main__":
    main()

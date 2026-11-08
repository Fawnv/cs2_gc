r"""Pack a release zip for GitHub Releases.

    python tools/make_release_zip.py --exe <build OUTDIR>\cs2_gc.exe
    python tools/make_release_zip.py --exe ... --source CS2_GC_Public --out dist

Only cs2_gc.exe comes from the build. Everything else comes from the source tree (the public copy
CS2_GC_Public when there is one, where the paths are the default Steam ones, else this repo): the scripts,
LICENSE, README.md, the editor page and the example config.txt, inventory.txt, items_extra.txt
and price_sheet.txt. So a played inventory, a log or a price cache can't end up in the zip. The
layout is the repo's (release\cs2_gc.exe, release\cs2_gc\...), which the scripts expect.
"""
import argparse
import datetime
import os
import subprocess
import sys
import zipfile

DATA_FILES = ["config.txt", "inventory.txt", "items_extra.txt", "price_sheet.txt"]
ROOT_FILES = ["run_cs2_gc.cmd", "cleanup_cs2_folder.cmd", "open_editor.cmd", "LICENSE", "README.md"]


def main():
    repo = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="cs2_gc.exe of a Release build")
    public = os.path.join(repo, "CS2_GC_Public")
    parser.add_argument("--source", default=public if os.path.isdir(public) else repo)
    parser.add_argument("--out", default=os.path.join(repo, "dist"))
    parser.add_argument("--name", help="zip name without .zip (default cs2_gc-<date>-<commit>)")
    args = parser.parse_args()

    source = os.path.abspath(args.source)
    missing = [p for p in ROOT_FILES + [os.path.join("examples", f) for f in DATA_FILES] + ["editor"]
               if not os.path.exists(os.path.join(source, p))]
    if missing or not os.path.isfile(args.exe):
        sys.exit(f"missing: {', '.join(missing) or args.exe}")

    commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=repo, text=True).strip()
    name = args.name or f"cs2_gc-{datetime.date.today().isoformat()}-{commit}"
    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, name + ".zip")

    entries = []  # (path in the zip, file on disk)
    for f in ROOT_FILES:
        entries.append((f, os.path.join(source, f)))
    entries.append(("README.txt", os.path.join(repo, "tools", "release_readme.txt")))
    entries.append(("release/cs2_gc.exe", args.exe))
    for f in DATA_FILES:
        entries.append((f"release/cs2_gc/{f}", os.path.join(source, "examples", f)))
    editor = os.path.join(source, "editor")
    for root, _, files in os.walk(editor):
        for f in sorted(files):
            path = os.path.join(root, f)
            entries.append(("release/cs2_gc/editor/" + os.path.relpath(path, editor).replace(os.sep, "/"), path))

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for inside, path in entries:
            data = open(path, "rb").read()
            if inside.endswith((".cmd", ".txt")) and not inside.startswith("release/cs2_gc/editor/"):
                # Notepad and cmd.exe want CRLF
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            z.writestr(f"{name}/{inside}", data)

    print(f"{zip_path}: {len(entries)} files, {os.path.getsize(zip_path) / 1e6:.1f} MB (commit {commit})")


if __name__ == "__main__":
    main()

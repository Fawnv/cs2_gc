r"""Pack a release archive (zip / tar.gz) for GitHub Releases.

    python tools/make_release_zip.py --exe release/cs2_gc.exe             (Windows zip)
    python tools/make_release_zip.py --bin release_linux/cs2_gc_launcher   (Linux tar.gz + zip)
    python tools/make_release_zip.py --all                                (both)

Only the binaries come from the build. Everything else comes from the source tree (the public copy
CS2_GC_Public when there is one, where the paths are the default Steam ones, else this repo): scripts,
LICENSE, README.md, the editor page and example data files (config.txt, inventory.txt, etc.).
"""
import argparse
import datetime
import io
import os
import stat
import subprocess
import sys
import tarfile
import time
import zipfile

DATA_FILES = ["config.txt", "inventory.txt", "items_extra.txt", "price_sheet.txt"]
ROOT_FILES_WIN = ["run_cs2_gc.cmd", "cleanup_cs2_folder.cmd", "open_editor.cmd", "LICENSE", "README.md"]
ROOT_FILES_LINUX = ["run_cs2_gc.sh", "cleanup_cs2_folder.sh", "LICENSE", "README.md"]


def is_elf_binary(path):
    if not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def pack_windows(repo, source, exe_path, out_dir, base_name, commit):
    missing = [p for p in ROOT_FILES_WIN + [os.path.join("examples", f) for f in DATA_FILES] + ["editor"]
               if not os.path.exists(os.path.join(source, p))]
    if missing or not os.path.isfile(exe_path):
        sys.exit(f"[Windows] missing: {', '.join(missing) or exe_path}")

    name = base_name or f"cs2_gc-{datetime.date.today().isoformat()}-{commit}"
    zip_path = os.path.join(out_dir, name + ".zip")

    entries = []  # (inside_path, full_path_on_disk)
    for f in ROOT_FILES_WIN:
        entries.append((f, os.path.join(source, f)))
    readme_path = os.path.join(repo, "tools", "release_readme.txt")
    if os.path.exists(readme_path):
        entries.append(("README.txt", readme_path))
    entries.append(("release/cs2_gc.exe", exe_path))
    for f in DATA_FILES:
        entries.append((f"release/cs2_gc/{f}", os.path.join(source, "examples", f)))

    builtin = os.path.join(source, "mods")
    if os.path.isdir(builtin):
        for root, _, files in os.walk(builtin):
            for f in sorted(files):
                path = os.path.join(root, f)
                entries.append(("release/cs2_gc/mods/" + os.path.relpath(path, builtin).replace(os.sep, "/"), path))

    editor = os.path.join(source, "editor")
    for root, _, files in os.walk(editor):
        for f in sorted(files):
            path = os.path.join(root, f)
            entries.append(("release/cs2_gc/editor/" + os.path.relpath(path, editor).replace(os.sep, "/"), path))

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for inside, path in entries:
            data = open(path, "rb").read()
            if inside.endswith((".cmd", ".txt")) and not inside.startswith(("release/cs2_gc/editor/", "release/cs2_gc/mods/")):
                # Notepad and cmd.exe want CRLF
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            z.writestr(f"{name}/{inside}", data)

    print(f"Windows: {zip_path} ({len(entries)} files, {os.path.getsize(zip_path) / 1e6:.1f} MB)")
    return zip_path


def pack_linux(repo, source, bin_path, out_dir, base_name, commit, formats=("tar.gz", "zip")):
    missing = [p for p in ROOT_FILES_LINUX + [os.path.join("examples", f) for f in DATA_FILES] + ["editor"]
               if not os.path.exists(os.path.join(source, p))]
    if missing or not os.path.isfile(bin_path):
        sys.exit(f"[Linux] missing: {', '.join(missing) or bin_path}")

    default_name = f"cs2_gc-{datetime.date.today().isoformat()}-{commit}-linux-x64"
    name = (base_name + "-linux-x64") if base_name else default_name

    entries = []  # (inside_path, file_data, is_executable)
    for f in ROOT_FILES_LINUX:
        data = open(os.path.join(source, f), "rb").read()
        is_exec = f.endswith(".sh")
        if is_exec or f.endswith(".md"):
            data = data.replace(b"\r\n", b"\n")
        entries.append((f, data, is_exec))

    readme_path = os.path.join(repo, "tools", "release_readme_linux.txt")
    if not os.path.exists(readme_path):
        readme_path = os.path.join(repo, "tools", "release_readme.txt")
    if os.path.exists(readme_path):
        readme_data = open(readme_path, "rb").read().replace(b"\r\n", b"\n")
        entries.append(("README.txt", readme_data, False))

    bin_data = open(bin_path, "rb").read()
    entries.append(("release_linux/cs2_gc_launcher", bin_data, True))

    for f in DATA_FILES:
        data = open(os.path.join(source, "examples", f), "rb").read().replace(b"\r\n", b"\n")
        entries.append((f"release_linux/cs2_gc/{f}", data, False))

    builtin = os.path.join(source, "mods")
    if os.path.isdir(builtin):
        for root, _, files in os.walk(builtin):
            for f in sorted(files):
                path = os.path.join(root, f)
                data = open(path, "rb").read()
                rel = os.path.relpath(path, builtin).replace(os.sep, "/")
                entries.append((f"release_linux/cs2_gc/mods/{rel}", data, False))

    editor = os.path.join(source, "editor")
    for root, _, files in os.walk(editor):
        for f in sorted(files):
            path = os.path.join(root, f)
            data = open(path, "rb").read()
            rel = os.path.relpath(path, editor).replace(os.sep, "/")
            entries.append((f"release_linux/cs2_gc/editor/{rel}", data, False))

    results = []
    if "tar.gz" in formats:
        tar_path = os.path.join(out_dir, name + ".tar.gz")
        now = int(time.time())
        with tarfile.open(tar_path, "w:gz", compresslevel=9) as tar:
            for inside, data, is_exec in entries:
                ti = tarfile.TarInfo(name=f"{name}/{inside}")
                ti.size = len(data)
                ti.mtime = now
                ti.mode = 0o755 if is_exec else 0o644
                tar.addfile(ti, io.BytesIO(data))
        print(f"Linux (tar.gz): {tar_path} ({len(entries)} files, {os.path.getsize(tar_path) / 1e6:.1f} MB)")
        results.append(tar_path)

    if "zip" in formats:
        zip_path = os.path.join(out_dir, name + ".zip")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            now_tuple = datetime.datetime.now().timetuple()[:6]
            for inside, data, is_exec in entries:
                zi = zipfile.ZipInfo(f"{name}/{inside}", now_tuple)
                zi.compress_type = zipfile.ZIP_DEFLATED
                # Store POSIX file permissions in the external attributes
                mode = (stat.S_IFREG | 0o755) if is_exec else (stat.S_IFREG | 0o644)
                zi.external_attr = mode << 16
                z.writestr(zi, data)
        print(f"Linux (zip):    {zip_path} ({len(entries)} files, {os.path.getsize(zip_path) / 1e6:.1f} MB)")
        results.append(zip_path)

    return results


def main():
    repo = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", help="cs2_gc.exe of a Windows Release build (or ELF binary)")
    parser.add_argument("--bin", "--launcher", dest="launcher", help="cs2_gc_launcher of a Linux Release build")
    parser.add_argument("--target", choices=["windows", "linux", "all", "auto"], default="auto")
    public = os.path.join(repo, "CS2_GC_Public")
    parser.add_argument("--source", default=public if os.path.isdir(public) else repo)
    parser.add_argument("--out", default=os.path.join(repo, "dist"))
    parser.add_argument("--name", help="archive base name (default cs2_gc-<date>-<commit>)")
    parser.add_argument("--format", choices=["zip", "tar.gz", "both"], default="both",
                        help="Linux archive format (default: both)")
    parser.add_argument("--all", action="store_true", help="build both Windows and Linux release archives")
    args = parser.parse_args()

    source = os.path.abspath(args.source)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    commit = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=repo, text=True).strip()

    # Determine what to build
    build_win = False
    build_linux = False

    exe = args.exe
    launcher = args.launcher

    if args.all or args.target == "all":
        build_win = True
        build_linux = True
    elif args.target == "windows":
        build_win = True
    elif args.target == "linux":
        build_linux = True
    elif launcher:
        build_linux = True
    elif exe:
        if is_elf_binary(exe):
            launcher = exe
            build_linux = True
        else:
            build_win = True
    else:
        # Auto-detect from default paths
        default_exe = os.path.join(repo, "release", "cs2_gc.exe")
        default_linux = os.path.join(repo, "release_linux", "cs2_gc_launcher")
        if os.path.isfile(default_exe):
            exe = default_exe
            build_win = True
        if os.path.isfile(default_linux):
            launcher = default_linux
            build_linux = True

    if not build_win and not build_linux:
        sys.exit("No build binaries found or specified. Pass --exe <path> or --bin <path>.")

    if build_win:
        if not exe:
            exe = os.path.join(repo, "release", "cs2_gc.exe")
        pack_windows(repo, source, exe, out_dir, args.name, commit)

    if build_linux:
        if not launcher:
            launcher = os.path.join(repo, "release_linux", "cs2_gc_launcher")
        linux_formats = ["tar.gz", "zip"] if args.format == "both" else [args.format]
        pack_linux(repo, source, launcher, out_dir, args.name, commit, formats=linux_formats)


if __name__ == "__main__":
    main()

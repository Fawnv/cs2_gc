"""Read-only VPK v1/v2 reader (Valve pak01_dir.vpk).

    python tools/vpk_extract.py <pak01_dir.vpk> list [substring]
    python tools/vpk_extract.py <pak01_dir.vpk> get <path/in/vpk> <out_file>

Only reads the .vpk files; the output file is written wherever you point it.
"""
import os
import struct
import sys

VPK_SIGNATURE = 0x55AA1234
DIR_ARCHIVE = 0x7FFF


def read_cstr(buf, pos):
    end = buf.index(b"\0", pos)
    return buf[pos:end].decode("utf-8", "replace"), end + 1


def read_tree(path):
    with open(path, "rb") as f:
        data = f.read()
    sig, version, tree_size = struct.unpack_from("<III", data, 0)
    if sig != VPK_SIGNATURE:
        raise ValueError("not a vpk")
    header_size = 12 if version == 1 else 28
    pos = header_size
    entries = {}
    while True:
        ext, pos = read_cstr(data, pos)
        if not ext:
            break
        while True:
            folder, pos = read_cstr(data, pos)
            if not folder:
                break
            while True:
                name, pos = read_cstr(data, pos)
                if not name:
                    break
                crc, preload, archive, offset, length, term = struct.unpack_from("<IHHIIH", data, pos)
                pos += 18
                preload_data = data[pos:pos + preload]
                pos += preload
                full = (f"{folder}/" if folder != " " else "") + name + (f".{ext}" if ext != " " else "")
                entries[full] = (archive, offset, length, preload_data)
    return data, header_size + tree_size, entries


def read_file(dir_path, entry, dir_data, data_start):
    archive, offset, length, preload = entry
    if not length:
        return preload
    if archive == DIR_ARCHIVE:
        return preload + dir_data[data_start + offset:data_start + offset + length]
    archive_path = dir_path.replace("_dir.vpk", f"_{archive:03d}.vpk")
    with open(archive_path, "rb") as f:
        f.seek(offset)
        return preload + f.read(length)


def main():
    vpk, cmd = sys.argv[1], sys.argv[2]
    dir_data, data_start, entries = read_tree(vpk)
    if cmd == "list":
        flt = sys.argv[3] if len(sys.argv) > 3 else ""
        for name in sorted(entries):
            if flt in name:
                print(name)
    elif cmd == "get":
        name, out = sys.argv[3], sys.argv[4]
        blob = read_file(vpk, entries[name], dir_data, data_start)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "wb") as f:
            f.write(blob)
        print(f"{name}: {len(blob)} bytes -> {out}")


if __name__ == "__main__":
    main()

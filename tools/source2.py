"""Read and write the compiled Source 2 resources a skin needs: .vtex_c textures and .vcompmat_c materials.

Only what cs2_gc's mod tools use, worked out from the game's own files:
  resource      header (file size, header version 12, type version, block table) + blocks aligned to 16
  RERL          external references: MurmurHash64B(path, 0xEDABCDEF) + path, sorted by path
  KV3 v4 / v5   the binary KeyValues3 in DATA and RED2 (LZ4 compressed, 16 KB frames)
  VTEX          texture header + extra data, pixel data after the blocks, smallest mip first
"""
import struct

import numpy as np

RESOURCE_HEADER_VERSION = 12
KV3_MAGIC = b"3VK"
KV3_TRAILER = b"\x00\xdd\xee\xff"
LZ4_FRAME = 0x4000
ID_SEED = 0xEDABCDEF


def _align(pos, count, size):
    """KV3 pads before a section only when the section isn't empty."""
    return (pos + size - 1) & ~(size - 1) if count else pos


# ----------------------------------------------------------------------------- lz4

def lz4_decompress(src, size):
    """One LZ4 block -> `size` bytes."""
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        length = token >> 4
        if length == 15:
            while True:
                b = src[i]
                i += 1
                length += b
                if b != 255:
                    break
        dst += src[i:i + length]
        i += length
        if i >= n:
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        length = token & 15
        if length == 15:
            while True:
                b = src[i]
                i += 1
                length += b
                if b != 255:
                    break
        length += 4
        start = len(dst) - offset
        if offset >= length:
            dst += dst[start:start + length]
        else:  # overlapping copy = the last `offset` bytes repeated
            pattern = bytes(dst[start:])
            dst += (pattern * (length // offset + 1))[:length]
    if len(dst) != size:
        raise ValueError(f"lz4: got {len(dst)} bytes, expected {size}")
    return bytes(dst)


def _lz4_length(n):
    return b"\xff" * (n // 255) + bytes([n % 255])


def lz4_compress_runs(data, unit=4):
    """LZ4 block that only finds runs of a repeated `unit`-byte value (transparent icon background,
    flat texture areas). Simple, fast with numpy, and any LZ4 decoder reads it."""
    n = len(data) // unit
    if n < 4 or len(data) % unit:
        return lz4_literals(data)
    view = np.frombuffer(data, dtype=f"V{unit}")
    rep = np.zeros(n + 1, bool)
    rep[1:n] = view[1:] == view[:-1]
    edges = np.flatnonzero(np.diff(rep.astype(np.int8)))
    starts, ends = edges[0::2] + 1, edges[1::2] + 1  # runs of repeats: pixels [start, end)
    out = bytearray()
    lit = 0
    limit = len(data) - 12  # the last match has to start 12 bytes before the end, the last 5 are literals
    for s, e in zip(starts.tolist(), ends.tolist()):
        if e - s < 2:
            continue
        m0, m1 = s * unit, min(e * unit, len(data) - 5)
        if m0 > limit or m1 - m0 < 4:
            break
        literals = data[lit:m0]
        length = m1 - m0 - 4
        out.append((min(len(literals), 15) << 4) | min(length, 15))
        if len(literals) >= 15:
            out += _lz4_length(len(literals) - 15)
        out += literals
        out += struct.pack("<H", unit)
        if length >= 15:
            out += _lz4_length(length - 15)
        lit = m1
    rest = data[lit:]
    out.append(min(len(rest), 15) << 4)
    if len(rest) >= 15:
        out += _lz4_length(len(rest) - 15)
    out += rest
    return bytes(out)


def lz4_literals(data):
    """A valid LZ4 block that stores `data` as literals only (any LZ4 decoder reads it back)."""
    n = len(data)
    if n < 15:
        return bytes([n << 4]) + data
    rest = n - 15
    return bytes([0xF0]) + b"\xff" * (rest // 255) + bytes([rest % 255]) + data


# ---------------------------------------------------------------------------- hash

def murmur64b(data, seed=ID_SEED):
    m, r, mask = 0x5BD1E995, 24, 0xFFFFFFFF
    h1 = (seed ^ len(data)) & mask
    h2 = (seed >> 32) & mask
    i, left = 0, len(data)

    def mix(h, k):
        k = (k * m) & mask
        k ^= k >> r
        k = (k * m) & mask
        return ((h * m) & mask) ^ k

    while left >= 8:
        h1 = mix(h1, int.from_bytes(data[i:i + 4], "little"))
        h2 = mix(h2, int.from_bytes(data[i + 4:i + 8], "little"))
        i += 8
        left -= 8
    if left >= 4:
        h1 = mix(h1, int.from_bytes(data[i:i + 4], "little"))
        i += 4
        left -= 4
    if left == 3:
        h2 ^= data[i + 2] << 16
    if left >= 2:
        h2 ^= data[i + 1] << 8
    if left >= 1:
        h2 ^= data[i]
        h2 = (h2 * m) & mask
    h1 ^= h2 >> 18
    h1 = (h1 * m) & mask
    h2 ^= h1 >> 22
    h2 = (h2 * m) & mask
    h1 ^= h2 >> 17
    h1 = (h1 * m) & mask
    h2 ^= h1 >> 19
    h2 = (h2 * m) & mask
    return (h1 << 32) | h2


def resource_id(path):
    """The id RERL keeps for a resource path such as "materials/x_tga_1234.vtex"."""
    return murmur64b(path.lower().encode())


# ------------------------------------------------------------------------ resource

class Resource:
    """A compiled resource: type version, ordered blocks and (for textures) the data after them."""

    def __init__(self, type_version, blocks, tail=b""):
        self.type_version = type_version
        self.blocks = blocks  # [(fourcc, bytes)]
        self.tail = tail

    @classmethod
    def parse(cls, blob):
        size, header_version, type_version, table, count = struct.unpack_from("<IHHII", blob, 0)
        if header_version != RESOURCE_HEADER_VERSION:
            raise ValueError(f"resource header version {header_version}")
        blocks = []
        pos = 8 + table
        end = 0
        for _ in range(count):
            fourcc = blob[pos:pos + 4].decode("latin1")
            offset, length = struct.unpack_from("<II", blob, pos + 4)
            start = pos + 4 + offset
            blocks.append((fourcc, blob[start:start + length]))
            end = max(end, start + length)
            pos += 12
        return cls(type_version, blocks, blob[size:] if size < len(blob) else b"")

    def block(self, fourcc):
        return next((data for name, data in self.blocks if name == fourcc), None)

    def set_block(self, fourcc, data):
        self.blocks = [(name, data if name == fourcc else old) for name, old in self.blocks]

    def build(self):
        table = 16 + 12 * len(self.blocks)
        out = bytearray(table)
        entries = []
        for fourcc, data in self.blocks:
            out += b"\0" * (-len(out) % (4 if fourcc == "RERL" else 16))
            entries.append((fourcc, len(out), len(data)))
            out += data
        size = len(out)
        struct.pack_into("<IHHII", out, 0, size, RESOURCE_HEADER_VERSION, self.type_version, 8, len(self.blocks))
        for i, (fourcc, start, length) in enumerate(entries):
            pos = 16 + 12 * i
            out[pos:pos + 4] = fourcc.encode("latin1")
            struct.pack_into("<II", out, pos + 4, start - (pos + 4), length)
        return bytes(out) + self.tail


def read_rerl(block):
    offset, count = struct.unpack_from("<II", block, 0)
    refs = []
    for i in range(count):
        pos = offset + 16 * i
        rid, name_offset = struct.unpack_from("<QI", block, pos)
        start = pos + 8 + name_offset
        refs.append((rid, block[start:block.index(b"\0", start)].decode()))
    return refs


def build_rerl(paths, block_start=52):
    """RERL for these paths. `block_start` is where the block lands in the file (52 with 3 blocks):
    the ids are 8-aligned in the file, like the compiler writes them."""
    paths = sorted(set(paths), key=str.lower)
    offset = 8 + (-(block_start + 8) % 8)
    out = bytearray(struct.pack("<II", offset, len(paths)))
    out += b"\0" * (offset - 8)
    table = len(out)
    out += b"\0" * (16 * len(paths))
    for i, path in enumerate(paths):
        pos = table + 16 * i
        struct.pack_into("<QI", out, pos, resource_id(path), len(out) - (pos + 8))
        out += path.encode() + b"\0"
    return bytes(out)


# ----------------------------------------------------------------------------- kv3

# binary KV3 value types (the low 7 bits; 0x80 means a flag byte follows the type)
T_NULL, T_BOOL, T_INT64, T_UINT64, T_DOUBLE, T_STRING, T_BLOB, T_ARRAY, T_OBJECT = 1, 2, 3, 4, 5, 6, 7, 8, 9
T_ARRAY_TYPED, T_INT32, T_UINT32, T_TRUE, T_FALSE, T_INT64_0, T_INT64_1, T_DOUBLE_0, T_DOUBLE_1 = range(10, 19)
T_FLOAT, T_INT16, T_UINT16, T_UINT8, T_INT32_AS_BYTE, T_ARRAY_BYTE_LEN, T_ARRAY_AUX = 19, 20, 21, 22, 23, 24, 25


class KV3:
    """A binary KV3 block (version 4 or 5) split into its buffers, so strings can be swapped and
    the block written back. `decode()` returns the values as Python objects."""

    def __init__(self, block):
        if block[1:4] != KV3_MAGIC or block[0] not in (4, 5):
            raise ValueError("not a KV3 v4/v5 block")
        self.version = block[0]
        self.format = block[4:20]
        if self.version == 4:
            self.h = list(struct.unpack_from("<13I", block, 20))
            header_end = 20 + 13 * 4
        else:
            self.h = list(struct.unpack_from("<25I", block, 20))
            header_end = 20 + 25 * 4
        h = self.h
        self.compression = h[0]
        self.frame = h[1]
        if h[11] or (self.version == 5 and h[18]):
            raise ValueError("KV3 with 2-byte values isn't supported")
        # h[9], h[10]: binary blobs (a material's dynamic expressions). Their data comes after the
        # buffers and their sizes sit in the buffers past the types: kept as they are, so only
        # build() (string swaps) works on such a block, not decode() / nodes()
        self.has_blobs = bool(h[9] or h[10])
        data = block[header_end:]
        if self.version == 4:
            raw = self._unpack(data[:h[8]], h[7])
            self.buffers = [raw]
            self.blob_tail = bytes(data[h[8]:])
        else:
            self.buffers = [self._unpack(data[:h[14]], h[13]), self._unpack(data[h[14]:h[14] + h[16]], h[15])]
            self.blob_tail = bytes(data[h[14] + h[16]:])
        self._split()

    def _unpack(self, data, size):
        if self.compression == 0:
            return bytes(data[:size])
        if self.compression == 1:
            if size > LZ4_FRAME:
                raise ValueError("KV3 buffer over one LZ4 frame isn't supported")
            return lz4_decompress(data, size)
        raise ValueError(f"KV3 compression {self.compression} isn't supported")

    def _split(self):
        """Find the strings in the buffers."""
        h = self.h
        if self.version == 4:
            raw = self.buffers[0]
            n1, n4, n8 = h[2], h[3], h[4]
            ints_at = _align(n1, n4, 4)
            count, = struct.unpack_from("<I", raw, ints_at)
            doubles_at = _align(ints_at + 4 * n4, n8, 8)
            strings_at = doubles_at + 8 * n8
            pos = strings_at
            strings = []
            for _ in range(count):
                end = raw.index(b"\0", pos)
                strings.append(raw[pos:end].decode("utf-8"))
                pos = end + 1
            self.strings = strings
            self._strings_span = (strings_at, pos)
        else:
            # buffer 1: bytes (strings, then array data), ints (string count, then array data), doubles
            raw = self.buffers[0]
            n1, n4, n8 = h[2], h[3], h[4]
            ints_at = _align(n1, n4, 4)
            doubles_at = _align(ints_at + 4 * n4, n8, 8)
            count, = struct.unpack_from("<I", raw, ints_at)
            pos = 0
            strings = []
            for _ in range(count):
                end = raw.index(b"\0", pos)
                strings.append(raw[pos:end].decode("utf-8"))
                pos = end + 1
            self.strings = strings
            self._strings_span = (0, pos)
            self._aux = (raw[pos:n1], raw[ints_at + 4:ints_at + 4 * n4], raw[doubles_at:doubles_at + 8 * n8])

    def replace_strings(self, mapping):
        """Swap whole strings (old -> new). Returns how many were replaced."""
        hits = 0
        for i, s in enumerate(self.strings):
            if s in mapping:
                self.strings[i] = mapping[s]
                hits += 1
        return hits

    def build(self):
        h = list(self.h)
        start, end = self._strings_span
        packed = b"".join(s.encode("utf-8") + b"\0" for s in self.strings)
        grow = len(packed) - (end - start)
        if self.version == 4:
            raw = self.buffers[0]
            raw = raw[:start] + packed + raw[end:]
            h[5] += grow  # strings + types size
            buffers = [raw]
        else:
            aux_bytes, aux_ints, aux_doubles = self._aux
            n1, n4, n8 = h[2] + grow, h[3], h[4]
            raw = bytearray(packed + aux_bytes)
            raw += b"\0" * (_align(n1, n4, 4) - n1)
            raw += struct.pack("<I", len(self.strings)) + aux_ints
            raw += b"\0" * (_align(len(raw), n8, 8) - len(raw))
            raw += aux_doubles
            h[2] = n1
            buffers = [bytes(raw), self.buffers[1]]
        if any(len(b) > LZ4_FRAME for b in buffers):
            raise ValueError("KV3 buffer grew over 16 KB")
        blobs = [lz4_literals(b) if h[0] == 1 else b for b in buffers]
        if self.version == 4:
            h[7], h[8] = len(buffers[0]), len(blobs[0])
        else:
            h[13], h[14], h[15], h[16] = len(buffers[0]), len(blobs[0]), len(buffers[1]), len(blobs[1])
            h[7], h[8] = h[13] + h[15], h[14] + h[16]
        head = bytes([self.version]) + KV3_MAGIC + self.format + struct.pack(f"<{len(h)}I", *h)
        return head + b"".join(blobs) + self.blob_tail

    # -- full decode (for looking at values) --

    def _types_end(self, raw):
        """Where the types stop: before the trailer, and before the blob sizes when there are blobs."""
        if not self.has_blobs:
            return len(raw) - 4
        return raw.rindex(KV3_TRAILER) - 4 * self.h[9]

    def decode(self):
        """The values as Python objects (binary blobs come out as None)."""
        h = self.h
        if self.version == 4:
            raw = self.buffers[0]
            n1, n4, n8 = h[2], h[3], h[4]
            ints_at = _align(n1, n4, 4)
            doubles_at = _align(ints_at + 4 * n4, n8, 8)
            types_at = self._strings_span[1]
            types_end = self._types_end(raw)
            reader = _Reader(raw[:n1], raw[ints_at:ints_at + 4 * n4], raw[doubles_at:doubles_at + 8 * n8],
                             raw[types_at:types_end], counts=None)
            reader.ints_pos = 4  # the string count
        else:
            raw1, raw2 = self.buffers
            n1, n4, n8 = h[17], h[19], h[20]
            objects = h[22]
            counts_end = 4 * objects
            bytes_at = counts_end
            ints_at = _align(bytes_at + n1, n4, 4)
            doubles_at = _align(ints_at + 4 * n4, n8, 8)
            types_at = doubles_at + 8 * n8
            reader = _Reader(raw2[bytes_at:bytes_at + n1], raw2[ints_at:ints_at + 4 * n4],
                             raw2[doubles_at:doubles_at + 8 * n8], raw2[types_at:self._types_end(raw2)],
                             counts=raw2[:counts_end])
            reader.aux = _Reader(*self._aux, types=b"", counts=None)
            reader.aux.strings = self.strings
        reader.strings = self.strings
        value = reader.value()
        reader.check_done()
        return value


    # -- values that can be changed and written back (version 4 only) --

    def nodes(self):
        """The block as a tree of Node, which keeps every value's KV3 type so `encode` can write the
        block back byte for byte."""
        if self.version != 4 or self.has_blobs:
            raise ValueError("only KV3 v4 without binary blobs can be written back")
        h, raw = self.h, self.buffers[0]
        n1, n4, n8 = h[2], h[3], h[4]
        ints_at = _align(n1, n4, 4)
        doubles_at = _align(ints_at + 4 * n4, n8, 8)
        reader = _NodeReader(raw[:n1], raw[ints_at:ints_at + 4 * n4], raw[doubles_at:doubles_at + 8 * n8],
                             raw[self._strings_span[1]:len(raw) - 4], counts=None)
        reader.ints_pos = 4
        reader.strings = self.strings
        root = reader.node()
        reader.check_done()
        return root

    def encode(self, root):
        """Write a Node tree (from `nodes`, values changed) into this block. Same objects and arrays."""
        w = _Writer(self.strings)
        w.node(root)
        raw = bytearray(w.b1)
        n4, n8 = len(w.b4) // 4, len(w.b8) // 8
        raw += b"\0" * (_align(len(raw), n4, 4) - len(raw))
        struct.pack_into("<I", w.b4, 0, len(w.strings))
        raw += w.b4
        raw += b"\0" * (_align(len(raw), n8, 8) - len(raw))
        raw += w.b8
        old = self.buffers[0]
        old_tail = len(old) - self._strings_span[0]  # strings, types and the trailer
        tail = b"".join(s.encode("utf-8") + b"\0" for s in w.strings) + w.types + KV3_TRAILER
        raw += tail
        h = self.h
        h[2], h[3], h[4] = len(w.b1), n4, n8
        h[5] += len(tail) - old_tail
        h[7] = len(raw)
        self.buffers = [bytes(raw)]
        self._split()


class Node:
    """One KV3 value with its type: v is a scalar, a list of Node, or for objects a list of
    (key, key string index, Node)."""
    __slots__ = ("t", "v", "flag", "sub", "idx")

    def __init__(self, t, v=None, flag=None, sub=None, idx=None):
        self.t, self.v, self.flag, self.sub, self.idx = t, v, flag, sub, idx

    def get(self, key):
        return next((n for k, _, n in self.v if k == key), None) if self.t == T_OBJECT else None

    def plain(self):
        if self.t == T_OBJECT:
            return {k: n.plain() for k, _, n in self.v}
        if isinstance(self.v, list):
            return [n.plain() for n in self.v]
        return self.v

    def set(self, value):
        """A new scalar value of the same kind (bool, number, string)."""
        t = self.t
        if t in (T_TRUE, T_FALSE, T_BOOL):
            if t == T_BOOL:
                self.v = bool(value)
            else:
                self.t, self.v = (T_TRUE, True) if value else (T_FALSE, False)
        elif t in (T_DOUBLE, T_DOUBLE_0, T_DOUBLE_1):
            value = float(value)
            self.t = T_DOUBLE_0 if value == 0.0 else T_DOUBLE_1 if value == 1.0 else T_DOUBLE
            self.v = value
        elif t == T_FLOAT:
            self.v = float(value)
        elif t == T_INT32_AS_BYTE:
            if not 0 <= int(value) <= 255:
                raise ValueError("value doesn't fit a byte")
            self.v = int(value)
        elif t in (T_INT32, T_UINT32, T_INT64, T_UINT64):
            self.v = int(value)
        elif t in (T_INT64_0, T_INT64_1):
            if int(value) not in (0, 1):
                self.t = T_INT64
            else:
                self.t = T_INT64_0 + int(value)
            self.v = int(value)
        elif t == T_STRING:
            self.v, self.idx = str(value), None
        else:
            raise ValueError(f"can't set a KV3 value of type {t}")


SCALAR_TYPES = (T_NULL, T_BOOL, T_INT64, T_UINT64, T_DOUBLE, T_INT32, T_UINT32, T_TRUE, T_FALSE, T_INT64_0,
                T_INT64_1, T_DOUBLE_0, T_DOUBLE_1, T_FLOAT, T_INT32_AS_BYTE)


class _Reader:
    def __init__(self, b1, b4, b8, types, counts):
        self.b1, self.b4, self.b8, self.types, self.counts = b1, b4, b8, types, counts
        self.p1 = self.ints_pos = self.p8 = self.pt = self.pc = 0
        self.aux = None
        self.aux_p1 = self.aux_p4 = 0

    def u8(self):
        v = self.b1[self.p1]
        self.p1 += 1
        return v

    def i32(self):
        v, = struct.unpack_from("<i", self.b4, self.ints_pos)
        self.ints_pos += 4
        return v

    def f32(self):
        v, = struct.unpack_from("<f", self.b4, self.ints_pos)
        self.ints_pos += 4
        return v

    def f64(self, fmt="<d"):
        v, = struct.unpack_from(fmt, self.b8, self.p8)
        self.p8 += 8
        return v

    def count(self):
        if self.counts is None:
            return self.i32()
        v, = struct.unpack_from("<I", self.counts, self.pc)
        self.pc += 4
        return v

    def type(self):
        t = self.types[self.pt]
        self.pt += 1
        flag = 0
        if t & 0x80:
            flag = self.types[self.pt]
            self.pt += 1
        return t & 0x7F, flag

    def string(self):
        i = self.i32()
        return "" if i < 0 else self.strings[i]

    def value(self, t=None):
        if t is None:
            t, _ = self.type()
        if t == T_NULL:
            return None
        if t == T_BOOL:
            return bool(self.u8())
        if t == T_TRUE:
            return True
        if t == T_FALSE:
            return False
        if t in (T_INT64_0, T_INT64_1):
            return t - T_INT64_0
        if t in (T_DOUBLE_0, T_DOUBLE_1):
            return float(t - T_DOUBLE_0)
        if t == T_INT64:
            return self.f64("<q")
        if t == T_UINT64:
            return self.f64("<Q")
        if t == T_DOUBLE:
            return self.f64()
        if t in (T_INT32, T_UINT32):
            return self.i32()
        if t == T_INT32_AS_BYTE:
            return self.u8()
        if t == T_FLOAT:
            return self.f32()
        if t == T_STRING:
            return self.string()
        if t == T_BLOB:  # the data is in the blob blocks after the buffers
            return None
        if t == T_OBJECT:
            n = self.count()
            out = {}
            for _ in range(n):
                key = self.string()
                out[key] = self.value()
            return out
        if t == T_ARRAY:
            return [self.value() for _ in range(self.i32())]
        if t == T_ARRAY_TYPED:
            n = self.i32()
            sub, _ = self.type()
            return [self.value(sub) for _ in range(n)]
        if t == T_ARRAY_BYTE_LEN:
            n = self.u8()
            sub, _ = self.type()
            return [self.value(sub) for _ in range(n)]
        if t == T_ARRAY_AUX:  # v5: count here, the values in buffer 1
            n = self.u8()
            sub, _ = self.type()
            return [self.aux.value(sub) for _ in range(n)]
        raise ValueError(f"KV3 type {t} isn't supported (types offset {self.pt - 1})")

    def check_done(self):
        readers = [("", self)] + ([("aux ", self.aux)] if self.aux else [])
        left = [(prefix + name, len(buf) - pos) for prefix, r in readers for name, buf, pos in
                (("bytes", r.b1, r.p1), ("ints", r.b4, r.ints_pos), ("doubles", r.b8, r.p8),
                 ("types", r.types, r.pt)) if len(buf) != pos]
        if left:
            raise ValueError(f"KV3 decode left data: {left}")


class _NodeReader(_Reader):
    def raw_type(self):
        t = self.types[self.pt]
        self.pt += 1
        flag = None
        if t & 0x80:
            flag = self.types[self.pt]
            self.pt += 1
        return t & 0x7F, flag

    def node(self, t=None, flag=None):
        if t is None:
            t, flag = self.raw_type()
        if t in SCALAR_TYPES:
            return Node(t, self.value(t), flag)
        if t == T_STRING:
            i = self.i32()
            return Node(t, "" if i < 0 else self.strings[i], flag, idx=i)
        if t == T_OBJECT:
            items = []
            for _ in range(self.count()):
                i = self.i32()
                items.append(("" if i < 0 else self.strings[i], i, self.node()))
            return Node(t, items, flag)
        if t == T_ARRAY:
            return Node(t, [self.node() for _ in range(self.i32())], flag)
        if t in (T_ARRAY_TYPED, T_ARRAY_BYTE_LEN):
            n = self.i32() if t == T_ARRAY_TYPED else self.u8()
            sub = self.raw_type()
            return Node(t, [self.node(*sub) for _ in range(n)], flag, sub=sub)
        raise ValueError(f"KV3 type {t} can't be written back")


class _Writer:
    """The inverse of _NodeReader for KV3 v4: values go to the bytes / ints / doubles streams and
    the types stream in the order they are read."""

    def __init__(self, strings):
        self.b1, self.b4, self.b8, self.types = bytearray(), bytearray(4), bytearray(), bytearray()
        self.strings = list(strings)
        self.index = {}
        for i, s in enumerate(self.strings):
            self.index.setdefault(s, i)

    def type(self, t, flag):
        self.types += bytes([t]) if flag is None else bytes([t | 0x80, flag])

    def string(self, s, idx):
        if idx is not None and ((idx < 0 and s == "") or (0 <= idx < len(self.strings) and self.strings[idx] == s)):
            return idx
        if s not in self.index:
            self.index[s] = len(self.strings)
            self.strings.append(s)
        return self.index[s]

    def node(self, n, typed=True):
        t, v = n.t, n.v
        if typed:
            self.type(t, n.flag)
        if t in (T_NULL, T_TRUE, T_FALSE, T_INT64_0, T_INT64_1, T_DOUBLE_0, T_DOUBLE_1):
            return
        if t in (T_BOOL, T_INT32_AS_BYTE):
            self.b1.append(int(v))
        elif t in (T_INT64, T_UINT64, T_DOUBLE):
            self.b8 += struct.pack({T_INT64: "<q", T_UINT64: "<Q", T_DOUBLE: "<d"}[t], v)
        elif t in (T_INT32, T_UINT32):
            self.b4 += struct.pack("<i", v)
        elif t == T_FLOAT:
            self.b4 += struct.pack("<f", v)
        elif t == T_STRING:
            self.b4 += struct.pack("<i", self.string(v, n.idx))
        elif t == T_OBJECT:
            self.b4 += struct.pack("<i", len(v))
            for key, idx, child in v:
                self.b4 += struct.pack("<i", self.string(key, idx))
                self.node(child)
        elif t == T_ARRAY:
            self.b4 += struct.pack("<i", len(v))
            for child in v:
                self.node(child)
        elif t in (T_ARRAY_TYPED, T_ARRAY_BYTE_LEN):
            if t == T_ARRAY_TYPED:
                self.b4 += struct.pack("<i", len(v))
            else:
                self.b1.append(len(v))
            self.type(*n.sub)
            for child in v:
                self.node(child, typed=False)
        else:
            raise ValueError(f"KV3 type {t} can't be written")


# ---------------------------------------------------------------------------- mesh

# meshoptimizer's vertex codec (header 0xa0 | version): blocks of up to 256 vertices, every byte
# column packed in groups of 16 deltas; version 1 adds a control byte per 4 columns and a delta mode
# per 4 bytes (bytes, 16-bit words, or 32-bit xor with a rotation)
_MESHOPT_BITS_V0 = (0, 2, 4, 8)
_MESHOPT_BITS_V1 = (0, 1, 2, 4, 8)


def _meshopt_group(data, pos, out, at, bits):
    """One group of 16 bytes at `bits` per value; values of all ones are escapes to a full byte."""
    if bits == 0:
        return pos
    if bits == 8:
        out[at:at + 16] = data[pos:pos + 16]
        return pos + 16
    count = 16 * bits // 8
    escape, per = (1 << bits) - 1, 8 // bits
    var = pos + count
    for b in range(count):
        byte = data[pos + b]
        for k in range(per):
            # high bits first, except 1-bit values (version 1 only): low bit first
            enc = (byte >> (k if bits == 1 else 8 - bits * (k + 1))) & escape
            if enc == escape:
                enc = data[var]
                var += 1
            out[at + b * per + k] = enc
    return var


def _meshopt_bytes(data, pos, size, bits):
    out = bytearray(size)
    header, pos = pos, pos + (size // 16 + 3) // 4
    for g in range(size // 16):
        pos = _meshopt_group(data, pos, out, g * 16, bits[(data[header + g // 4] >> (g % 4 * 2)) & 3])
    return pos, out


def _unzigzag(v):
    return (v >> 1) ^ -(v & 1)


def meshopt_decode_vertices(data, count, size):
    """meshopt_decodeVertexBuffer: `count` vertices of `size` bytes -> (count, size) uint8."""
    if data[0] & 0xF0 != 0xA0 or data[0] & 0x0F > 1:
        raise ValueError(f"not a meshopt vertex buffer (header {data[0]:#x})")
    version = data[0] & 0x0F
    tail_size = size + (size // 4 if version else 0)
    tail_pad = max(tail_size, 24 if version else 32)
    tail = len(data) - tail_size
    last = np.frombuffer(data, np.uint8, size, tail).astype(np.int64)
    channels = data[tail + size:tail + tail_size]
    block = min((8192 // size) & ~15, 256)
    pos, done, parts = 1, 0, []
    while done < count:
        n = min(block, count - done)
        aligned = (n + 15) & ~15
        control = pos
        pos += size // 4 if version else 0
        out = np.zeros((n, size), np.int64)
        for k in range(0, size, 4):
            ctrl_byte = data[control + k // 4] if version else 0
            cols = []
            for j in range(4):
                ctrl = (ctrl_byte >> (j * 2)) & 3
                if ctrl == 3:
                    cols.append(np.frombuffer(data, np.uint8, n, pos).astype(np.int64))
                    pos += n
                elif ctrl == 2:
                    cols.append(np.zeros(n, np.int64))
                else:
                    pos, buf = _meshopt_bytes(data, pos, aligned, _MESHOPT_BITS_V1[ctrl:] if version else _MESHOPT_BITS_V0)
                    cols.append(np.frombuffer(bytes(buf[:n]), np.uint8).astype(np.int64))
            mode = channels[k // 4] if version else 0
            if mode & 3 == 0:  # byte deltas
                for j in range(4):
                    out[:, k + j] = (last[k + j] + np.cumsum(_unzigzag(cols[j]))) & 0xFF
            elif mode & 3 == 1:  # 16-bit deltas
                for j in (0, 2):
                    d = _unzigzag(cols[j] | cols[j + 1] << 8)
                    v = (int(last[k + j] | last[k + j + 1] << 8) + np.cumsum(d)) & 0xFFFF
                    out[:, k + j], out[:, k + j + 1] = v & 0xFF, v >> 8
            elif mode & 3 == 2:  # 32-bit xor, rotated left
                rot = (32 - (mode >> 4)) & 31
                d = (cols[0] | cols[1] << 8 | cols[2] << 16 | cols[3] << 24) & 0xFFFFFFFF
                d = ((d << rot) | (d >> ((32 - rot) & 31))) & 0xFFFFFFFF
                p = int(last[k] | last[k + 1] << 8 | last[k + 2] << 16 | last[k + 3] << 24)
                v = np.bitwise_xor.accumulate(d) ^ p
                for j in range(4):
                    out[:, k + j] = (v >> (8 * j)) & 0xFF
            else:
                raise ValueError("meshopt: bad channel mode")
        last = out[-1].copy()
        parts.append(out.astype(np.uint8))
        done += n
    if pos != len(data) - tail_pad:
        raise ValueError("meshopt vertex buffer didn't end where expected")
    return np.concatenate(parts)


def meshopt_decode_indices(data, count):
    """meshopt_decodeIndexBuffer (triangles, header 0xe0 | version) -> (count // 3, 3) int array."""
    if data[0] & 0xF0 != 0xE0 or data[0] & 0x0F > 1:
        raise ValueError(f"not a meshopt index buffer (header {data[0]:#x})")
    fecmax = 13 if data[0] & 0x0F else 15
    edges = [(0xFFFFFFFF, 0xFFFFFFFF)] * 16
    verts = [0xFFFFFFFF] * 16
    eo = vo = 0
    nxt = last = 0
    code = 1
    pos = 1 + count // 3
    aux = len(data) - 16
    out = []

    def vbyte():
        nonlocal pos
        result, shift = 0, 0
        for _ in range(5):
            b = data[pos]
            pos += 1
            result |= (b & 127) << shift
            shift += 7
            if b < 128:
                break
        return result

    def index():
        nonlocal last
        last = (last + _unzigzag(vbyte())) & 0xFFFFFFFF
        return last

    for _ in range(count // 3):
        tri = data[code]
        code += 1
        if tri < 0xF0:
            a, b = edges[(eo - 1 - (tri >> 4)) & 15]
            fec = tri & 15
            if fec < fecmax:
                c = nxt if fec == 0 else verts[(vo - 1 - fec) & 15]
                if fec == 0:
                    nxt += 1
                verts[vo] = c
                vo = (vo + (fec == 0)) & 15
            else:
                c = last = (last + (fec - (fec ^ 3))) & 0xFFFFFFFF if fec != 15 else index()
                verts[vo] = c
                vo = (vo + 1) & 15
            edges[eo] = (c, b)
            eo = (eo + 1) & 15
            edges[eo] = (a, c)
            eo = (eo + 1) & 15
        else:
            if tri < 0xFE:
                codeaux = data[aux + (tri & 15)]
                feb, fec = codeaux >> 4, codeaux & 15
                a = nxt
                nxt += 1
                b = nxt if feb == 0 else verts[(vo - feb) & 15]
                nxt += feb == 0
                c = nxt if fec == 0 else verts[(vo - fec) & 15]
                nxt += fec == 0
                push_b, push_c = feb == 0, fec == 0
            else:
                codeaux = data[pos]
                pos += 1
                fea = 0 if tri == 0xFE else 15
                feb, fec = codeaux >> 4, codeaux & 15
                if codeaux == 0:
                    nxt = 0
                a = 0
                if fea == 0:
                    a = nxt
                    nxt += 1
                if feb == 0:
                    b = nxt
                    nxt += 1
                else:
                    b = verts[(vo - feb) & 15]
                if fec == 0:
                    c = nxt
                    nxt += 1
                else:
                    c = verts[(vo - fec) & 15]
                if fea == 15:
                    a = index()
                if feb == 15:
                    b = index()
                if fec == 15:
                    c = index()
                push_b, push_c = feb in (0, 15), fec in (0, 15)
            verts[vo] = a
            vo = (vo + 1) & 15
            verts[vo] = b
            vo = (vo + push_b) & 15
            verts[vo] = c
            vo = (vo + push_c) & 15
            edges[eo] = (b, a)
            eo = (eo + 1) & 15
            edges[eo] = (c, b)
            eo = (eo + 1) & 15
            edges[eo] = (a, c)
            eo = (eo + 1) & 15
        out.append((a, b, c))
    if pos != aux:
        raise ValueError("meshopt index buffer didn't end where expected")
    return np.array(out, np.int64).reshape(-1, 3)


def unpack_normals(packed):
    """CompressedTangentFrame (R32_UINT): 10+10 bits of an octahedral normal, the tangent angle and its
    sign in the rest. Returns the unit normals."""
    packed = packed.astype(np.uint32)
    x = ((packed >> 12) & 0x3FF) / 1023 * 2 - 1
    y = ((packed >> 22) & 0x3FF) / 1023 * 2 - 1
    z = 1 - np.abs(x) - np.abs(y)
    lower = z < 0
    sx, sy = np.where(x >= 0, 1.0, -1.0), np.where(y >= 0, 1.0, -1.0)
    x, y = np.where(lower, (1 - np.abs(y)) * sx, x), np.where(lower, (1 - np.abs(x)) * sy, y)
    n = np.stack([x, y, z], -1)
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-12)


# vertex formats (DXGI) the weapon models use, as (numpy type, components, scale)
_VERTEX_FORMATS = {2: ("<f4", 4, 1), 6: ("<f4", 3, 1), 16: ("<f4", 2, 1), 10: ("<f2", 4, 1), 34: ("<f2", 2, 1),
                   35: ("<u2", 2, 65535), 37: ("<i2", 2, 32767), 28: ("u1", 4, 255), 42: ("<u4", 1, 0)}


class Model:
    """The render meshes of a .vmdl_c: a list of draw calls, each a dict with the material, positions,
    normals, uv (TEXCOORD 0) and triangles (indices into those)."""

    def __init__(self, blob):
        res = Resource.parse(blob)
        ctrl = KV3(res.block("CTRL")).decode()
        self.draws = []
        for mesh in ctrl.get("embedded_meshes", []):
            data = KV3(res.blocks[mesh["m_nDataBlock"]][1]).decode()
            vbs = [self._vertices(res, b) for b in mesh["m_vertexBuffers"]]
            ibs = [self._indices(res, b) for b in mesh["m_indexBuffers"]]
            for obj in data.get("m_sceneObjects", []):
                for dc in obj.get("m_drawCalls", []):
                    if dc.get("m_nPrimitiveType") != "RENDER_PRIM_TRIANGLES":
                        continue
                    attrs = vbs[dc["m_vertexBuffers"][0]["m_hBuffer"]]
                    ib = ibs[dc["m_indexBuffer"]["m_hBuffer"]]
                    start = dc["m_nStartIndex"] // 3
                    tris = ib[start:start + dc["m_nIndexCount"] // 3] + dc["m_nBaseVertex"]
                    self.draws.append({"mesh": mesh.get("m_Name", ""), "material": dc["m_material"],
                                       "position": attrs["POSITION0"], "normal": attrs.get("NORMAL0"),
                                       "uv": attrs.get("TEXCOORD0"), "triangles": tris})

    @staticmethod
    def _vertices(res, desc):
        raw = res.blocks[desc["m_nBlockIndex"]][1]
        count, size = desc["m_nElementCount"], desc["m_nElementSizeInBytes"]
        if desc.get("m_bCompressedZSTD"):
            raise ValueError("zstd vertex buffers aren't supported")
        if desc.get("m_bMeshoptCompressed"):
            rows = meshopt_decode_vertices(raw, count, size)
        else:
            rows = np.frombuffer(raw, np.uint8, count * size).reshape(count, size)
        out = {}
        for field in desc["m_inputLayoutFields"]:
            name = f'{field["m_pSemanticName"]}{field["m_nSemanticIndex"]}'
            fmt = _VERTEX_FORMATS.get(field["m_Format"])
            if fmt is None or name in out:
                continue
            dtype, n, scale = fmt
            width = np.dtype(dtype).itemsize * n
            cols = np.ascontiguousarray(rows[:, field["m_nOffset"]:field["m_nOffset"] + width])
            values = cols.view(dtype).reshape(count, n)
            if field["m_Format"] == 42:
                out[name] = unpack_normals(values[:, 0])
            elif scale != 1:
                out[name] = values.astype(np.float64) / scale
            else:
                out[name] = values.astype(np.float64)
        return out

    @staticmethod
    def _indices(res, desc):
        raw = res.blocks[desc["m_nBlockIndex"]][1]
        count, size = desc["m_nElementCount"], desc["m_nElementSizeInBytes"]
        if desc.get("m_bMeshoptIndexSequence") or desc.get("m_bCompressedZSTD"):
            raise ValueError("this index buffer encoding isn't supported")
        if desc.get("m_bMeshoptCompressed"):
            return meshopt_decode_indices(raw, count)
        return np.frombuffer(raw, "<u2" if size == 2 else "<u4", count).astype(np.int64).reshape(-1, 3)


# ---------------------------------------------------------------------------- vtex

FMT_DXT1, FMT_DXT5, FMT_RGBA8888, FMT_RGBA16F, FMT_BC7, FMT_ATI2N, FMT_ATI1N, FMT_BGRA8888 = 1, 2, 4, 10, 20, 21, 27, 28
VTEX_NO_LOD = 8
EXTRA_FALLBACK, EXTRA_COMPRESSED_MIPS = 1, 4
BLOCK_BYTES = {FMT_DXT1: 8, FMT_DXT5: 16, FMT_BC7: 16, FMT_ATI1N: 8, FMT_ATI2N: 16}
PIXEL_BYTES = {FMT_RGBA8888: 4, FMT_BGRA8888: 4, FMT_RGBA16F: 8}
PILLOW_BCN = {FMT_DXT1: 1, FMT_DXT5: 3, FMT_ATI1N: 4, FMT_ATI2N: 5, FMT_BC7: 7}


def mip_size(fmt, w, h):
    if fmt in BLOCK_BYTES:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK_BYTES[fmt]
    return w * h * PIXEL_BYTES[fmt]


class Texture:
    """Header fields of a .vtex_c and access to its mips."""

    def __init__(self, blob):
        self.res = Resource.parse(blob)
        d = self.res.block("DATA")
        self.version, self.flags = struct.unpack_from("<HH", d, 0)
        self.reflectivity = struct.unpack_from("<4f", d, 4)
        self.width, self.height, self.depth = struct.unpack_from("<HHH", d, 20)
        self.format, self.mips = d[26], d[27]
        extra_offset, extra_count = struct.unpack_from("<II", d, 32)
        self.extra = {}
        for i in range(extra_count):
            pos = 32 + extra_offset + 12 * i
            kind, offset, size = struct.unpack_from("<III", d, pos)
            self.extra[kind] = d[pos + 4 + offset:pos + 4 + offset + size]
        self.compressed = None
        if EXTRA_COMPRESSED_MIPS in self.extra:
            # the sizes array sits right after the 12 byte record (the record's size doesn't count it)
            pos = next(32 + extra_offset + 12 * i for i in range(extra_count)
                       if struct.unpack_from("<I", d, 32 + extra_offset + 12 * i)[0] == EXTRA_COMPRESSED_MIPS)
            kind, offset, size = struct.unpack_from("<III", d, pos)
            body = pos + 4 + offset
            flag, sizes_offset, count = struct.unpack_from("<III", d, body)
            sizes = struct.unpack_from(f"<{count}I", d, body + 4 + sizes_offset)
            self.compressed = (bool(flag), sizes)
        self.pixels = self.res.tail

    def mip(self, level):
        """Raw (decompressed) bytes of mip `level` (0 = largest), and its size."""
        pos = 0
        for lv in range(self.mips - 1, level - 1, -1):
            w, h = max(1, self.width >> lv), max(1, self.height >> lv)
            raw = mip_size(self.format, w, h)
            stored = self.compressed[1][lv] if self.compressed else raw  # sizes are listed largest mip first
            if lv == level:
                data = self.pixels[pos:pos + stored]
                if self.compressed and self.compressed[0] and stored != raw:
                    data = lz4_decompress(data, raw)
                return data, w, h
            pos += stored
        raise ValueError("no such mip")

    def image(self, level=0):
        """Mip `level` as an RGBA (or single channel for ATI1N) numpy array."""
        data, w, h = self.mip(level)
        if self.format == FMT_BGRA8888:
            return np.frombuffer(data, np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]].copy()
        if self.format == FMT_RGBA8888:
            return np.frombuffer(data, np.uint8).reshape(h, w, 4).copy()
        if self.format == FMT_ATI1N:
            return decode_bc4(data, w, h)
        if self.format in (FMT_DXT1, FMT_DXT5):
            return decode_dxt(data, w, h, self.format == FMT_DXT5)
        if self.format == FMT_BC7:
            # Pillow's own BCn decoder (the one its DDS reader uses); blocks are 4x4, a smaller mip is padded
            from PIL import Image
            bw, bh = max(4, (w + 3) // 4 * 4), max(4, (h + 3) // 4 * 4)
            img = np.asarray(Image.frombytes("RGBA", (bw, bh), bytes(data), "bcn", 7))
            return img[:h, :w].copy()
        raise ValueError(f"texture format {self.format} can't be decoded here")


def build_texture(template, fmt, mips, flags=0, fallback=None, red2=None, compress=True):
    """A .vtex_c from mips (largest first, each (bytes, w, h)). With `compress` every mip that gets
    smaller is stored as an LZ4 block (the rest raw, like the game does for its small mips).
    `template` is a game .vtex_c whose RED2 block is reused (it only describes the source file)."""
    w, h = mips[0][1], mips[0][2]
    rgba_mean = fallback[1] if fallback else (0.5, 0.5, 0.5, 1.0)
    extras = []
    if fallback is not None:
        extras.append((EXTRA_FALLBACK, fallback[0]))
    unit = BLOCK_BYTES.get(fmt, PIXEL_BYTES.get(fmt, 4))
    stored = []
    for data, mw, mh in mips:
        packed = lz4_compress_runs(data, unit) if compress else data
        if compress and len(packed) < len(data):
            if lz4_decompress(packed, len(data)) != data:
                raise ValueError("lz4 run compression failed its round trip")
            stored.append(packed)
        else:
            stored.append(data)
    mips = [(s, m[1], m[2]) for s, m in zip(stored, mips)]
    sizes = [len(m[0]) for m in mips]  # largest first, while the data goes smallest first
    flag = 1 if compress else 0
    extras.append((EXTRA_COMPRESSED_MIPS, struct.pack("<III", flag, 8, len(sizes)), struct.pack(f"<{len(sizes)}I", *sizes)))
    d = bytearray(40 + 12 * len(extras))
    struct.pack_into("<HH4fHHHBBI", d, 0, 1, flags, *rgba_mean, w, h, 1, fmt, len(mips), 0)
    struct.pack_into("<II", d, 32, 8, len(extras))
    for i, extra in enumerate(extras):
        pos = 40 + 12 * i
        struct.pack_into("<III", d, pos, extra[0], len(d) - (pos + 4), len(extra[1]))
        d += extra[1]
        if len(extra) > 2:
            d += extra[2]
    blocks = []
    red2 = red2 if red2 is not None else (Resource.parse(template).block("RED2") if template else None)
    if red2 is not None:
        blocks.append(("RED2", red2))
    blocks.append(("DATA", bytes(d)))
    res = Resource(1, blocks, b"".join(m[0] for m in reversed(mips)))
    return res.build()


# ------------------------------------------------------------------- block codecs

def _blocks(img):
    """HxWxC -> (H/4 * W/4, 16, C) in raster block order."""
    h, w, c = img.shape
    return img.reshape(h // 4, 4, w // 4, 4, c).swapaxes(1, 2).reshape(-1, 16, c)


def _unblocks(blocks, w, h):
    c = blocks.shape[-1]
    return blocks.reshape(h // 4, w // 4, 4, 4, c).swapaxes(1, 2).reshape(h, w, c)


def _pad4(img):
    h, w = img.shape[:2]
    ph, pw = -h % 4, -w % 4
    if ph or pw:
        img = np.pad(img, ((0, ph), (0, pw), (0, 0)), mode="edge")
    return img


def _to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.uint32)
    return ((c[..., 0] * 31 + 127) // 255 << 11) | ((c[..., 1] * 63 + 127) // 255 << 5) | ((c[..., 2] * 31 + 127) // 255)


def _from565(v):
    v = v.astype(np.uint32)
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def encode_color_blocks(rgb):
    """BC1 color part for (N, 16, 3) float pixels -> (N, 8) uint8, 4-color mode."""
    mean = rgb.mean(1, keepdims=True)
    d = rgb - mean
    cov = np.einsum("nki,nkj->nij", d, d)
    axis = np.ones((rgb.shape[0], 3), np.float32) / np.sqrt(3)
    for _ in range(8):
        axis = np.einsum("nij,nj->ni", cov, axis)
        norm = np.linalg.norm(axis, axis=1, keepdims=True)
        axis = np.where(norm > 1e-6, axis / np.maximum(norm, 1e-12), 1 / np.sqrt(3))
    t = np.einsum("nki,ni->nk", d, axis)
    lo = mean[:, 0] + axis * t.min(1, keepdims=True)
    hi = mean[:, 0] + axis * t.max(1, keepdims=True)
    c0, c1 = _to565(hi), _to565(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = _from565(c0), _from565(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)  # (N, 4, 3)
    dist = ((rgb[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1)  # (N, 16, 4)
    idx = dist.argmin(-1).astype(np.uint32)
    idx[c0 == c1] = 0
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((rgb.shape[0], 8), np.uint8)
    out[:, 0:2] = c0.astype("<u2").view(np.uint8).reshape(-1, 2)
    out[:, 2:4] = c1.astype("<u2").view(np.uint8).reshape(-1, 2)
    out[:, 4:8] = bits.astype("<u4").view(np.uint8).reshape(-1, 4)
    return out


def encode_alpha_blocks(a):
    """BC4 / DXT5 alpha part for (N, 16) float values -> (N, 8) uint8 (8-value mode)."""
    hi = np.clip(np.rint(a.max(1)), 0, 255)
    lo = np.clip(np.rint(a.min(1)), 0, 255)
    same = hi == lo
    hi = np.where(same & (hi < 255), hi + 1, hi)
    lo = np.where(same & (hi == 255) & (lo == 255), 254, lo)
    a0, a1 = hi, lo
    weights = np.array([0, 7, 1, 2, 3, 4, 5, 6], np.float32)  # palette slot -> weight of a1 (in 7ths)
    palette = (a0[:, None] * (7 - weights) + a1[:, None] * weights) / 7  # slots 0..7
    dist = np.abs(a[:, :, None] - palette[:, None, :])
    idx = dist.argmin(-1).astype(np.uint64)
    bits = (idx << (3 * np.arange(16, dtype=np.uint64))).sum(1).astype(np.uint64)
    out = np.zeros((a.shape[0], 8), np.uint8)
    out[:, 0] = a0.astype(np.uint8)
    out[:, 1] = a1.astype(np.uint8)
    out[:, 2:8] = bits.astype("<u8").view(np.uint8).reshape(-1, 8)[:, :6]
    return out


def encode_dxt(img, alpha):
    """RGBA uint8 image -> DXT1 (alpha False) or DXT5 bytes."""
    img = _pad4(img)
    blocks = _blocks(img.astype(np.float32))
    color = encode_color_blocks(blocks[..., :3])
    if not alpha:
        return color.tobytes()
    return np.concatenate([encode_alpha_blocks(blocks[..., 3]), color], 1).tobytes()


def decode_color_blocks(c):
    c0 = c[:, 0:2].copy().view("<u2")[:, 0]
    c1 = c[:, 2:4].copy().view("<u2")[:, 0]
    bits = c[:, 4:8].copy().view("<u4")[:, 0]
    p0, p1 = _from565(c0), _from565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) / 3, (p0 + p1) / 2)
    p3 = np.where(four, (p0 + 2 * p1) / 3, 0)
    palette = np.stack([p0, p1, p2, p3], 1)
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    return np.take_along_axis(palette, idx[..., None].astype(np.int64).repeat(3, -1), 1)


def decode_alpha_blocks(a):
    a0 = a[:, 0].astype(np.float32)
    a1 = a[:, 1].astype(np.float32)
    bits = np.zeros(len(a), np.uint64)
    for i in range(6):
        bits |= a[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    eight = (a0 > a1)[:, None]
    w = np.arange(1, 7, dtype=np.float32)
    mid8 = ((7 - w) * a0[:, None] + w * a1[:, None]) / 7
    w5 = np.arange(1, 5, dtype=np.float32)
    mid6 = ((5 - w5) * a0[:, None] + w5 * a1[:, None]) / 5
    mid6 = np.concatenate([mid6, np.zeros((len(a), 1)), np.full((len(a), 1), 255)], 1)
    palette = np.concatenate([a0[:, None], a1[:, None], np.where(eight, mid8, mid6)], 1)
    idx = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(palette, idx.astype(np.int64), 1)


def decode_dxt(data, w, h, alpha):
    pw, ph = (w + 3) & ~3, (h + 3) & ~3
    n = (pw // 4) * (ph // 4)
    raw = np.frombuffer(data, np.uint8).reshape(n, 16 if alpha else 8)
    rgb = decode_color_blocks(raw[:, 8:] if alpha else raw)
    a = decode_alpha_blocks(raw[:, :8]) if alpha else np.full((n, 16), 255, np.float32)
    px = np.concatenate([rgb, a[..., None]], -1)
    return np.clip(np.rint(_unblocks(px, pw, ph)[:h, :w]), 0, 255).astype(np.uint8)


def decode_bc4(data, w, h):
    pw, ph = (w + 3) & ~3, (h + 3) & ~3
    raw = np.frombuffer(data, np.uint8).reshape(-1, 8)
    v = decode_alpha_blocks(raw)[..., None]
    return np.clip(np.rint(_unblocks(v, pw, ph)[:h, :w, 0]), 0, 255).astype(np.uint8)


def fallback_bits(img):
    """The 32x32 DXT5 preview every game texture carries (extra data 1), plus the mean RGBA in 0..1."""
    from PIL import Image
    small = np.asarray(Image.fromarray(img, "RGBA").resize((32, 32), Image.LANCZOS))
    mean = tuple(float(x) for x in img.reshape(-1, 4).mean(0) / 255)
    return encode_dxt(small, True), mean

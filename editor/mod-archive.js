// A mod archive for the Mods tab: a plain .zip (stored or deflated entries; no zip64, no encryption)
// with mod.txt at its top, or inside the one folder at its top. The page unpacks it and sends the
// files to the GC, which never sees the archive itself.

const decoder = new TextDecoder();

// the GC takes letters, digits, space, _ - . for a mod folder, not starting with _ or .
export function modFolderName(name) {
  const cleaned = name.replace(/\.zip$/i, '').replace(/\.csforge$/i, '').replace(/[^A-Za-z0-9 _.-]+/g, '_').replace(/^[_. ]+|[. ]+$/g, '').slice(0, 64);
  return cleaned || 'mod';
}

export async function openModArchive(file) {
  const buffer = await file.arrayBuffer();
  const view = new DataView(buffer);
  let end = -1;
  for (let at = buffer.byteLength - 22; at >= Math.max(0, buffer.byteLength - 22 - 65535); at--) {
    if (view.getUint32(at, true) === 0x06054b50) { end = at; break; }
  }
  if (end < 0) throw new Error('This is not a zip archive.');
  const count = view.getUint16(end + 10, true);
  let offset = view.getUint32(end + 16, true);
  if (count === 0xffff || offset === 0xffffffff) throw new Error('Zip64 archives (over 4 GB or 65535 files) are not supported.');

  const entries = [];
  for (let i = 0; i < count; i++) {
    if (offset + 46 > buffer.byteLength || view.getUint32(offset, true) !== 0x02014b50) throw new Error('The zip archive is damaged.');
    const flags = view.getUint16(offset + 8, true), method = view.getUint16(offset + 10, true);
    const packed = view.getUint32(offset + 20, true), size = view.getUint32(offset + 24, true);
    const nameLength = view.getUint16(offset + 28, true), extra = view.getUint16(offset + 30, true), comment = view.getUint16(offset + 32, true);
    const local = view.getUint32(offset + 42, true);
    const name = decoder.decode(new Uint8Array(buffer, offset + 46, nameLength)).replaceAll('\\', '/');
    offset += 46 + nameLength + extra + comment;
    if (name.endsWith('/')) continue;
    if (flags & 1) throw new Error('Encrypted archives are not supported.');
    if (method !== 0 && method !== 8) throw new Error(`${name} uses a compression this page cannot unpack.`);
    entries.push({ name, method, packed, size, local });
  }

  // mod.txt at the top, or in the single folder everything is in
  let prefix = '', folder = modFolderName(file.name);
  if (!entries.some(entry => entry.name === 'mod.txt')) {
    const tops = new Set(entries.map(entry => entry.name.split('/')[0]));
    const top = tops.size === 1 ? [...tops][0] : '';
    if (!top || !entries.some(entry => entry.name === `${top}/mod.txt`)) throw new Error('No mod.txt in the archive: this is not a CSForge mod.');
    prefix = top + '/';
    folder = modFolderName(top);
  }

  const read = async entry => {
    if (entry.local + 30 > buffer.byteLength) throw new Error('The zip archive is damaged.');
    const start = entry.local + 30 + view.getUint16(entry.local + 26, true) + view.getUint16(entry.local + 28, true);
    if (start + entry.packed > buffer.byteLength) throw new Error('The zip archive is damaged.');
    const bytes = new Uint8Array(buffer, start, entry.packed);
    if (entry.method === 0) return bytes;
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  };
  const files = entries.map(entry => ({ path: entry.name.slice(prefix.length), size: entry.size, read: () => read(entry) }));
  return { folder, files, totalBytes: files.reduce((sum, entry) => sum + entry.size, 0) };
}

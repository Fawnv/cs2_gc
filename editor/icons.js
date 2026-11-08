export const PLACEHOLDER = './placeholder.svg';

export function wearIndex(value) {
  const number = value === undefined || String(value).trim() === '' ? NaN : Number(value);
  return Number.isFinite(number) ? Math.round(Math.max(0, Math.min(1, number)) * 2) : 0;
}

export class Icons {
  constructor(data) {
    if (data?.formatVersion !== 1) throw new Error('Unsupported icon manifest format.');
    if (!/^[a-f0-9]{64}$/.test(data.catalogSha256)) throw new Error('Invalid icon catalogue fingerprint.');
    for (const table of ['images', 'definitions', 'paint', 'kits']) {
      if (!data[table] || typeof data[table] !== 'object' || Array.isArray(data[table])) throw new Error(`Invalid icon table: ${table}.`);
    }
    // cs2_gc: icons are Steam economy images (see tools/gen_invedit_catalog.py)
    const allowed = ['https://cdn.steamstatic.com/apps/730/icons/', 'https://community.akamai.steamstatic.com/economy/image/',
      'https://community.cloudflare.steamstatic.com/economy/image/', 'https://steamcommunity-a.akamaihd.net/economy/image/',
      'https://raw.githubusercontent.com/ByMykel/counter-strike-image-tracker/'];
    for (const url of Object.values(data.images)) {
      if (typeof url !== 'string' || !allowed.some(prefix => url.startsWith(prefix))) throw new Error('Invalid icon URL.');
    }
    const check = id => { if (id !== null && !Object.hasOwn(data.images, id)) throw new Error('Unknown icon image reference.'); };
    for (const definition of Object.values(data.definitions)) {
      if (!definition || typeof definition.kind !== 'string') throw new Error('Invalid icon definition.');
      check(definition.image);
    }
    for (const slots of Object.values(data.paint)) {
      if (!Array.isArray(slots) || slots.length !== 3) throw new Error('Invalid icon wear slots.');
      slots.forEach(check);
    }
    Object.values(data.kits).forEach(check);
    this.data = data;
  }

  assertCatalog(catalog) {
    if (catalog.sha256 !== this.data.catalogSha256) throw new Error('Icon manifest and catalogue snapshots differ. Regenerate icons.');
  }

  candidates(item) {
    const { images, definitions, paint, kits } = this.data;
    const definition = definitions[item?.defIndex];
    const attrs = new Map((item?.attributes ?? []).map(attr => [attr.defIndex, attr.value]));
    const id = value => {
      if (value === undefined || String(value).trim() === '') return 0;
      const number = Number(value);
      return Number.isInteger(number) && number > 0 && number <= 0xffffffff ? number : 0;
    };
    const choices = [];
    const kitKind = { stickers: 'sticker', graffiti: 'graffiti', patches: 'patch', music: 'music', keychains: 'keychain' }[definition?.kind];
    const kitAttribute = { music: 166, keychain: 299 }[kitKind] ?? 113;
    if (kitKind) choices.push(kits[`${kitKind}:${id(attrs.get(kitAttribute))}`]);
    else if (['weapons', 'knives', 'gloves'].includes(definition?.kind)) {
      const slots = paint[`${item.defIndex}:${id(attrs.get(6))}`];
      if (slots) choices.push(slots[wearIndex(attrs.get(8))], slots[0]);
    }
    choices.push(definition?.image);
    return [...new Set([...choices.filter(Boolean).map(key => images[key]), PLACEHOLDER])];
  }

  recordCandidates(record) {
    if (record.template) return this.candidates(record.template);
    const attributes = [];
    if (record.kind === 'paint') attributes.push({ defIndex: 6, value: String(record.kitId) }, { defIndex: 8, value: String(record.wearMin ?? 0) });
    if (['sticker', 'graffiti', 'patch', 'music', 'keychain'].includes(record.kind)) attributes.push({ defIndex: { music: 166, keychain: 299 }[record.kind] ?? 113, value: String(record.kitId) });
    return this.candidates({ defIndex: record.defIndex, attributes });
  }
}

export async function loadIcons(fetcher = globalThis.fetch) {
  const response = await fetcher('./icons.json');
  if (!response.ok) throw new Error(`Icon request failed (HTTP ${response.status}).`);
  return new Icons(await response.json());
}

// One state per element: unchanged renders preserve successful or failed attempts.
export function setThumbnail(image, candidates = [PLACEHOLDER]) {
  const key = JSON.stringify(candidates);
  if (image.dataset.iconCandidates === key) return;
  image.dataset.iconCandidates = key;
  let index = 0;
  image.onerror = () => {
    if (++index < candidates.length) image.src = candidates[index];
    else image.onerror = null;
  };
  image.src = candidates[0];
}

export function createThumbnail() {
  const image = document.createElement('img');
  image.className = 'thumbnail';
  image.width = 80;
  image.height = 60;
  image.alt = '';
  image.loading = 'lazy';
  image.decoding = 'async';
  return image;
}

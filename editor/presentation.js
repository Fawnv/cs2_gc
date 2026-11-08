import { normalizeSearch } from './catalog.js';

export const inventoryOrder = (a, b) => Boolean(b.inventory & 0x40000000) - Boolean(a.inventory & 0x40000000) || b.highId - a.highId;
export function exterior(value) {
  if (value === undefined || String(value).trim() === '') return '';
  const wear = Number(value);
  if (!Number.isFinite(wear)) return '';
  return wear < .07 ? 'Factory New' : wear < .15 ? 'Minimal Wear' : wear < .38 ? 'Field-Tested' : wear < .45 ? 'Well-Worn' : 'Battle-Scarred';
}

export class Presentation {
  constructor(catalog) {
    this.catalog = catalog;
    const data = catalog?.data ?? {};
    this.paint = new Map((data.paintKits ?? []).map(k => [k.id, k]));
    this.kits = new Map((data.stickerKits ?? []).map(k => [`${k.kind}:${k.id}`, k]));
    this.music = new Map((data.musicDefinitions ?? []).map(k => [k.id, k]));
    this.colors = new Map(Object.values(data.rarities ?? {}).map(r => [Number(r.value), data.colors?.[r.color]?.hex_color]));
  }

  item(item, record = null) {
    const definition = this.catalog?.definitions.get(item.defIndex);
    const attrs = new Map((item.attributes ?? []).map(a => [a.defIndex, a.value]));
    const category = record?.category ?? definition?.category ?? 'unknown';
    const paint = ['weapons', 'knives', 'gloves'].includes(category) ? this.paint.get(Number(attrs.get(6))) : null;
    const kind = { stickers: 'sticker', graffiti: 'graffiti', patches: 'patch', keychains: 'keychain' }[category];
    const kit = kind ? this.kits.get(`${kind}:${Number(attrs.get(kind === 'keychain' ? 299 : 113))}`) : category === 'music' ? this.music.get(Number(attrs.get(166))) : null;
    const base = definition?.name || definition?.internalName || `Definition ${item.defIndex}`;
    const name = record?.name || (paint && paint.id ? `${base} | ${paint.name}` : kit?.name ? `${base} | ${kit.name}` : base);
    const [title, ...rest] = name.split(' | ');
    const badges = [];
    if (Number(attrs.get(6)) > 0 && ['weapons', 'knives', 'gloves'].includes(category)) {
      const wear = exterior(attrs.get(8));
      if (wear) badges.push(wear);
    }
    if (attrs.has(80)) badges.push('StatTrak');
    if (!record && (item.inventory & (1 << 30))) badges.push('Unacknowledged');
    if (record && !record.template) badges.push('Unavailable');
    const customName = attrs.get(111) || '';
    const named = !record && customName !== '';
    return {
      name: named ? customName : name, title: named ? customName : title,
      subtitle: named ? '' : rest.join(' | ') || category, category, badges,
      color: this.colors.get(item.rarity ?? record?.rarity) || '#888888',
      search: normalizeSearch([name, customName, definition?.internalName ?? '', item.highId ?? '', item.defIndex, paint?.id ?? kit?.id ?? '', ...[6, 113, 166, 299].map(id => attrs.get(id) ?? '')].join(' ')),
    };
  }
}

export function filterInventory(items, presentation, { query = '', category = '' } = {}) {
  const tokens = normalizeSearch(query).split(' ').filter(Boolean);
  return [...items].filter(item => {
    const info = presentation.item(item);
    return (!category || info.category === category) && tokens.every(token => info.search.includes(token));
  }).sort(inventoryOrder);
}

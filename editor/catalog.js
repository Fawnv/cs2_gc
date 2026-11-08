export const PAGE_SIZE = 50;

// The exporter uses casefold and collapses whitespace, underscores, pipes and #.
// These folds cover the localized English catalogue and common query variants.
export function normalizeSearch(text) {
  return String(text).toLowerCase().replace(/ß/g, 'ss').replace(/ς/g, 'σ')
    .replace(/[\s_|#]+/gu, ' ').trim();
}

export class Catalog {
  constructor(data) {
    if (data?.formatVersion !== 1) throw new Error(`Unsupported catalogue format: ${data?.formatVersion}. Expected 1.`);
    for (const name of ['definitions', 'variants', 'attributes']) {
      if (!Array.isArray(data[name])) throw new Error(`Invalid catalogue: missing ${name}.`);
    }
    this.data = data;
    this.records = new Map();
    this.definitions = new Map();
    this.attributes = new Map();
    this.searchTexts = new Map();
    for (const record of [...data.definitions, ...data.variants]) {
      if (!record || typeof record.key !== 'string' || this.records.has(record.key)) throw new Error('Invalid catalogue record key.');
      this.records.set(record.key, record);
      this.searchTexts.set(record.key, normalizeSearch([record.searchText ?? '', ...(record.aliases ?? []), record.defIndex, record.kitId ?? ''].join(' ')));
    }
    for (const record of data.definitions) this.definitions.set(record.defIndex, record);
    for (const record of data.attributes) this.attributes.set(record.id, record);
  }

  search({ view = 'definitions', category = '', query = '', allDefinitions = false } = {}) {
    const tokens = normalizeSearch(query).split(' ').filter(Boolean);
    return this.data[view].filter(record =>
      (view !== 'definitions' || allDefinitions || !record.advancedOnly) &&
      (!category || record.category === category) &&
      tokens.every(token => this.searchTexts.get(record.key).includes(token)));
  }

  page(options, page = 0) {
    const matches = this.search(options);
    const pages = Math.max(1, Math.ceil(matches.length / PAGE_SIZE));
    page = Math.max(0, Math.min(page, pages - 1));
    return { records: matches.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE), total: matches.length, page, pages };
  }

  template(key, statTrak = false) {
    const record = this.records.get(key);
    if (!record) throw new Error('Select a catalogue record.');
    const template = statTrak ? record.statTrakTemplate : record.template;
    if (!template) throw new Error(record.unavailableReason || 'This template is unavailable.');
    return structuredClone(template);
  }
}

export async function loadCatalog(fetcher = globalThis.fetch) {
  const response = await fetcher('./catalog.json');
  if (!response.ok) throw new Error(`Catalogue request failed (HTTP ${response.status}).`);
  const text = await response.text();
  const catalog = new Catalog(JSON.parse(text));
  const digest = await globalThis.crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  catalog.sha256 = [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('');
  return catalog;
}

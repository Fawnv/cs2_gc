// Shared page-session preferences. Catalogue templates remain immutable.
export class CreationPreferences {
  constructor() {
    this.wear = null;
    this.seed = '0';
    this.statTrak = false;
    this.counter = '0';
  }

  commitWear(text, min, max) {
    const value = Number(text);
    const fallback = this.wear === null ? min : Number(this.wear);
    this.wear = String(Math.max(min, Math.min(max,
      String(text).trim() !== '' && Number.isFinite(value) ? value : fallback)));
    return this.wear;
  }

  build(catalog, key) {
    const record = catalog.records.get(key);
    const item = catalog.template(key, Boolean(record?.statTrakTemplate && this.statTrak));
    const attrs = new Map(item.attributes.map(a => [a.defIndex, a]));
    const set = (id, value) => {
      if (attrs.has(id)) attrs.get(id).value = String(value);
      else item.attributes.push({ defIndex: id, value: String(value) });
    };
    const numeric = (text, label, min, max, integer = false) => {
      const value = Number(text);
      if (String(text).trim() === '' || !Number.isFinite(value) || value < min || value > max || (integer && !Number.isInteger(value))) {
        throw new Error(`${label} must be ${integer ? 'an integer' : 'a number'} between ${min} and ${max}.`);
      }
      return value;
    };
    const paint = catalog.data.paintKits?.find(k => k.id === Number(attrs.get(6)?.value));
    const painted = attrs.has(7) && attrs.has(8);
    const min = record.wearMin ?? paint?.wearMin;
    const max = record.wearMax ?? paint?.wearMax;
    let clamped = false, wear = null;
    if (painted) {
      if (!Number.isFinite(min) || !Number.isFinite(max)) throw new Error('Paint wear limits are unavailable.');
      const preferred = this.wear === null ? min : Number(this.wear);
      if (!Number.isFinite(preferred) || (this.wear !== null && String(this.wear).trim() === '')) throw new Error('Invalid paint wear.');
      wear = Math.max(min, Math.min(max, preferred));
      clamped = wear !== preferred;
      set(8, wear);
      set(7, numeric(this.seed, 'Paint seed', 0, 1000, true));
    }
    const statTrak = attrs.has(80);
    if (statTrak) set(80, numeric(this.counter, 'StatTrak counter', 0, 0xffffffff, true));
    return { item, painted, min, max, wear, clamped, statTrak };
  }
}

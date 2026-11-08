export const ROW_HEIGHT = 232;
export function gridRange(count, columns, scrollTop, height, overscan = 3) {
  const rows = Math.ceil(count / columns);
  const first = Math.min(rows, Math.max(0, Math.floor(scrollTop / ROW_HEIGHT) - overscan));
  const last = Math.min(rows, Math.ceil((scrollTop + height) / ROW_HEIGHT) + overscan);
  return { start: first * columns, end: Math.min(count, last * columns), totalHeight: rows * ROW_HEIGHT };
}

// Selection belongs to the application; this controller owns only mounted cards.
export class VirtualGrid {
  constructor(viewport, { key, create, update, select }) {
    Object.assign(this, { viewport, key, create, update, select });
    this.records = [];
    this.indices = new Map();
    this.nodes = new Map();
    this.columns = 1;
    this.focusKey = null;
    this.canvas = document.createElement('div');
    this.canvas.className = 'grid-canvas';
    viewport.append(this.canvas);
    viewport.addEventListener('scroll', () => this.schedule());
    viewport.addEventListener('keydown', event => this.navigate(event));
    viewport.addEventListener('focusout', () => this.schedule());
    this.observer = new ResizeObserver(() => {
      if (!viewport.clientWidth) return;
      const anchor = this.anchor();
      this.columns = Math.max(1, Math.floor((viewport.clientWidth + 12) / 172));
      this.updateHeight();
      this.restore(anchor);
      this.schedule();
    });
    this.observer.observe(viewport);
  }
  anchor() {
    const index = Math.floor(this.viewport.scrollTop / ROW_HEIGHT) * this.columns;
    return { key: this.records[index] && this.key(this.records[index]), offset: this.viewport.scrollTop % ROW_HEIGHT };
  }
  restore(anchor) {
    const index = this.indices.get(anchor.key);
    const max = Math.max(0, Math.ceil(this.records.length / this.columns) * ROW_HEIGHT - this.viewport.clientHeight);
    this.viewport.scrollTop = Math.min(max, index === undefined ? this.viewport.scrollTop : Math.floor(index / this.columns) * ROW_HEIGHT + anchor.offset);
  }
  updateHeight() {
    this.canvas.style.height = `${Math.ceil(this.records.length / this.columns) * ROW_HEIGHT}px`;
  }
  setRecords(records, reset = false) {
    const anchor = this.anchor();
    this.records = records;
    this.indices = new Map(records.map((record, index) => [this.key(record), index]));
    this.updateHeight();
    if (reset) this.viewport.scrollTop = 0;
    else this.restore(anchor);
    this.schedule();
  }
  schedule() {
    if (this.frame) return;
    this.frame = requestAnimationFrame(() => { this.frame = null; this.render(); });
  }
  render() {
    for (const [key, node] of this.nodes) if (!this.indices.has(key)) {
      node.parentElement.remove(); this.nodes.delete(key);
    }
    if (!this.viewport.clientWidth) return;
    const range = gridRange(this.records.length, this.columns, this.viewport.scrollTop, this.viewport.clientHeight);
    this.canvas.style.height = `${range.totalHeight}px`;
    const visible = new Set();
    for (let i = range.start; i < range.end; i++) visible.add(i);
    const active = document.activeElement;
    for (const [key, node] of this.nodes) if (node === active || node.contains(active)) {
      const index = this.indices.get(key);
      if (index !== undefined) visible.add(index);
    }
    const keys = new Set([...visible].map(i => this.key(this.records[i])));
    for (const [key, node] of this.nodes) if (!keys.has(key)) { node.parentElement.remove(); this.nodes.delete(key); }
    if (!this.indices.has(this.focusKey)) this.focusKey = this.records[range.start] && this.key(this.records[range.start]);
    // Ensure the grid has an entry point when its previous focus target is offscreen.
    const tabKey = keys.has(this.focusKey) ? this.focusKey : this.records[range.start] && this.key(this.records[range.start]);
    const width = (this.viewport.clientWidth - (this.columns - 1) * 12) / this.columns;
    let previous = null;
    for (const i of [...visible].sort((a, b) => a - b)) {
      const record = this.records[i], key = this.key(record);
      let node = this.nodes.get(key);
      if (!node) {
        node = this.create(record);
        node.addEventListener('click', () => { this.focusKey = key; this.select(key); this.schedule(); });
        node.addEventListener('focus', () => { this.focusKey = key; });
        this.nodes.set(key, node);
        const entry = document.createElement('div');
        entry.className = 'grid-entry';
        entry.setAttribute('role', 'listitem');
        entry.append(node);
        this.canvas.append(entry);
      }
      this.update(node, record);
      node.tabIndex = key === tabKey ? 0 : -1;
      const entry = node.parentElement;
      entry.style.width = `${width}px`;
      entry.style.left = `${(i % this.columns) * (width + 12)}px`;
      entry.style.top = `${Math.floor(i / this.columns) * ROW_HEIGHT}px`;
      entry.setAttribute('aria-posinset', i + 1);
      entry.setAttribute('aria-setsize', this.records.length);
      const next = previous ? previous.nextSibling : this.canvas.firstChild;
      if (next !== entry) {
        if (this.canvas.moveBefore) this.canvas.moveBefore(entry, next);
        else this.canvas.insertBefore(entry, next);
      }
      previous = entry;
    }
    if (active?.isConnected && this.canvas.contains(active) && document.activeElement !== active) active.focus({ preventScroll: true });
  }
  reveal(key, focus = false) {
    const index = this.indices.get(key);
    if (index === undefined) return;
    const top = Math.floor(index / this.columns) * ROW_HEIGHT;
    if (top < this.viewport.scrollTop || top + ROW_HEIGHT > this.viewport.scrollTop + this.viewport.clientHeight) this.viewport.scrollTop = top;
    this.focusKey = key;
    this.render();
    if (focus) this.nodes.get(key)?.focus({ preventScroll: true });
  }
  navigate(event) {
    const index = this.indices.get(this.focusKey);
    if (index === undefined || !this.viewport.contains(document.activeElement)) return;
    const moves = { ArrowLeft: index - 1, ArrowRight: index + 1, ArrowUp: index - this.columns, ArrowDown: index + this.columns, Home: 0, End: this.records.length - 1 };
    if (!(event.key in moves)) return;
    event.preventDefault();
    const next = Math.max(0, Math.min(this.records.length - 1, moves[event.key]));
    this.reveal(this.key(this.records[next]), true);
  }
}

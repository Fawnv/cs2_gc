import { InventoryConnection } from './connection.js';
import { loadCatalog } from './catalog.js';
import { loadIcons, createThumbnail, setThumbnail } from './icons.js';
import { Presentation, filterInventory } from './presentation.js';
import { VirtualGrid } from './grid.js';
import { CreationPreferences } from './creation-draft.js';

const $ = id => document.getElementById(id);
let catalog = null, icons = null, iconsLoading = false, loading = false;
let selectedKey = null, selectedId = null, activeTab = 'inventory';
let presentation = new Presentation(null);
let inventoryMatches = [], catalogueMatches = [];
let previousItems = new Map(), previousCatalog = null;
let drawerReturn = null;
const narrow = matchMedia('(max-width:999px)');
const preferences = new CreationPreferences();
let draftError = '';
let renderedWearKey = null, wearDirty = false;
function creationDraft() {
  draftError = '';
  if (!catalog || !selectedKey) return null;
  try { return preferences.build(catalog, selectedKey); }
  catch (error) { draftError = error.message; return null; }
}

function selectedTemplate() {
  return creationDraft()?.item ?? null;
}
function renderBadges(container, badges) {
  container.replaceChildren(...badges.map(text => {
    const badge = document.createElement('span');
    badge.textContent = text;
    if (text === 'StatTrak') badge.className = 'badge-stattrak';
    return badge;
  }));
}
function renderMetadata(container, entries) {
  container.replaceChildren(...entries.flatMap(([label, value]) => {
    const term = document.createElement('dt');
    const description = document.createElement('dd');
    term.textContent = label; description.textContent = value;
    return [term, description];
  }));
}
function createCard() {
  const button = document.createElement('button');
  button.type = 'button'; button.className = 'item-card';
  const image = createThumbnail();
  const title = document.createElement('span'); title.className = 'card-title';
  const subtitle = document.createElement('span'); subtitle.className = 'card-subtitle';
  const badges = document.createElement('span'); badges.className = 'card-badges';
  button.append(image, title, subtitle, badges);
  return button;
}
function updateCard(node, item, record = null) {
  const info = presentation.item(item, record);
  node.dataset.key = record?.key ?? item.highId;
  node.style.setProperty('--rarity', info.color);
  node.children[1].textContent = info.title;
  node.children[2].textContent = info.subtitle;
  renderBadges(node.children[3], info.badges);
  node.setAttribute('aria-label', [info.name, ...info.badges].join(', '));
  node.title = info.name;
  node.setAttribute('aria-pressed', String(record ? selectedKey === record.key : selectedId === item.highId));
  node.classList.toggle('created-item', !record && connection.creation?.status === 'confirmed' && connection.creation.highId === item.highId);
  setThumbnail(node.firstChild, record ? icons?.recordCandidates(record) : icons?.candidates(item));
}
const inventoryGrid = new VirtualGrid($('items'), {
  key: item => item.highId, create: createCard,
  update: (node, item) => updateCard(node, item),
  select: id => { selectedId = id; inventoryGrid.schedule(); renderSelection(); openDrawer(); },
});
const catalogueGrid = new VirtualGrid($('catalog-results'), {
  key: record => record.key, create: createCard,
  update: (node, record) => updateCard(node, record.template ?? { defIndex: record.defIndex, rarity: record.rarity }, record),
  select: key => { commitWear(); selectedKey = key; catalogueGrid.schedule(); renderSelection(); openDrawer(); },
});
// Delegation keeps the action attached to the current virtualized card identity.
$('catalog-results').addEventListener('dblclick', event => {
  const card = event.target.closest('.item-card');
  if (!card || narrow.matches) return;
  commitWear();
  selectedKey = card.dataset.key;
  renderSelection();
  addSelected();
});

function openDrawer() {
  if (!narrow.matches || $('selection').hidden) return;
  drawerReturn = document.activeElement;
  $('inspector').classList.add('drawer-open');
  document.body.classList.add('drawer-active');
  $('inspector').setAttribute('role', 'dialog');
  $('inspector').setAttribute('aria-modal', 'true');
  document.querySelectorAll('main > :not(.workspace), .workspace > section').forEach(node => node.inert = true);
  $('inspector-close').focus();
}
function closeDrawer(restoreFocus = true) {
  $('inspector').classList.remove('drawer-open');
  document.body.classList.remove('drawer-active');
  $('inspector').removeAttribute('role'); $('inspector').removeAttribute('aria-modal');
  document.querySelectorAll('[inert]').forEach(node => node.inert = false);
  if (restoreFocus && drawerReturn?.isConnected) drawerReturn.focus({ preventScroll: true });
}
$('inspector-close').addEventListener('click', () => closeDrawer());
narrow.addEventListener('change', () => { closeDrawer(false); $('inspector-close').hidden = !narrow.matches; });
$('inspector-close').hidden = !narrow.matches;
$('inspector').addEventListener('keydown', event => {
  if (!$('inspector').classList.contains('drawer-open')) return;
  if (event.key === 'Escape') { event.preventDefault(); closeDrawer(); }
  if (event.key !== 'Tab') return;
  const controls = [...$('inspector').querySelectorAll('button, input')].filter(node => !node.disabled && node.getClientRects().length);
  const first = controls[0], last = controls.at(-1);
  if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
  else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
});

function renderSelection() {
  const record = activeTab === 'catalog' ? catalog?.records.get(selectedKey) : null;
  const draft = record ? creationDraft() : null;
  // Keep controls available for correcting invalid inputs.
  let controls = draft;
  if (record?.template && !controls) {
    try { const defaults = new CreationPreferences(); defaults.statTrak = preferences.statTrak; controls = defaults.build(catalog, selectedKey); } catch {}
  }
  const item = activeTab === 'inventory' ? connection.inventory.items.get(selectedId) : draft?.item;
  const hasSelection = Boolean(record || item);
  $('selection').hidden = !hasSelection;
  $('inspector-empty').hidden = hasSelection;
  $('create').hidden = activeTab !== 'catalog';
  $('stattrak-label').hidden = !record?.statTrakTemplate;
  $('creation-controls').hidden = !record?.template;
  $('stattrak').checked = preferences.statTrak;
  $('paint-controls').hidden = !controls?.painted;
  $('counter-label').hidden = !controls?.statTrak;
  $('delete').hidden = activeTab !== 'inventory';
  $('delete').disabled = !item || !connection.canMutate;
  $('deletion-status').textContent = connection.deletion?.message || '';
  renderEdit(activeTab === 'inventory' ? item : null);
  if (controls?.painted) {
    for (const id of ['wear', 'wear-slider']) { $(id).min = controls.min; $(id).max = controls.max; }
    const wear = preferences.wear === null ? controls.min : preferences.commitWear(preferences.wear, controls.min, controls.max);
    if (renderedWearKey !== record.key || (!wearDirty && document.activeElement !== $('wear'))) $('wear').value = wear;
    if (renderedWearKey !== record.key) wearDirty = false;
    $('wear-slider').value = wear;
  }
  renderedWearKey = controls?.painted ? record.key : null;
  $('creation-status').textContent = connection.creation?.message || '';
  $('view-created').hidden = !(connection.creation?.status === 'confirmed' && connection.inventory.items.has(connection.creation.highId));
  if (!hasSelection) { closeDrawer(false); return; }
  const info = presentation.item(item ?? { defIndex: record.defIndex }, record);
  $('selection-name').textContent = info.name;
  renderBadges($('selection-badges'), info.badges);
  const details = [];
  if (!record) {
    details.push(['Item', item.highId]);
    for (const [id, label] of [[8, 'Paint wear'], [7, 'Paint seed']]) {
      const attribute = item.attributes.find(a => a.defIndex === id);
      if (attribute) details.push([label, attribute.value]);
    }
  }
  if (record?.resultDefIndex != null) details.push(['Coupon creates', `Definition ${record.resultDefIndex}`]);
  if (record?.forcedStatTrak) details.push(['StatTrak', 'Forced coupon default']);
  renderMetadata($('selection-info'), details);
  $('selection-reason').textContent = [...new Set([record?.unavailableReason, record ? draftError : ''].filter(Boolean))].join(' ');
  setThumbnail($('selection-icon'), record ? (item ? icons?.candidates(item) : icons?.recordCandidates(record)) : icons?.candidates(item));
  $('create').disabled = !record || !item || !connection.canCreate;
}
function renderCatalog(reset = false) {
  if (!catalog) return;
  const view = $('catalog-view').value;
  $('catalog-all').disabled = view !== 'definitions';
  $('advanced-label').hidden = view !== 'definitions';
  catalogueMatches = catalog.search({ view, category: $('catalog-category').value, query: $('catalog-search').value, allDefinitions: $('catalog-all').checked });
  $('catalog-count').textContent = `${catalogueMatches.length.toLocaleString()} items`;
  catalogueGrid.setRecords(catalogueMatches, reset);
  renderSelection();
}
function rebuildInventory(reset = false) {
  inventoryMatches = filterInventory(connection.inventory.items.values(), presentation, { query: $('inventory-search').value, category: $('inventory-category').value });
  $('inventory-count').textContent = `${inventoryMatches.length.toLocaleString()} of ${connection.inventory.items.size.toLocaleString()} items`;
  $('empty').hidden = inventoryMatches.length > 0;
  $('empty').textContent = !connection.inventory.ready ? 'Connect to receive inventory.' : connection.inventory.items.size ? 'No items match your filters.' : 'Inventory is empty.';
  inventoryGrid.setRecords(inventoryMatches, reset);
}
function renderInventory(current) {
  $('status').textContent = current.status; $('status').dataset.state = current.status.toLowerCase(); $('message').textContent = current.message;
  $('connect').disabled = current.socket !== null; $('disconnect').disabled = current.socket === null;
  $('count').textContent = `(${current.inventory.items.size} items)`;
  if (!current.inventory.items.has(selectedId)) selectedId = null;
  const changed = previousCatalog !== catalog || previousItems.size !== current.inventory.items.size || [...current.inventory.items].some(([id, item]) => previousItems.get(id) !== item);
  if (changed) {
    previousItems = new Map(current.inventory.items); previousCatalog = catalog;
    rebuildInventory();
  } else { inventoryGrid.schedule(); }
  // Connection readiness and empty messages can change without changing the map.
  if (!inventoryMatches.length) {
    $('empty').hidden = false;
    $('empty').textContent = !current.inventory.ready ? 'Connect to receive inventory.' : current.inventory.items.size ? 'No items match your filters.' : 'Inventory is empty.';
  }
  renderSelection();
}
const tabs = ['inventory', 'catalog', 'mods'];
function switchTab(tab) {
  commitWear();
  closeDrawer(false); activeTab = tab;
  for (const name of tabs) {
    $(name + '-panel').hidden = name !== tab;
    $(name + '-tab').setAttribute('aria-selected', String(name === tab));
    $(name + '-tab').tabIndex = name === tab ? 0 : -1;
  }
  $('workspace').dataset.tab = tab;
  if (tab === 'mods' && !connection.mods) connection.requestMods();
  inventoryGrid.schedule(); catalogueGrid.schedule(); renderSelection();
}
for (const tab of tabs) {
  $(tab + '-tab').addEventListener('click', () => switchTab(tab));
  $(tab + '-tab').addEventListener('keydown', event => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
    event.preventDefault();
    const index = tabs.indexOf(tab);
    const next = event.key === 'Home' ? tabs[0] : event.key === 'End' ? tabs.at(-1)
      : tabs[(index + (event.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length];
    switchTab(next); $(next + '-tab').focus();
  });
}

// CSForge's Mods tab. Strings from mod.txt only ever go through textContent.
function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}
function modRow(mod, state) {
  const busy = connection.modsPending || connection.socket?.readyState !== 1;
  const row = element('div', 'mod-row' + (mod.enabled ? '' : ' mod-off'));
  row.setAttribute('role', 'listitem');

  const toggle = element('input');
  toggle.type = 'checkbox';
  toggle.checked = mod.enabled;
  toggle.disabled = busy;
  toggle.setAttribute('aria-label', `${mod.name} enabled`);
  toggle.addEventListener('change', () => connection.setMod(mod.folder, toggle.checked, mod.priority));

  const body = element('div');
  const sub = [mod.version && `v${mod.version}`, mod.author && `by ${mod.author}`, `mods\\${mod.folder}`].filter(Boolean).join(' · ');
  body.append(element('div', 'mod-name', mod.name), element('div', 'mod-sub', sub));
  if (mod.description) body.append(element('div', 'mod-desc', mod.description));
  const tags = element('div', 'mod-tags');
  const tagList = [`${mod.files} file${mod.files === 1 ? '' : 's'}`];
  if (mod.items) tagList.push('items');
  if (mod.localization) tagList.push('texts');
  for (const tag of tagList) tags.append(element('span', '', tag));
  if (state.loaded && mod.changed) tags.append(element('span', 'mod-pending', 'restart the game to apply'));
  body.append(tags);
  if (mod.lostFiles) {
    const lost = element('details', 'mod-lost');
    const winners = [...new Set(mod.lost.map(entry => entry.winner))].join(', ');
    lost.append(element('summary', '', `${mod.lostFiles} file${mod.lostFiles === 1 ? '' : 's'} replaced by ${winners} (higher priority)`));
    const list = element('ul');
    for (const entry of mod.lost) list.append(element('li', '', `${entry.path} ← ${entry.winner}`));
    if (mod.lostFiles > mod.lost.length) list.append(element('li', '', `…and ${mod.lostFiles - mod.lost.length} more`));
    lost.append(list);
    body.append(lost);
  }

  const priority = element('label', 'mod-priority', 'Priority');
  const input = element('input');
  input.type = 'number'; input.step = '1'; input.value = String(mod.priority); input.disabled = busy;
  const commit = () => {
    const value = Number(input.value);
    if (!Number.isInteger(value)) { input.value = String(mod.priority); return; }
    if (value !== mod.priority) connection.setMod(mod.folder, mod.enabled, value);
  };
  input.addEventListener('change', commit);
  input.addEventListener('keydown', event => { if (event.key === 'Enter') { event.preventDefault(); commit(); } });
  priority.append(input);

  row.append(toggle, body, priority);
  return row;
}
let renderedMods = null, renderedModsPending = null;
function renderMods(current) {
  const state = current.mods;
  $('mods-refresh').disabled = current.socket?.readyState !== 1 || current.modsPending;
  $('mods-count').textContent = state ? `(${state.mods.filter(mod => mod.enabled).length}/${state.mods.length})` : '';
  if (state === renderedMods && current.modsPending === renderedModsPending) return;
  renderedMods = state; renderedModsPending = current.modsPending;
  const focused = document.activeElement?.closest?.('.mod-row') ? [...$('mods-list').children].indexOf(document.activeElement.closest('.mod-row')) : -1;
  if (!state) {
    $('mods-status').textContent = current.socket ? (current.modsPending ? 'Loading mods…' : 'The GC has not sent the mod list.') : 'Connect to see the mods.';
    $('mods-message').textContent = '';
    $('mods-list').replaceChildren();
    return;
  }
  const enabled = state.mods.filter(mod => mod.enabled).length;
  $('mods-status').textContent = !state.modsOn
    ? `CSForge is off: set "mods" "1" in cs2_gc\\config.txt and restart the game. ${state.mods.length} mod${state.mods.length === 1 ? '' : 's'} in cs2_gc\\mods.`
    : state.loaded ? `CSForge loaded the mods this game run. ${enabled} of ${state.mods.length} enabled now.`
    : `CSForge had nothing to load this game run. ${enabled} of ${state.mods.length} enabled now.`;
  $('mods-message').textContent = state.message;
  $('mods-message').classList.toggle('status-error', state.message.startsWith("Can't"));
  if (!state.mods.length) {
    $('mods-list').replaceChildren(element('p', '', 'No mods yet: every folder in cs2_gc\\mods is one (see the README, "CSForge").'));
    return;
  }
  $('mods-list').replaceChildren(...state.mods.map(mod => modRow(mod, state)));
  if (focused >= 0) $('mods-list').children[focused]?.querySelector('input:not([disabled])')?.focus();
}
const connection = new InventoryConnection(current => { renderInventory(current); renderMods(current); });
$('mods-refresh').addEventListener('click', () => connection.requestMods());
$('connect').addEventListener('click', () => connection.connect());
$('disconnect').addEventListener('click', () => connection.disconnect());
window.addEventListener('pagehide', () => connection.disconnect());
for (const id of ['catalog-view', 'catalog-category', 'catalog-all', 'catalog-search']) $(id).addEventListener(id === 'catalog-search' ? 'input' : 'change', () => renderCatalog(true));
for (const id of ['inventory-search', 'inventory-category']) $(id).addEventListener(id === 'inventory-search' ? 'input' : 'change', () => { rebuildInventory(true); renderSelection(); });
$('stattrak').addEventListener('change', () => { preferences.statTrak = $('stattrak').checked; renderSelection(); });
for (const [id, field] of [['wear-slider', 'wear'], ['seed', 'seed'], ['counter', 'counter']]) {
  $(id).addEventListener('input', () => {
    preferences[field] = $(id).value;
    if (id === 'wear-slider') { $('wear').value = $(id).value; wearDirty = false; }
    renderSelection();
  });
}
function commitWear() {
  if (activeTab !== 'catalog' || $('paint-controls').hidden || !wearDirty) return;
  preferences.commitWear($('wear').value, Number($('wear').min), Number($('wear').max));
  $('wear').value = preferences.wear;
  wearDirty = false;
  renderSelection();
}
$('wear').addEventListener('input', () => { wearDirty = true; });
$('wear').addEventListener('blur', commitWear);
$('wear').addEventListener('keydown', event => {
  if (event.key === 'Enter') { event.preventDefault(); commitWear(); }
});
// editing an item of the inventory: the fields follow the item until one is changed
const RARITIES = [[0, 'Stock'], [1, 'Consumer'], [2, 'Industrial'], [3, 'Mil-Spec'], [4, 'Restricted'], [5, 'Classified'], [6, 'Covert'], [7, 'Contraband']];
const QUALITIES = [[0, 'Normal'], [1, 'Genuine'], [2, 'Vintage'], [3, '★ Unusual'], [4, 'Unique'], [9, 'StatTrak™'], [12, 'Souvenir']];
let editId = null, editDirty = false;
const attributeOf = (item, id) => item.attributes.find(a => a.defIndex === id)?.value;
function fillOptions(select, options, value) {
  const list = options.some(([v]) => v === value) ? options : [...options, [value, `Other (${value})`]];
  select.replaceChildren(...list.map(([v, label]) => new Option(label, String(v))));
  select.value = String(value);
}
function renderEdit(item) {
  $('edit-controls').hidden = !item;
  if (!item) { editId = null; return; }
  $('save').disabled = !connection.canMutate || !editDirty;
  if (editId === item.highId && editDirty) return;
  editId = item.highId; editDirty = false;
  fillOptions($('edit-rarity'), RARITIES, item.rarity);
  fillOptions($('edit-quality'), QUALITIES, item.quality);
  $('edit-paint').hidden = attributeOf(item, 6) === undefined;
  $('edit-wear').value = attributeOf(item, 8) ?? '0';
  $('edit-seed').value = Math.round(Number(attributeOf(item, 7) ?? 0));
  const kills = attributeOf(item, 80);
  // a storage unit (1201) has no model for the counter to go on: the game crashes on it
  $('edit-stattrak').closest('label').hidden = item.defIndex === 1201;
  $('edit-stattrak').checked = kills !== undefined && item.defIndex !== 1201;
  $('edit-counter').value = kills ?? 0;
  $('edit-counter-label').hidden = kills === undefined || item.defIndex === 1201;
  $('edit-name').value = attributeOf(item, 111) ?? '';
  $('edit-status').textContent = connection.modification?.highId === item.highId ? connection.modification.message : '';
  $('edit-status').classList.remove('status-error');
}
function editChanged() {
  editDirty = true;
  $('edit-status').textContent = '';
  $('edit-status').classList.remove('status-error');
  $('save').disabled = !connection.canMutate;
}
for (const id of ['edit-rarity', 'edit-quality', 'edit-wear', 'edit-seed', 'edit-counter', 'edit-name']) $(id).addEventListener('input', editChanged);
$('edit-stattrak').addEventListener('change', () => {
  const on = $('edit-stattrak').checked;
  $('edit-counter-label').hidden = !on;
  // a gun's StatTrak is also its quality (unique <-> strange); a knife keeps its star
  if (on && $('edit-quality').value === '4') $('edit-quality').value = '9';
  if (!on && $('edit-quality').value === '9') $('edit-quality').value = '4';
  editChanged();
});
function editedItem(item) {
  const attributes = item.attributes.map(a => ({ ...a }));
  const set = (id, value) => {
    const index = attributes.findIndex(a => a.defIndex === id);
    if (value === null) { if (index >= 0) attributes.splice(index, 1); return; }
    if (index >= 0) attributes[index].value = String(value); else attributes.push({ defIndex: id, value: String(value) });
  };
  if (!$('edit-paint').hidden) {
    const wear = Number($('edit-wear').value.trim().replace(',', '.'));
    if (!Number.isFinite(wear) || wear < 0 || wear > 1) throw new Error('Wear must be a number from 0 to 1.');
    const seed = Number($('edit-seed').value);
    if (!Number.isInteger(seed) || seed < 0 || seed > 1000) throw new Error('Paint seed must be a whole number from 0 to 1000.');
    set(8, wear); set(7, seed);
  }
  if ($('edit-stattrak').checked) {
    const counter = Number($('edit-counter').value);
    if (!Number.isInteger(counter) || counter < 0 || counter > 0xffffffff) throw new Error('The StatTrak counter must be a whole number.');
    set(80, counter);
    if (!attributes.some(a => a.defIndex === 81)) set(81, 0);
  } else { set(80, null); set(81, null); }
  set(111, $('edit-name').value.trim() || null);
  return { ...item, rarity: Number($('edit-rarity').value), quality: Number($('edit-quality').value), attributes };
}
$('save').addEventListener('click', () => {
  const item = connection.inventory.items.get(selectedId);
  if (!item || !connection.canMutate) return;
  try {
    const edited = editedItem(item);
    editDirty = false;
    connection.modify(edited, presentation.item(edited).name);
  }
  catch (error) { $('edit-status').textContent = error.message; $('edit-status').classList.add('status-error'); }
});
function addSelected() {
  if (!connection.canCreate) return;
  commitWear();
  try { const record = catalog.records.get(selectedKey); connection.create(selectedTemplate(), record.name || record.internalName || record.key); }
  catch (error) { $('creation-status').textContent = error.message; }
}
$('create').addEventListener('click', addSelected);
$('delete').addEventListener('click', () => {
  const item = connection.inventory.items.get(selectedId);
  if (!item || !connection.canMutate) return;
  try { connection.delete(item.highId, presentation.item(item).name); }
  catch (error) { $('deletion-status').textContent = error.message; }
});
$('view-created').addEventListener('click', () => {
  const id = connection.creation?.highId;
  if (!connection.inventory.items.has(id)) return;
  if (!inventoryMatches.some(item => item.highId === id)) {
    $('inventory-search').value = ''; $('inventory-category').value = ''; rebuildInventory(true);
  }
  selectedId = id; switchTab('inventory');
  requestAnimationFrame(() => { inventoryGrid.reveal(id, true); renderSelection(); openDrawer(); });
});

async function initializeCatalog() {
  if (loading) return;
  loading = true;
  $('catalog-status').textContent = 'Loading catalogue...';
  $('catalog-status').classList.remove('status-error');
  $('catalog-retry').hidden = true;
  try {
    catalog = await loadCatalog();
    presentation = new Presentation(catalog);
    if (icons) {
      try { icons.assertCatalog(catalog); }
      catch (error) { icons = null; iconFailure(error); }
    }
    $('catalog-status').textContent = '';
    const categories = [...new Set([...catalog.data.definitions, ...catalog.data.variants].map(r => r.category))].sort();
    for (const category of categories) {
      const option = document.createElement('option');
      option.value = category;
      option.textContent = category;
      $('catalog-category').append(option);
      $('inventory-category').append(option.cloneNode(true));
    }
    $('catalog-controls').disabled = false;
    renderCatalog();
    inventoryGrid.schedule();
    renderInventory(connection);
  } catch (error) {
    $('catalog-status').textContent = `Catalogue unavailable: ${error.message}`;
    $('catalog-status').classList.add('status-error');
    $('catalog-retry').hidden = false;
  } finally { loading = false; }
}
$('catalog-retry').addEventListener('click', initializeCatalog);
function iconFailure(error) {
  $('icon-status').textContent = `Icons unavailable: ${error.message}`;
  $('icon-status').classList.add('status-error');
  $('icon-retry').hidden = false;
}

async function initializeIcons() {
  if (iconsLoading) return;
  iconsLoading = true;
  $('icon-status').textContent = 'Loading icons...';
  $('icon-status').classList.remove('status-error');
  $('icon-retry').hidden = true;
  try {
    const loaded = await loadIcons();
    if (catalog) loaded.assertCatalog(catalog);
    icons = loaded;
    $('icon-status').textContent = '';
  } catch (error) {
    icons = null;
    iconFailure(error);
  } finally {
    iconsLoading = false;
    catalogueGrid.schedule();
    inventoryGrid.schedule();
    renderInventory(connection);
  }
}
$('icon-retry').addEventListener('click', initializeIcons);
rebuildInventory();
renderInventory(connection);
if (location.protocol === 'file:') {
  $('message').textContent = 'Open http://127.0.0.1:13001/ while CS2 runs with cs2_gc instead of opening this file directly.';
}
initializeCatalog();
initializeIcons();

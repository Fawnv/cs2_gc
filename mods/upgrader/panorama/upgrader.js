var CsgcUpgrader = (function () {
    // per skin: def, paint kit, tier (the color), then its prices per wear (Factory New .. Battle-Scarred)
    // in hundredths of the currency: 5 plain, 5 StatTrak, 5 souvenir; 0 = not on sale
    const CATALOG = [@csforge:upgrader_catalog@];
    const STRIDE = 18;
    const CURRENCY = '@csforge:upgrader_currency@'; // empty: no price list, the prices are 10^tier (1:10 per rarity step)
    const PRICE_TIME = @csforge:upgrader_price_time@;
    const COUNT = CATALOG.length / STRIDE;
    const COLORS = ['#ffffff', '#b0c3d9', '#5e98d9', '#4b69ff', '#8847ff', '#d32ce6', '#eb4b4b', '#e4ae39'];
    const WEARS = ['FN', 'MW', 'FT', 'WW', 'BS'];
    const MULTIPLIERS = [1.5, 2, 3, 5, 10];
    const PAGE_SIZE = 20;
    const MAX_DEPOSITS = 10;
    const SPIN_SECONDS = 6;
    const SPIN_TURNS = 8;
    const REVEAL_DELAY = 0.8; // the game's new item popup, this long after the needle stops
    const REST = 180; // the needle waits at the bottom, in the middle of the win arc
    const byKey = {}; // def * 65536 + paint kit -> catalog index
    for (let i = 0; i < COUNT; i++)
        byKey[CATALOG[i * STRIDE] * 65536 + CATALOG[i * STRIDE + 1]] = i;
    const names = {};
    let prices = {}; // item id -> what it's worth, until the next inventory update
    let categories = null;
    const state = { deposits: [], target: null, tier: 0, kind: 'all', same: false, statTrak: false, multiplier: 0, page: 0, picking: false,
        busy: false, result: null, rotation: REST, spinChance: 0, updates: 0, hold: null, note: '' };
    let ui = null;
    let panel = null;
    function T(token, fallback) {
        const text = $.Localize('#' + token);
        return (!text || text.charAt(0) === '#') ? fallback : text;
    }
    function Money(hundredths) {
        if (!CURRENCY || !hundredths)
            return '';
        const fixed = (hundredths / 100).toFixed(hundredths >= 100000 ? 0 : 2).split('.');
        const text = fixed[0].replace(/\B(?=(\d{3})+(?!\d))/g, ' ') + (fixed.length > 1 ? '.' + fixed[1] : '');
        if (CURRENCY === 'USD')
            return '$' + text;
        if (CURRENCY === 'EUR')
            return text + ' €';
        if (CURRENCY === 'RUB')
            return text + ' ' + T('csgc_upgrade_rub', 'RUB');
        return text + ' ' + CURRENCY;
    }
    function Percent(chance) {
        const p = chance * 100;
        return (p >= 1 ? p.toFixed(2) : p >= 0.01 ? p.toFixed(3) : p.toFixed(5)) + '%';
    }
    // kind: 0 plain, 1 StatTrak, 2 souvenir
    function Price(index, wear, kind) {
        return CATALOG[index * STRIDE + 3 + kind * 5 + wear] || 0;
    }
    function TierOf(id) {
        const slot = InventoryAPI.GetDefaultSlot(id);
        if (slot === 'melee' || slot === 'clothing_hands')
            return 7;
        return Number(InventoryAPI.GetItemRarity(id)) || 0;
    }
    function Kind(def) {
        return def >= 4700 ? 'gloves' : def >= 500 ? 'knives' : 'guns';
    }
    function Faux(index) {
        return InventoryAPI.GetFauxItemIDFromDefAndPaintIndex(CATALOG[index * STRIDE], CATALOG[index * STRIDE + 1]);
    }
    function Name(index) {
        if (!(index in names))
            names[index] = InventoryAPI.GetItemName(Faux(index));
        return names[index];
    }
    // an inventory item's catalog skin: by its paint kit, or by the name when that can't be read
    // (the item's name has the finish's in it, with StatTrak or Souvenir in front)
    function IndexOf(id) {
        const def = Number(InventoryAPI.GetItemDefinitionIndex(id));
        const index = byKey[def * 65536 + Number(InventoryAPI.GetItemAttributeValue(id, 'set item texture prefab'))];
        if (index !== undefined)
            return index;
        const name = InventoryAPI.GetItemName(id) || '';
        for (let i = 0; i < COUNT; i++) {
            if (CATALOG[i * STRIDE] === def && name.indexOf(Name(i)) >= 0)
                return i;
        }
        return -1;
    }
    // what an inventory item is worth, the way the gc counts it: its wear, StatTrak, souvenir
    function ItemPrice(id) {
        if (id in prices)
            return prices[id];
        let price = 0;
        const index = IndexOf(id);
        if (index >= 0) {
            let wear = Number(InventoryAPI.GetWear(id));
            if (!(wear >= 0 && wear <= 4))
                wear = 0;
            const statTrak = InventoryAPI.GetItemAttributeValue(id, 'kill eater') !== undefined;
            const souvenir = Number(InventoryAPI.GetItemQuality(id)) === 12;
            price = Price(index, wear, statTrak ? 1 : souvenir ? 2 : 0);
        }
        prices[id] = price;
        return price;
    }
    // the guns, knives and gloves in the inventory, the way the game's own search lists them: the
    // filter wants a loadout category first, flags alone give nothing. Also how a burnt deposit is
    // noticed (IsValidItemID only looks at the id's form)
    function InventoryItems() {
        if (!categories) {
            categories = ['secondary', 'smg', 'rifle', 'heavy', 'melee', 'clothing_hands'];
            try {
                InventoryAPI.GetCategories().split(',').forEach((category) => {
                    InventoryAPI.GetSubCategories(category).split(',').forEach((sub) => {
                        if (sub && categories.indexOf(sub) < 0 && /melee|knife|glove|hand|clothing/.test(sub))
                            categories.push(sub);
                    });
                });
            }
            catch (e) {
                $.Msg('[csgc upgrader] categories: ' + e + '\n');
            }
            $.Msg('[csgc upgrader] inventory categories: ' + categories.join(', ') + '\n');
        }
        const ids = [];
        const seen = {};
        categories.forEach((category) => {
            InventoryAPI.SetInventorySortAndFilters('inv_sort_rarity', false, category + ',is_rental:false', '', '');
            const count = InventoryAPI.GetInventoryCount();
            for (let i = 0; i < count; i++) {
                const id = InventoryAPI.GetInventoryItemIDByIndex(i);
                if (!seen[id]) {
                    seen[id] = true;
                    ids.push(id);
                }
            }
        });
        return ids;
    }
    // why an item can't go in: '' if it can
    function Reject(id) {
        if (!id || !InventoryAPI.IsValidItemID(id) || InventoryAPI.IsFauxItemID(id) || InventoryAPI.IsRental(id))
            return 'id';
        const slot = InventoryAPI.GetDefaultSlot(id);
        if (!slot || ['c4', 'flair0', 'musickit', 'spray0', 'customplayer', 'pet'].indexOf(slot) >= 0)
            return 'slot';
        // stock guns (with a charm, stickers or a name they're real items too) are normal quality
        if (Number(InventoryAPI.GetItemQuality(id)) === 0)
            return 'stock';
        return ItemPrice(id) > 0 ? '' : 'price';
    }
    function CanDeposit(id) {
        return Reject(id) === '';
    }
    function BetValue() {
        let total = 0;
        for (const id of state.deposits) {
            const price = ItemPrice(id);
            if (!price)
                return 0;
            total += price;
        }
        return total;
    }
    function TargetPrice(target) {
        target = target || state.target;
        return target ? Price(target.index, target.wear, target.st ? 1 : 0) : 0;
    }
    // what the bet is worth over the target's price; 0 when the target isn't dearer
    function Chance() {
        const bet = BetValue();
        const price = TargetPrice();
        return bet > 0 && price > bet ? bet / price : 0;
    }
    function TargetLine(target, bet) {
        const price = TargetPrice(target);
        return WEARS[target.wear] + (target.st ? ' ST' : '') + (CURRENCY ? ' · ' + Money(price) : '') +
            (bet > 0 && price > bet ? ' · ' + Percent(bet / price) : '');
    }
    function RuleText() {
        if (!CURRENCY)
            return T('csgc_upgrade_rule', '1:10 per rarity step, the chances of several skins add up. The skins burn if it fails.');
        const date = new Date(PRICE_TIME * 1000);
        const two = (n) => (n < 10 ? '0' : '') + n;
        return T('csgc_upgrade_rule_prices', 'Chance = what the bet is worth / the target\'s price, the target has to cost more. The skins burn if it fails. Prices:') +
            ' market.csgo.com, ' + two(date.getDate()) + '.' + two(date.getMonth() + 1) + ' ' + two(date.getHours()) + ':' + two(date.getMinutes());
    }
    function Style(target, style) {
        for (const key in style)
            target.style[key] = style[key];
        return target;
    }
    function Box(parent, flow, style) {
        const box = $.CreatePanel('Panel', parent, '');
        box.style.flowChildren = flow;
        return Style(box, style || {});
    }
    function Text(parent, text, style) {
        const label = $.CreatePanel('Label', parent, '');
        label.text = text;
        return Style(label, Object.assign({ color: '#ffffffdd', fontSize: '18px' }, style || {}));
    }
    function Btn(parent, text, onClick, style) {
        const button = $.CreatePanel('Button', parent, '');
        Style(button, Object.assign({ backgroundColor: '#ffffff1a', padding: '6px 12px', margin: '3px' }, style || {}));
        if (text)
            Text(button, text, { fontSize: '16px', horizontalAlign: 'center', verticalAlign: 'center' });
        button.SetPanelEvent('onactivate', onClick);
        return button;
    }
    function Tile(parent, itemId, tier, label, onClick, selected, sub) {
        const tile = $.CreatePanel('Button', parent, '');
        Style(tile, { width: '132px', height: '146px', margin: '4px', flowChildren: 'down', backgroundColor: selected ? '#ffffff38' : '#ffffff0d' });
        const image = $.CreatePanel('ItemImage', tile, '', { itemid: itemId, scaling: 'stretch-to-fit-preserve-aspect' });
        image.itemid = itemId;
        Style(image, { width: '124px', height: '84px', horizontalAlign: 'center' });
        Box(tile, 'none', { width: '100%', height: '3px', backgroundColor: COLORS[tier] || '#ffffff' });
        Text(tile, label, { fontSize: '13px', width: '100%', textAlign: 'center', textOverflow: 'shrink' });
        if (sub)
            Text(tile, sub, { fontSize: '13px', color: '#e8c060', width: '100%', textAlign: 'center', textOverflow: 'shrink' });
        if (onClick)
            tile.SetPanelEvent('onactivate', onClick);
        return tile;
    }
    function BuildWheel(parent) {
        const size = 300;
        const wheel = Box(parent, 'none', { width: size + 'px', height: size + 'px', horizontalAlign: 'center', margin: '6px 0px 12px 0px' });
        Box(wheel, 'none', { width: '100%', height: '100%', borderRadius: '50%', backgroundColor: '#3b3f45' });
        Box(wheel, 'none', { width: (size - 14) + 'px', height: (size - 14) + 'px', borderRadius: '50%', backgroundColor: '#17191c', horizontalAlign: 'center', verticalAlign: 'center' });
        ui.arc = Box(wheel, 'none', { width: (size - 14) + 'px', height: (size - 14) + 'px', borderRadius: '50%', backgroundColor: '#e8840c', horizontalAlign: 'center', verticalAlign: 'center' });
        ui.arc.style.clip = 'radial(50% 50%, 180deg, 0deg)';
        const inner = Box(wheel, 'down', { width: (size - 92) + 'px', height: (size - 92) + 'px', borderRadius: '50%', backgroundColor: '#22252a', border: '3px solid #4a4e55', horizontalAlign: 'center', verticalAlign: 'center' });
        ui.percent = Text(inner, '-', { fontSize: '42px', color: '#ffffff', horizontalAlign: 'center', margin: '64px 0px 0px 0px' });
        Text(inner, T('csgc_upgrade_chance', 'Chance'), { fontSize: '18px', color: '#ffffffaa', horizontalAlign: 'center' });
        ui.spinner = Box(wheel, 'none', { width: '100%', height: '100%' });
        Box(ui.spinner, 'none', { width: '10px', height: '50px', backgroundColor: '#ffffff', borderRadius: '5px', border: '2px solid #000000aa', horizontalAlign: 'center', verticalAlign: 'top', margin: '2px 0px 0px 0px' });
        ui.spinner.style.transform = 'rotateZ(' + REST + 'deg)';
    }
    function SetWheel(chance) {
        const sweep = Math.max(0, Math.min(360, chance * 360));
        ui.arc.style.clip = 'radial(50% 50%, ' + (180 - sweep / 2).toFixed(3) + 'deg, ' + sweep.toFixed(3) + 'deg)';
        ui.percent.text = chance > 0 ? Percent(chance) : '-';
    }
    // the game shows its new item popup on every inventory update and the count on the inventory
    // button: both held back while the wheel spins, the way the game's own case and trade up
    // popups do it (CapabilityPopupIsOpen), or the won skin shows before the needle stops
    function HoldReveal() {
        let root = panel;
        while (root.GetParent())
            root = root.GetParent();
        state.hold = { alert: root.FindChildTraverse('MainMenuInvAlert'), count: InventoryAPI.GetUnacknowledgeItemsCount() };
        $.DispatchEvent('CapabilityPopupIsOpen', true);
    }
    function KeepAlert() {
        const alert = state.hold && state.hold.alert;
        if (alert && alert.IsValid()) {
            alert.SetDialogVariable('alert_value', state.hold.count.toString());
            alert.SetHasClass('hidden', state.hold.count < 1);
        }
    }
    function Reveal() {
        if (!state.hold)
            return;
        state.hold.count = InventoryAPI.GetUnacknowledgeItemsCount();
        KeepAlert();
        state.hold = null;
        $.DispatchEvent('CapabilityPopupIsOpen', false); // the inventory looks again: the popup
    }
    function Spin(won, then) {
        // the needle lands inside the win arc (centered at the bottom) or outside it, after 8 turns.
        // Turned frame by frame: a transition goes from one rotateZ to the other the short way,
        // whole turns in between are lost
        const sweep = Math.max(0, Math.min(360, state.spinChance * 360));
        let land;
        if (won)
            land = 180 + (Math.random() - 0.5) * sweep * 0.8;
        else
            land = (180 + sweep / 2 + (360 - sweep) * (0.1 + 0.8 * Math.random())) % 360;
        const start = ((state.rotation % 360) + 360) % 360;
        let delta = land - start;
        if (delta < 0)
            delta += 360;
        const total = 360 * SPIN_TURNS + delta;
        const began = Date.now();
        let notch = 0;
        let ticked = 0;
        ui.spinner.style.transitionDuration = '0s';
        const frame = () => {
            const t = Math.min(1, (Date.now() - began) / (SPIN_SECONDS * 1000));
            // fast from the start, slowing down towards the end (ease out cubic)
            const turned = total * (1 - Math.pow(1 - t, 3));
            ui.spinner.style.transform = 'rotateZ(' + ((start + turned) % 360).toFixed(2) + 'deg)';
            // a tick every 30 degrees, no closer than 70 ms (a rattle while fast, clicks at the end)
            if (Math.floor(turned / 30) !== notch && Date.now() - ticked > 70) {
                notch = Math.floor(turned / 30);
                ticked = Date.now();
                $.DispatchEvent('CSGOPlaySoundEffect', 'UIPanorama.XP.Ticker', 'MOUSE');
            }
            KeepAlert();
            if (t < 1) {
                $.Schedule(0.01, frame);
                return;
            }
            state.rotation = (start + total) % 360;
            then();
        };
        frame();
    }
    function Build(target) {
        panel = target;
        ui = {};
        panel.style.flowChildren = 'down';
        ui.status = Text(panel, RuleText(), { fontSize: '16px', color: '#ffffff99', margin: '10px 16px 4px 16px' });
        const body = Box(panel, 'right', { width: '100%', height: 'fill-parent-flow(1.0)', margin: '4px 16px 12px 16px' });
        ui.left = Box(body, 'down', { width: '300px', height: '100%' });
        const middle = Box(body, 'down', { width: '340px', height: '100%', margin: '0px 16px' });
        ui.right = Box(body, 'down', { width: 'fill-parent-flow(1.0)', height: '100%' });
        BuildWheel(middle);
        ui.middle = Box(middle, 'down', { width: '100%' });
    }
    function Render() {
        if (!ui)
            return;
        ui.status.text = state.note || RuleText();
        // each part on its own, so one failing doesn't take the others with it
        const errors = [];
        [['left', RenderLeft], ['middle', RenderMiddle], ['right', RenderRight]].forEach((part) => {
            try {
                part[1]();
            }
            catch (e) {
                errors.push(part[0] + ': ' + e);
                $.Msg('[csgc upgrader] ' + part[0] + ': ' + e + '\n' + e.stack + '\n');
            }
        });
        if (errors.length)
            ui.status.text = 'Upgrader: ' + errors.join('; ');
    }
    function RenderLeft() {
        ui.left.RemoveAndDeleteChildren();
        const bet = BetValue();
        Text(ui.left, T('csgc_upgrade_deposit', 'Your skins') + ' (' + state.deposits.length + '/' + MAX_DEPOSITS + ')' +
            (CURRENCY && bet ? ': ' + Money(bet) : ''), { fontSize: '20px' });
        const grid = Box(ui.left, 'right-wrap', { width: '100%', height: 'fill-parent-flow(1.0)', overflow: 'squish scroll' });
        state.deposits.forEach((id) => {
            Tile(grid, id, TierOf(id), InventoryAPI.GetItemName(id), () => {
                if (state.busy)
                    return;
                state.deposits = state.deposits.filter((other) => other !== id);
                state.result = null;
                Render();
            }, false, Money(ItemPrice(id)));
        });
        if (state.deposits.length < MAX_DEPOSITS && !state.busy) {
            Btn(ui.left, state.picking ? T('csgc_upgrade_back', 'Back to the targets') : '+ ' + T('csgc_upgrade_add', 'Add a skin'), () => {
                state.picking = !state.picking;
                state.page = 0;
                Render();
            }, { width: '100%' });
        }
    }
    function RenderMiddle() {
        ui.middle.RemoveAndDeleteChildren();
        const bet = BetValue();
        const chance = state.busy ? state.spinChance : Chance();
        if (!state.busy)
            SetWheel(state.result ? state.spinChance : chance);
        const row = Box(ui.middle, 'right', { horizontalAlign: 'center' });
        if (state.result && !state.busy) {
            if (state.result.won) {
                Tile(row, state.result.won, TierOf(state.result.won), InventoryAPI.GetItemName(state.result.won), null, true, Money(ItemPrice(state.result.won)));
                Text(ui.middle, T('csgc_upgrade_won', 'You got it!'), { fontSize: '26px', color: '#7fe07f', horizontalAlign: 'center' });
            }
            else {
                Text(ui.middle, T('csgc_upgrade_lost', 'No luck, the skins are gone'), { fontSize: '22px', color: '#e07f7f', horizontalAlign: 'center' });
            }
        }
        else if (state.target) {
            const tier = CATALOG[state.target.index * STRIDE + 2];
            Tile(row, Faux(state.target.index), tier, Name(state.target.index), null, true, TargetLine(state.target, 0));
            if (bet && TargetPrice() <= bet)
                Text(ui.middle, T('csgc_upgrade_too_cheap', 'The target has to cost more than the bet'), { fontSize: '15px', color: '#e07f7f', horizontalAlign: 'center' });
        }
        else {
            Text(ui.middle, T('csgc_upgrade_pick_target', 'Pick the skin you want on the right'), { fontSize: '15px', color: '#ffffff88', horizontalAlign: 'center' });
        }
        if (CURRENCY && !state.result && (bet || state.target))
            Text(ui.middle, (Money(bet) || '0') + '  →  ' + (Money(TargetPrice()) || '?'), { fontSize: '18px', color: '#e8c060', horizontalAlign: 'center', margin: '4px 0px 0px 0px' });
        const go = Btn(ui.middle, state.busy ? T('csgc_upgrade_wait', 'Rolling...') : T('csgc_upgrade_go', 'Upgrade'), Go,
            { width: '100%', height: '56px', margin: '10px 0px', backgroundColor: chance > 0 && !state.busy ? '#4b9e3fcc' : '#ffffff14' });
        go.enabled = chance > 0 && !state.busy;
    }
    function RenderPick(filters, items) {
        Text(filters, T('csgc_upgrade_your_items', 'Your skins'), { fontSize: '18px', margin: '4px' });
        const all = InventoryItems();
        const skipped = { id: 0, slot: 0, stock: 0, price: 0, deposit: 0 };
        all.forEach((id) => {
            const reason = Reject(id) || (state.deposits.indexOf(id) >= 0 ? 'deposit' : '');
            if (reason) {
                skipped[reason]++;
                return;
            }
            items.push({ id: id, tier: TierOf(id), name: InventoryAPI.GetItemName(id), price: ItemPrice(id), pick: () => {
                if (state.busy)
                    return;
                state.deposits.push(id);
                if (state.deposits.length >= MAX_DEPOSITS)
                    state.picking = false;
                state.result = null;
                state.note = '';
                Render();
            } });
        });
        items.forEach((item) => { item.sub = Money(item.price); });
        items.sort((a, b) => (b.price - a.price) || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
        if (!items.length) {
            // say why in words, so a screenshot tells what's wrong. The base guns (faux ids, one
            // per weapon, 'id') are in every category list and don't count
            const words = { stock: T('csgc_upgrade_skip_stock', 'stock'), price: T('csgc_upgrade_skip_price', 'without a market price'),
                deposit: T('csgc_upgrade_skip_deposit', 'in the bet'), slot: T('csgc_upgrade_skip_other', 'other') };
            const why = T('csgc_upgrade_guns_owned', 'skins in the inventory') + ': ' + (all.length - skipped.id) +
                Object.keys(words).filter((k) => skipped[k]).map((k) => ', ' + words[k] + ' ' + skipped[k]).join('');
            Text(filters, T('csgc_upgrade_no_guns', 'No skins with a market price to put in') + ' (' + why + ')', { fontSize: '15px', color: '#ffffff88', margin: '4px' });
            $.Msg('[csgc upgrader] deposit list empty: ' + all.length + ' listed, ' + JSON.stringify(skipped) + '\n');
        }
    }
    function Toggle(active) {
        return { backgroundColor: active ? '#ffffff40' : '#ffffff14' };
    }
    function RenderTargets(filters, items) {
        const bet = BetValue();
        Btn(filters, T('csgc_upgrade_any', 'Any'), () => { state.tier = 0; state.page = 0; Render(); }, Toggle(state.tier === 0));
        for (let t = 1; t <= 7; t++) {
            const tier = t;
            Btn(filters, '', () => { state.tier = state.tier === tier ? 0 : tier; state.page = 0; Render(); },
                { width: '30px', height: '30px', backgroundColor: COLORS[t] + (state.tier === t ? 'ff' : '50') });
        }
        [['all', T('csgc_upgrade_all', 'All')], ['guns', T('csgc_upgrade_guns', 'Guns')], ['knives', T('csgc_upgrade_knives', 'Knives')], ['gloves', T('csgc_upgrade_gloves', 'Gloves')]].forEach((kind) => {
            Btn(filters, kind[1], () => { state.kind = kind[0]; state.page = 0; Render(); }, Toggle(state.kind === kind[0]));
        });
        if (state.deposits.length) {
            Btn(filters, T('csgc_upgrade_same', 'Same weapon'), () => {
                state.same = !state.same;
                state.page = 0;
                if (state.same) {
                    // the deposit's weapon in every kind and rarity (knives + a gun's def is nothing)
                    state.kind = 'all';
                    state.tier = 0;
                }
                Render();
            }, Toggle(state.same));
        }
        Btn(filters, 'StatTrak™', () => { state.statTrak = !state.statTrak; state.page = 0; Render(); }, Toggle(state.statTrak));
        if (bet) {
            // how many times the bet the target costs at least: x2 starts the list at about 50%
            MULTIPLIERS.forEach((m) => {
                Btn(filters, 'x' + m, () => { state.multiplier = state.multiplier === m ? 0 : m; state.page = 0; Render(); }, Toggle(state.multiplier === m));
            });
        }
        const min = bet ? Math.max(bet + 1, state.multiplier ? Math.ceil(bet * state.multiplier) : 0) : 1;
        const firstDef = state.deposits.length ? Number(InventoryAPI.GetItemDefinitionIndex(state.deposits[0])) : -1;
        const kind = state.statTrak ? 1 : 0;
        for (let i = 0; i < COUNT; i++) {
            const base = i * STRIDE;
            const def = CATALOG[base];
            if ((state.tier && CATALOG[base + 2] !== state.tier) || (state.kind !== 'all' && Kind(def) !== state.kind) || (state.same && def !== firstDef))
                continue;
            for (let w = 0; w < 5; w++) {
                const price = CATALOG[base + 3 + kind * 5 + w];
                if (price && price >= min)
                    items.push({ index: i, wear: w, st: state.statTrak, price: price, tier: CATALOG[base + 2] });
            }
        }
        // cheapest first: from the multiplier's chance down
        items.sort((a, b) => (a.price - b.price) || (a.index - b.index) || (a.wear - b.wear));
        if (!items.length) {
            const text = bet ? T('csgc_upgrade_no_dearer', 'Nothing dearer than the bet with these filters: turn a filter or the multiplier off')
                : T('csgc_upgrade_no_targets', 'No targets with these filters: pick another rarity or turn a filter off');
            Text(filters, text + ' (' + state.kind + ', tier ' + (state.tier || 'any') + (state.same ? ', def ' + firstDef : '') +
                (state.statTrak ? ', StatTrak' : '') + (state.multiplier ? ', x' + state.multiplier : '') + ')',
                { fontSize: '15px', color: '#ffffff88', margin: '4px', width: '100%' });
        }
        return bet;
    }
    function RenderRight() {
        ui.right.RemoveAndDeleteChildren();
        const filters = Box(ui.right, 'right-wrap', { width: '100%' });
        const items = [];
        const bet = state.picking ? 0 : RenderTargets(filters, items);
        if (state.picking)
            RenderPick(filters, items);
        const pages = Math.max(1, Math.ceil(items.length / PAGE_SIZE));
        if (state.page >= pages)
            state.page = pages - 1;
        const grid = Box(ui.right, 'right-wrap', { width: '100%', height: 'fill-parent-flow(1.0)', overflow: 'squish scroll' });
        items.slice(state.page * PAGE_SIZE, (state.page + 1) * PAGE_SIZE).forEach((item) => {
            if (state.picking) {
                Tile(grid, item.id, item.tier, item.name, item.pick, false, item.sub);
                return;
            }
            // a target: the skin in one wear, what it costs and the chance for the bet
            const target = { index: item.index, wear: item.wear, st: item.st };
            const selected = !!state.target && state.target.index === item.index && state.target.wear === item.wear && state.target.st === item.st;
            Tile(grid, Faux(item.index), item.tier, Name(item.index), () => {
                if (state.busy)
                    return;
                state.target = target;
                state.result = null;
                Render();
            }, selected, TargetLine(target, bet));
        });
        const nav = Box(ui.right, 'right', { horizontalAlign: 'center' });
        Btn(nav, '<', () => { if (state.page > 0) { state.page--; Render(); } });
        Text(nav, (state.page + 1) + ' / ' + pages + '   (' + items.length + ')', { margin: '8px 14px' });
        Btn(nav, '>', () => { if (state.page + 1 < pages) { state.page++; Render(); } });
    }
    function Go() {
        const chance = Chance();
        if (state.busy || chance <= 0)
            return;
        UiToolkitAPI.ShowGenericPopupOkCancel(T('csgc_upgrade_title', 'Upgrader'),
            T('csgc_upgrade_confirm', 'The skins you put in burn if it fails. Go?') + ' ' + T('csgc_upgrade_chance', 'Chance') + ': ' + Percent(chance) +
            (CURRENCY ? ' (' + Money(BetValue()) + ' → ' + Money(TargetPrice()) + ')' : ''), '', Send, () => { });
    }
    function Send() {
        // listed ones first: a burnt deposit is noticed by its absence from the inventory lists
        const listed = {};
        InventoryItems().forEach((id) => { listed[id] = true; });
        const deposits = state.deposits.slice().sort((a, b) => (listed[b] ? 1 : 0) - (listed[a] ? 1 : 0));
        const target = state.target;
        // catalog index * 16 + wear * 2 + StatTrak, plus 1 (0 is no target)
        const code = target.index * 16 + target.wear * 2 + (target.st ? 1 : 0) + 1;
        state.spinChance = Chance();
        const before = {};
        const count = InventoryAPI.GetUnacknowledgeItemsCount();
        for (let i = 0; i < count; i++)
            before[InventoryAPI.GetUnacknowledgeItemByIndex(i)] = true;
        state.busy = true;
        state.picking = false;
        state.result = null;
        state.note = '';
        SetWheel(state.spinChance);
        Render();
        const updatesBefore = state.updates;
        HoldReveal();
        deposits.forEach((id) => InventoryAPI.SetItemAttributeValueAsync(id, 'upgrade threshold', code));
        InventoryAPI.SetItemAttributeValueAsync(deposits[0], 'upgrade level', deposits.length);
        // the gc's answer comes as an inventory update: the deposits are gone (rolled) or still there
        // (refused, the gc sends the first one back unchanged). An update can come before the
        // answer, so a deposit still there counts as refused only after a second of updates
        let tries = 0;
        let sinceUpdate = 0;
        let sinceGone = 0;
        const poll = () => {
            tries++;
            KeepAlert();
            if (state.updates === updatesBefore) {
                if (tries < 100) {
                    $.Schedule(0.1, poll);
                    return;
                }
                state.busy = false;
                Reveal();
                state.note = T('csgc_upgrade_timeout', 'No answer from the GC, see cs2_gc\\gc_log.txt');
                Render();
                return;
            }
            const present = {};
            InventoryItems().forEach((id) => { present[id] = true; });
            if (present[deposits[0]]) {
                if (++sinceUpdate < 10) {
                    $.Schedule(0.1, poll);
                    return;
                }
                state.busy = false;
                Reveal();
                state.deposits = state.deposits.filter((id) => present[id] && CanDeposit(id));
                state.note = T('csgc_upgrade_refused', 'The GC refused, see cs2_gc\\gc_log.txt');
                Render();
                return;
            }
            let won = '';
            const now = InventoryAPI.GetUnacknowledgeItemsCount();
            for (let i = 0; i < now; i++) {
                const id = InventoryAPI.GetUnacknowledgeItemByIndex(i);
                if (!before[id])
                    won = id;
            }
            // the won skin comes in its own message right after the burnt ones: a moment more
            // before it counts as lost
            if (!won && ++sinceGone < 4) {
                $.Schedule(0.1, poll);
                return;
            }
            state.deposits = [];
            Spin(!!won, () => {
                state.busy = false;
                state.result = { won: won };
                Render();
                $.Schedule(REVEAL_DELAY, Reveal);
            });
        };
        $.Schedule(0.1, poll);
    }
    function Show() {
        if (!ui)
            Build(panel);
        Render();
    }
    function Install(contextPanel, inventoryMain, navigate) {
        const tabs = contextPanel.FindChildInLayoutFile('id-navbar-tabs-catagory-btns-container');
        const content = inventoryMain.FindChildInLayoutFile('InventoryMenuContent');
        if (!tabs || !content) {
            $.Msg('[csgc upgrader] no inventory tabs to join\n');
            return;
        }
        if (content.FindChildInLayoutFile('csgc_upgrader'))
            return;
        const button = $.CreatePanel('RadioButton', tabs, 'csgc_upgrader_tab', { class: 'content-navbar__tabs__btn', group: 'inv-top-nav' });
        $.CreatePanel('Label', button, '', { text: '#csgc_upgrade_title' });
        panel = $.CreatePanel('Panel', content, 'csgc_upgrader', { class: 'inv-category' });
        // like the game's own tabs: once faded out it stops being visible, or it would take the clicks
        $.RegisterEventHandler('PropertyTransitionEnd', panel, (panelName, propertyName) => {
            if (propertyName === 'opacity' && panel.visible === true && panel.BIsTransparent()) {
                panel.visible = false;
                return true;
            }
            return false;
        });
        const open = () => {
            navigate('csgc_upgrader');
            try {
                Show();
            }
            catch (e) {
                $.Msg('[csgc upgrader] ' + e + '\n' + e.stack + '\n');
            }
        };
        button.SetPanelEvent('onactivate', open);
        inventoryMain.Data().csgcUpgrade = (id) => {
            if (!state.busy && state.deposits.indexOf(id) < 0 && state.deposits.length < MAX_DEPOSITS) {
                const reason = Reject(id);
                if (!reason) {
                    state.deposits.push(id);
                    state.result = null;
                    state.picking = false;
                    state.note = '';
                }
                else {
                    state.note = reason === 'price' ? T('csgc_upgrade_no_price', 'This skin has no market price in its wear, it can\'t go in')
                        : T('csgc_upgrade_cant', 'This item can\'t go in the upgrader');
                }
            }
            button.checked = true;
            open();
        };
        $.RegisterForUnhandledEvent('PanoramaComponent_MyPersona_InventoryUpdated', () => {
            state.updates++;
            prices = {};
            if (state.hold) {
                // the main menu sets the count on this same update, before or after this
                KeepAlert();
                $.Schedule(0.0, KeepAlert);
            }
            if (!ui || state.busy || !panel.BHasClass('Active'))
                return;
            if (state.deposits.length) {
                const present = {};
                InventoryItems().forEach((id) => { present[id] = true; });
                state.deposits = state.deposits.filter((id) => present[id]);
            }
            Render();
        });
    }
    return { Install: Install };
})();

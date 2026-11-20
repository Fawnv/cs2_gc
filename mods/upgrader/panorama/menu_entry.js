{
            name: 'csgc_upgrade',
            style: (id) => 'TopSeparator',
            AvailableForItem: (id) => {
                if (InventoryAPI.IsFauxItemID(id) || InventoryAPI.IsRental(id))
                    return false;
                const slot = InventoryAPI.GetDefaultSlot(id);
                if (!slot || slot === 'c4' || ItemInfo.IsEquippalbleButNotAWeapon(id))
                    return false;
                // a stock gun with a charm or a name is a real item too, but normal quality. Knives
                // and gloves can go in too: the window looks up the price
                if (Number(InventoryAPI.GetItemQuality(id)) === 0)
                    return false;
                return Number(InventoryAPI.GetItemRarity(id)) >= 1;
            },
            OnSelected: (id) => {
                $.DispatchEvent('ContextMenuEvent', '');
                let root = $.GetContextPanel();
                while (root.GetParent())
                    root = root.GetParent();
                const main = root.FindChildTraverse('InventoryMain');
                if (main && main.Data().csgcUpgrade)
                    main.Data().csgcUpgrade(id);
                else
                    $.Msg('[csgc upgrader] the inventory tab is not there\n');
            }
        },
        
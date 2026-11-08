#include "stdafx.h"
#include "inventory.h"
#include "case_opening.h"
#include "config.h"
#include "gc_const.h"
#include "inventory_messages.h"
#include "inventory_modify.h"
#include "keyvalue.h"
#include "random.h"

#include "base_gcmessages.pb.h"
#include "cstrike15_gcmessages.pb.h"

static const std::string &InventoryFilePath()
{
    static const std::string path = Platform::DataPath("inventory.txt");
    return path;
}

// mikkotodo move
constexpr uint32_t SlotUneqip = 0xffff;

static AttributeDefIndex StickerAttributeForSlot(AttributeDefIndex defIndex, uint32_t slot);

// get the full item id of the casket item is in
static uint64_t GetCasketId(const Item &item)
{
    uint64_t low = item.GetAttributeValue<uint32_t>(AttributeDefIndex::CasketIdLow);
    uint64_t high = item.GetAttributeValue<uint32_t>(AttributeDefIndex::CasketIdHigh);
    return low ? ((high << 32) | low) : 0;
}

// helper, see ItemIdDefaultItemMask for more information
inline bool IsDefaultItemId(uint64_t itemId, uint32_t &defIndex, uint32_t &paintKitIndex)
{
    if ((itemId & ItemIdDefaultItemMask) == ItemIdDefaultItemMask)
    {
        defIndex = itemId & 0xffff;
        paintKitIndex = (itemId >> 16) & 0xffff;
        return true;
    }

    return false;
}

inline void EquipSlotToProto(
    CSOEconEquipSlot &proto,
    const std::pair<uint32_t, uint32_t> &key,
    const EquipSlotValue *value,
    uint32_t accountId)
{
    proto.set_account_id(accountId);
    proto.set_class_id(key.first);
    proto.set_slot_id(key.second);

    if (value)
    {
        proto.set_item_id(value->itemId);
        proto.set_item_definition(value->defIndex);
    }
}

Inventory::Inventory(uint64_t steamId)
    : m_steamId{ steamId }
    , m_editor{ m_itemSchema }
{
    ReadFromFile();
}

Inventory::~Inventory()
{
    WriteToFile();
}

bool Inventory::Update(InventoryChangeMessages &changeMessages)
{
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (now >= m_nextRentalCheck)
    {
        m_nextRentalCheck = now + 10;
        if (ExpireRentals(now, changeMessages))
        {
            return true;
        }
    }

    EditorChanges changes;
    const bool hasChanges = m_editor.GetChanges(changes);

    if (m_editor.WantsFullInventory())
    {
        SendFullInventoryToEditor();
    }

    if (!hasChanges)
    {
        return false;
    }

    // FIXME: this will cause the changes to be sent back to the editor!!! ideally we would not do this,
    // but we have cases like removal of caskets or casketed items that will cause more item modifications
    InventoryModify modify;

    // not const ref since we move the item descs
    for (EditorItem &editorItem : changes.items)
    {
        if (editorItem.type == EditorItemChangeType::Destroyed)
        {
            Item *item = FindItem(ComposeItemId(AccountId(), editorItem.lowId));
            if (item)
            {
                DestroyItem(modify, item);
            }
            else
            {
                assert(false);
            }

            continue;
        }

        if (editorItem.type == EditorItemChangeType::Created)
        {
            // editorItem.lowId is NOT the item id we should use!!!!
            // it's an opaque "item request id" from the editor, we
            // need to send the actual item id back now that we've determined it
            Item &item = CreateItem(modify, std::move(editorItem.desc));
            m_editor.SendItemId(editorItem.lowId, item.LowId());
            continue;
        }

        // modification
        Item *item = FindItem(ComposeItemId(AccountId(), editorItem.lowId));
        if (!item)
        {
            assert(false);
            continue;
        }

        // no StatTrak counter on a storage unit: the client crashes on one (FixStatTrakStorageUnits)
        if (editorItem.desc.defIndex == ItemDefIndex::Casket)
        {
            std::erase_if(editorItem.desc.attributes, [](const ItemAttribute &attribute) {
                return attribute.DefIndex() == AttributeDefIndex::KillEater || attribute.DefIndex() == AttributeDefIndex::KillEaterScoreType;
            });
        }

        modify.UpdateFromDesc(*item, std::move(editorItem.desc));
    }

    changeMessages = FinishChanges(modify);
    return true;
}

void Inventory::SendFullInventoryToEditor()
{
    EditorChanges changes;

    m_editorVisibleItems.clear();

    for (const auto &pair : m_items)
    {
        if (GetCasketId(pair.second))
        {
            // don't expose casketed items to the editor
            continue;
        }

        EditorItem &item = changes.items.emplace_back();
        item.type = EditorItemChangeType::Created;
        item.lowId = pair.second.LowId();
        item.desc = pair.second.GetDesc();

        m_editorVisibleItems.insert(item.lowId);
    }

    m_editor.SendChanges(changes, true);
}

void Inventory::SendChangesToEditor(const InventoryModify &modify)
{
    EditorChanges changes;

    for (const auto &[lowId, change] : modify.m_itemChanges)
    {
        const Item *source = FindItem(ComposeItemId(AccountId(), lowId));
        if (!source || GetCasketId(*source))
        {
            if (m_editorVisibleItems.erase(lowId))
            {
                EditorItem &dest = changes.items.emplace_back();
                dest.lowId = lowId;
                dest.type = EditorItemChangeType::Destroyed;
            }

            continue;
        }

        bool inserted = m_editorVisibleItems.insert(lowId).second;

        EditorItem &dest = changes.items.emplace_back();
        dest.lowId = lowId;
        dest.type = inserted ? EditorItemChangeType::Created : EditorItemChangeType::Modified;
        dest.desc = source->GetDesc();
    }

    m_editor.SendChanges(changes, false);
}

InventoryChangeMessages Inventory::FinishChanges(
    InventoryModify &modify,
    std::optional<EGCItemCustomizationNotification> customizationType,
    std::initializer_list<uint64_t> customizationItemIds)
{
    InventoryChangeMessages messages;

    if (modify.HasChanges())
    {
        m_version++;
    }

    BuildSOCacheMessages(modify, m_clientCache, messages.client, false);
    BuildSOCacheMessages(modify, m_gameServerCache, messages.gameServer, true);

    if (customizationType.has_value())
    {
        messages.notification.mutable_item_id()->Assign(customizationItemIds.begin(), customizationItemIds.end());
        messages.notification.set_request(*customizationType);
    }

    if (modify.HasChanges())
    {
        SendChangesToEditor(modify);
        WriteToFile();
    }

    assert(!modify.m_finished);
    modify.m_finished = true;

    return messages;
}

void Inventory::BuildSOCacheMessages(
    const InventoryModify &modify,
    SOCacheState &state,
    SOCacheMessages &messages,
    bool server)
{
    if (!state.subscribed)
    {
        return;
    }

    for (auto [lowId, change] : modify.m_itemChanges)
    {
        uint64_t itemId = ComposeItemId(AccountId(), lowId);

        if (change == ItemChangeType::Destroyed)
        {
            assert(!FindItem(itemId));
            if (state.items.erase(itemId))
            {
                CSOEconItem econItem;
                econItem.set_id(itemId);
                ToSingleObject(messages.destroyed.emplace_back(), SOTypeItem, econItem);
            }

            continue;
        }

        Item *item = FindItem(itemId);
        if (!item)
        {
            assert(false);
            continue;
        }

        // keep items in the server socache even if they get unequipped later
        if (server && !item->HasEquips() && !state.items.contains(itemId))
        {
            continue;
        }

        CSOEconItem econItem;
        item->ToCSOEconItem(econItem, AccountId(), m_itemSchema, m_oldProtocol);

        if (state.items.insert(itemId).second)
        {
            ToSingleObject(messages.created.emplace_back(), SOTypeItem, econItem);
        }
        else
        {
            AddToMultipleObjects(messages.updated, SOTypeItem, econItem);
        }
    }

    // CS2 reads the loadout from CSOEconEquipSlot objects
    BuildEquipSlotMessages(state, messages);

    // account objects only live in the client's cache
    if (!server && modify.m_accountChanges)
    {
        BuildAccountObjects(messages, modify.m_accountChanges);
    }
}

void Inventory::AccountKeychainCharges(CSOAccountKeychainRemoveToolCharges &out) const
{
    out.set_charges(m_account.keychainRemoveCharges);
}

void Inventory::AccountXpShop(CSOAccountXpShop &out) const
{
    out.set_generation_time(m_account.xpShopGeneration);
    out.set_redeemable_balance(m_account.xpShopBalance);
    for (uint32_t xp : m_account.xpShopTracks)
    {
        out.add_xp_tracks(xp);
    }
}

void Inventory::CollectEquipSlots(EquipSlotMap &out) const
{
    out.clear();

    for (const auto &[itemId, item] : m_items)
    {
        for (const ItemEquip &equip : item.GetDesc().equips)
        {
            out[{ equip.Class(), equip.Slot() }] = EquipSlotValue{ itemId, FromEnum(item.DefIndex()) };
        }
    }

    for (const DefaultEquip &equip : m_defaultEquips)
    {
        out[{ equip.classId, equip.slotId }] = EquipSlotValue{ ItemIdDefaultItemMask | equip.defIndex, equip.defIndex };
    }
}

void Inventory::BuildEquipSlotMessages(SOCacheState &state, SOCacheMessages &messages)
{
    EquipSlotMap current;
    CollectEquipSlots(current);

    for (const auto &[key, value] : current)
    {
        auto it = state.equipSlots.find(key);
        if (it == state.equipSlots.end())
        {
            CSOEconEquipSlot proto;
            EquipSlotToProto(proto, key, &value, AccountId());
            ToSingleObject(messages.created.emplace_back(), SOTypeEquipSlot, proto);
        }
        else if (!(it->second == value))
        {
            CSOEconEquipSlot proto;
            EquipSlotToProto(proto, key, &value, AccountId());
            AddToMultipleObjects(messages.updated, SOTypeEquipSlot, proto);
        }
    }

    for (const auto &[key, value] : state.equipSlots)
    {
        if (!current.contains(key))
        {
            CSOEconEquipSlot proto;
            EquipSlotToProto(proto, key, nullptr, AccountId());
            ToSingleObject(messages.destroyed.emplace_back(), SOTypeEquipSlot, proto);
        }
    }

    state.equipSlots = std::move(current);
}

void Inventory::AddToMultipleObjects(CMsgSOMultipleObjects &message, SOTypeId type, const google::protobuf::MessageLite &object)
{
    if (!message.has_version())
    {
        assert(!message.has_owner_soid());
        message.set_version(m_version);
        message.mutable_owner_soid()->set_type(SoIdTypeSteamId);
        message.mutable_owner_soid()->set_id(m_steamId);
    }
    else
    {
        assert(message.has_owner_soid());
    }

    CMsgSOMultipleObjects_SingleObject *single = message.add_objects_modified();
    single->set_type_id(type);
    object.SerializeToString(single->mutable_object_data());
}

void Inventory::ToSingleObject(CMsgSOSingleObject &message, SOTypeId type, const google::protobuf::MessageLite &object)
{
    assert(!message.has_owner_soid());
    assert(!message.has_version());
    assert(!message.has_type_id());
    assert(!message.has_object_data());

    message.set_version(m_version);
    message.mutable_owner_soid()->set_type(SoIdTypeSteamId);
    message.mutable_owner_soid()->set_id(m_steamId);

    message.set_type_id(type);
    object.SerializeToString(message.mutable_object_data());
}

uint32_t Inventory::AccountId() const
{
    return m_steamId & 0xffffffff;
}

uint32_t Inventory::GetLowItemId(uint32_t lowItemId)
{
    // Players fuck up their inventory files constantly and end up with item id collisions...
    // This doesn't return until the item id is unique for this session, try with the provided
    // item id first, if it's invalid or already in use increment it

    if (!lowItemId)
    {
        m_lastLowItemId++;
        lowItemId = m_lastLowItemId;
    }

    for (;; lowItemId++)
    {
        if (!lowItemId)
        {
            // item ids should be strictly monotonically increasing, but
            // if the situation gets this cooked then we need to reset
            m_lastLowItemId = 1;
            lowItemId = m_lastLowItemId;
        }

        if (m_items.count(ComposeItemId(AccountId(), lowItemId)))
        {
            // item id collision
            continue;
        }

        if (lowItemId > m_lastLowItemId)
        {
            m_lastLowItemId = lowItemId;
        }

        // ok
        return lowItemId;
    }
}

void Inventory::CreateItem(uint32_t lowId, const KeyValue &kv)
{
    lowId = GetLowItemId(lowId);
    uint64_t itemId = ComposeItemId(AccountId(), lowId);

    // this will succeed, GetLowItemId confirmed there are no collisions
    [[maybe_unused]] auto [it, inserted] = m_items.try_emplace(itemId, lowId, AccountId(), kv, m_itemSchema);
    assert(inserted);
}

Item &Inventory::CreateItem(InventoryModify &modify, ItemDesc &&desc)
{
    uint32_t lowId = GetLowItemId(0);
    uint64_t itemId = ComposeItemId(AccountId(), lowId);

    // this will succeed, GetLowItemId confirmed there are no collisions
    auto [it, inserted] = m_items.try_emplace(itemId, lowId, std::move(desc));
    assert(inserted);

    modify.MarkItemCreated(lowId);

    // new item: if a cached icon exists, it's because
    // of item id reuse and the icon is likely wrong
    RemoveCachedIcon(lowId);

    return it->second;
}

void Inventory::ReadFromFile()
{
    KeyValueRead inventoryRead{ InventoryFilePath().c_str() };
    if (!inventoryRead.Good())
    {
        return;
    }

    const KeyValue &inventoryKey = inventoryRead.Root();

    const KeyValue *itemsKey = inventoryKey.GetSubkey("items");
    if (itemsKey)
    {
        m_items.reserve(itemsKey->Subkeys().size());

        for (const KeyValue &itemKey : itemsKey->Subkeys())
        {
            uint32_t lowItemId = FromString<uint32_t>(itemKey.Name());
            CreateItem(lowItemId, itemKey);
        }
    }

    ReadAccount(inventoryKey.GetSubkey("account"));

    const KeyValue *defaultEquipsKey = inventoryKey.GetSubkey("default_equips");
    if (defaultEquipsKey)
    {
        m_defaultEquips.reserve(defaultEquipsKey->Subkeys().size());

        for (const KeyValue &defaultEquipKey : defaultEquipsKey->Subkeys())
        {
            uint32_t defIndex = FromString<uint32_t>(defaultEquipKey.Name());
            uint32_t classId = defaultEquipKey.GetNumber<uint32_t>("class_id");
            uint32_t slotId = defaultEquipKey.GetNumber<uint32_t>("slot_id");
            m_defaultEquips.emplace_back(defIndex, classId, slotId);
        }
    }

    FixPaintedQualities();
    FixStatTrakStorageUnits();
    RefreshCarePackage();
}

void Inventory::WriteToFile() const
{
    Platform::Print("Writing inventory to {} ({} items, {} default equips)\n",
        InventoryFilePath(),
        m_items.size(),
        m_defaultEquips.size());

    FILE *f = Platform::OpenFile(InventoryFilePath(), "wb");
    if (!f)
    {
        Platform::Print("Could not open {} for writing\n", InventoryFilePath());
        return;
    }

    KeyValueWrite write{ f };

    if (!m_items.empty())
    {
        write.BeginBlock("items");

        for (const auto &pair : m_items)
        {
            write.BeginBlock(std::to_string(pair.second.LowId()));
            pair.second.ToKeyValue(write);
            write.EndBlock();
        }

        write.EndBlock();
    }

    WriteAccount(write);

    if (!m_defaultEquips.empty())
    {
        write.BeginBlock("default_equips");

        for (const DefaultEquip &defaultEquip : m_defaultEquips)
        {
            write.BeginBlock(std::to_string(defaultEquip.defIndex));
            write.AddNumber("class_id", defaultEquip.classId);
            write.AddNumber("slot_id", defaultEquip.slotId);
            write.EndBlock();
        }

        write.EndBlock();
    }

    bool writeFailed = ferror(f) != 0;
    int closeError = (fclose(f) == EOF) ? errno : 0;

    if (writeFailed)
    {
        Platform::Print("Writing {} failed\n", InventoryFilePath());
    }

    if (closeError != 0)
    {
        Platform::Print("Closing {} failed: {} ({})\n",
            InventoryFilePath(),
            strerror(closeError),
            closeError);
    }
}

void Inventory::BuildCacheSubscription(CMsgSOCacheSubscribed &message, int level, bool server)
{
    SOCacheState &state = server ? m_gameServerCache : m_clientCache;
    state.subscribed = true;
    state.items.clear();

    message.set_version(m_version);
    message.mutable_owner_soid()->set_type(SoIdTypeSteamId);
    message.mutable_owner_soid()->set_id(m_steamId);

    {
        CMsgSOCacheSubscribed_SubscribedType *object = message.add_objects();
        object->set_type_id(SOTypeItem);

        for (const auto &pair : m_items)
        {
            if (server && !pair.second.HasEquips())
            {
                continue;
            }

            CSOEconItem serialized;
            pair.second.ToCSOEconItem(serialized, AccountId(), m_itemSchema, m_oldProtocol);
            serialized.SerializeToString(object->add_object_data());
            state.items.insert(pair.first);
        }
    }

    {
        CSOPersonaDataPublic personaData;
        personaData.set_player_level(m_account.level ? m_account.level : level);
        personaData.set_elevated_state(true);

        CMsgSOCacheSubscribed_SubscribedType *object = message.add_objects();
        object->set_type_id(SOTypePersonaDataPublic);
        personaData.SerializeToString(object->add_object_data());
    }

    if (!server)
    {
        CSOEconGameAccountClient accountClient;
        accountClient.set_additional_backpack_slots(0);
        accountClient.set_bonus_xp_timestamp_refresh(static_cast<uint32_t>(time(nullptr)));
        accountClient.set_bonus_xp_usedflags(16); // caught cheater lobbies, overwatch bonus etc
        accountClient.set_elevated_state(ElevatedStatePrime);
        accountClient.set_elevated_timestamp(ElevatedStatePrime); // is this actually 5????

        CMsgSOCacheSubscribed_SubscribedType *object = message.add_objects();
        object->set_type_id(SOTypeGameAccountClient);
        accountClient.SerializeToString(object->add_object_data());

        // charm removals left (InventoryAPI.GetCacheTypeElementFieldByIndex('KeychainRemoveToolCharges', 0, 'charges'))
        CSOAccountKeychainRemoveToolCharges charges;
        AccountKeychainCharges(charges);
        object = message.add_objects();
        object->set_type_id(SOTypeAccountKeychainRemoveToolCharges);
        charges.SerializeToString(object->add_object_data());

        // Armory stars and passes (GetCacheTypeElementJSOByIndex('XpShop', 0))
        CSOAccountXpShop xpShop;
        AccountXpShop(xpShop);
        object = message.add_objects();
        object->set_type_id(SOTypeAccountXpShop);
        xpShop.SerializeToString(object->add_object_data());

        // weekly care package (GetCacheTypeElementJSOByIndex('PersonalStore', 0)), only once earned
        if (m_account.storeGeneration)
        {
            CSOAccountItemPersonalStore store;
            AccountPersonalStore(store);
            object = message.add_objects();
            object->set_type_id(SOTypeAccountItemPersonalStore);
            store.SerializeToString(object->add_object_data());
        }

        // terminals: offers seen and collection xp claimed, one object per terminal def
        if (!m_account.volatileHistory.empty())
        {
            CMsgSOCacheSubscribed_SubscribedType *offers = message.add_objects();
            offers->set_type_id(SOTypeVolatileItemOffer);
            CMsgSOCacheSubscribed_SubscribedType *claimed = message.add_objects();
            claimed->set_type_id(SOTypeVolatileItemClaimedRewards);
            for (const auto &[defIndex, history] : m_account.volatileHistory)
            {
                CSOVolatileItemOffer offer;
                offer.set_defidx(defIndex);
                for (auto [fauxId, when] : history.seen)
                {
                    offer.add_faux_itemid(fauxId);
                    offer.add_generation_time(when);
                }
                offer.SerializeToString(offers->add_object_data());

                CSOVolatileItemClaimedRewards rewards;
                rewards.set_defidx(defIndex);
                for (auto [reward, when] : history.claimed)
                {
                    rewards.add_reward(reward);
                    rewards.add_generation_time(when);
                }
                rewards.SerializeToString(claimed->add_object_data());
            }
        }
    }

    {
        // CS2 loadout
        CMsgSOCacheSubscribed_SubscribedType *object = message.add_objects();
        object->set_type_id(SOTypeEquipSlot);

        CollectEquipSlots(state.equipSlots);

        for (const auto &[key, value] : state.equipSlots)
        {
            CSOEconEquipSlot proto;
            EquipSlotToProto(proto, key, &value, AccountId());
            proto.SerializeToString(object->add_object_data());
        }
    }
}

// yes this function is inefficent!!! but i think that makes it more clear
// also i think this is the way valve gc does it???? can't remember
InventoryChangeMessages Inventory::EquipItem(uint64_t itemId, uint32_t classId, uint32_t slotId)
{
    InventoryModify modify;
    if (!EquipItem(modify, itemId, classId, slotId))
    {
        assert(false);
        return {};
    }

    return FinishChanges(modify);
}

bool Inventory::EquipItem(InventoryModify &modify, uint64_t itemId, uint32_t classId, uint32_t slotId)
{
    if (slotId == SlotUneqip)
    {
        // unequipping a specific item from all slots
        return UnequipItem(modify, itemId);
    }

    // mikkotodo cleanup, old junk
    assert(itemId != UINT64_MAX); // probably an old csgo thing

    if (!itemId)
    {
        // unequip from this slot, itemid not provided so nothing gets equipped
        UnequipItem(modify, classId, slotId);
        return true;
    }

    uint32_t defIndex, paintKitIndex;
    if (IsDefaultItemId(itemId, defIndex, paintKitIndex))
    {
        // if an item is equipped in this slot, unequip it first
        UnequipItem(modify, classId, slotId);

        Platform::Print("EquipItem def {} class {} slot {}\n", defIndex, classId, slotId);

        DefaultEquip &defaultEquip = m_defaultEquips.emplace_back(defIndex, classId, slotId);
        modify.MarkDefaultEquipChanged(defaultEquip);

        return true;
    }

    Item *item = FindItem(itemId);
    if (!item)
    {
        Platform::Print("EquipItem: no such item {}!!!!\n", itemId);
        return false; // didn't modify anything
    }

    // if an item is equipped in this slot, unequip it first
    UnequipItem(modify, classId, slotId);

    Platform::Print("EquipItem {} class {} slot {}\n", itemId, classId,
        slotId);

    modify.AddItemEquip(*item, classId, slotId);

    return true;
}

InventoryChangeMessages Inventory::AdjustEquipSlots(const CMsgAdjustEquipSlots &message)
{
    InventoryModify modify;

    for (const CMsgAdjustEquipSlot &slot : message.slots())
    {
        // item id 0 clears the slot (handled by EquipItem)
        if (!EquipItem(modify, slot.item_id(), slot.class_id(), slot.slot_id()))
        {
            Platform::Print("AdjustEquipSlots: could not equip {} to class {} slot {}\n",
                slot.item_id(), slot.class_id(), slot.slot_id());
        }
    }

    return FinishChanges(modify);
}

InventoryChangeMessages Inventory::RemoveItem(uint64_t itemId)
{
    InventoryModify modify;
    if (!DestroyItemById(modify, itemId))
    {
        assert(false);
        return {};
    }

    return FinishChanges(modify);
}

InventoryChangeMessages Inventory::UseItem(uint64_t itemId)
{
    InventoryModify modify;
    Item *item = FindItem(itemId);
    if (!item)
    {
        assert(false);
        return {};
    }

    if (item->DefIndex() == ItemDefIndex::XpShopTicket)
    {
        return UseXpShopTicket(*item);
    }

    if (item->DefIndex() == ItemDefIndex::KeychainRemoveToolPack)
    {
        return UseKeychainRemoveToolPack(*item);
    }

    if (std::string_view name = m_itemSchema.ItemDefName(item->DefIndex()); name.starts_with("tournament_pass_"))
    {
        return UseTournamentPass(*item, name);
    }

    if (item->DefIndex() != ItemDefIndex::Spray)
    {
        Platform::Print("UseItem: don't know how to use item {} (def {})\n", itemId, FromEnum(item->DefIndex()));
        return {};
    }

    // create an unsealed spray based on the sealed one
    ItemDesc temp = item->GetDesc();
    temp.defIndex = ItemDefIndex::SprayPaint;
    Item &unsealed = CreateItem(modify, std::move(temp));
    uint64_t unsealedId = unsealed.FullIdFor(AccountId());

    // remove the sealed spray from our inventory
    DestroyItem(modify, item);

    // equip the new spray, this will also unequip the old one if we had one
    EquipItem(modify, unsealedId, 0, LoadoutSlotGraffiti);

    // remove this to have unlimited sprays
    modify.SetItemAttribute(unsealed, AttributeDefIndex::SpraysRemaining, 50u);

    return FinishChanges(modify, k_EGCItemCustomizationNotification_GraffitiUnseal, { unsealedId });
}

// major viewer passes (InventoryAPI.UseTool(pass, ''), fan_token prefab): tournament_pass_<event> turns into
// the event's coin (tournament_journal_<event>), _pack also brings 3 souvenir tokens (_charge), and a token
// is traded for one of the event's souvenir packages (crate_<event>_promo_*)
InventoryChangeMessages Inventory::UseTournamentPass(Item &pass, std::string_view name)
{
    std::string_view rest = name.substr(std::string_view{ "tournament_pass_" }.size());
    bool pack = rest.ends_with("_pack");
    bool charge = rest.ends_with("_charge");
    std::string event{ rest.substr(0, rest.find('_')) };

    std::vector<ItemDefIndex> rewards;
    if (charge)
    {
        std::vector<ItemDefIndex> packages;
        for (const char *map : { "inferno", "mirage", "anubis", "overpass", "dust2", "ancient", "nuke", "train", "vertigo" })
        {
            if (auto defIndex = m_itemSchema.ItemDefIndexByName(std::format("crate_{}_promo_de_{}", event, map)))
            {
                packages.push_back(*defIndex);
            }
        }
        if (!packages.empty())
        {
            rewards.push_back(Random::Select(std::span<ItemDefIndex>{ packages }));
        }
    }
    else if (auto coin = m_itemSchema.ItemDefIndexByName("tournament_journal_" + event))
    {
        rewards.push_back(*coin);
        if (pack)
        {
            if (auto token = m_itemSchema.ItemDefIndexByName(std::format("tournament_pass_{}_charge", event)))
            {
                rewards.insert(rewards.end(), 3, *token);
            }
        }
    }

    if (rewards.empty())
    {
        Platform::Print("UseItem: nothing known to come out of {}\n", name);
        return {};
    }

    InventoryModify modify;
    uint64_t firstId = 0;
    for (ItemDefIndex defIndex : rewards)
    {
        ItemDesc desc{};
        if (m_itemSchema.GetItemDesc(defIndex, ItemOriginPurchased, UnacknowledgedPurchased, desc))
        {
            Item &item = CreateItem(modify, std::move(desc));
            firstId = firstId ? firstId : item.FullIdFor(AccountId());
        }
    }
    DestroyItem(modify, &pass);

    Platform::Print("UseItem: {} -> {} items\n", name, rewards.size());
    return FinishChanges(modify, k_EGCItemCustomizationNotification_ActivateFanToken, { firstId });
}

// Armory pass activation (InventoryAPI.UseTool(pass, '')): a new track with all of its stars
InventoryChangeMessages Inventory::UseXpShopTicket(Item &ticket)
{
    if (m_account.xpShopTracks.size() >= XpShopMaxTracks)
    {
        Platform::Print("Armory: already {} passes active, finish (spend) them first\n", m_account.xpShopTracks.size());
        return {};
    }

    uint64_t ticketId = ticket.FullIdFor(AccountId());

    InventoryModify modify;
    m_account.xpShopTracks.push_back(XpShopStarsPerPass * XpShopStarXp);
    m_account.xpShopBalance += XpShopStarsPerPass;
    m_account.xpShopGeneration = static_cast<uint32_t>(time(nullptr));
    modify.MarkAccountChanged(AccountChangeXpShop);
    DestroyItem(modify, &ticket);

    Platform::Print("Armory: pass activated, {} stars, {} passes\n", m_account.xpShopBalance, m_account.xpShopTracks.size());
    return FinishChanges(modify, k_EGCItemCustomizationNotification_XpShopUseTicket, { ticketId });
}

InventoryChangeMessages Inventory::UseKeychainRemoveToolPack(Item &pack)
{
    InventoryModify modify;
    m_account.keychainRemoveCharges += KeychainRemoveChargesPerPack;
    modify.MarkAccountChanged(AccountChangeKeychainCharges);
    DestroyItem(modify, &pack);

    Platform::Print("Charm removals: {} left\n", m_account.keychainRemoveCharges);
    return FinishChanges(modify);
}

InventoryChangeMessages Inventory::RedeemXpShopReward(uint32_t campaign, uint32_t redeemId, uint32_t expectedCost, uint64_t &createdId)
{
    createdId = 0;

    const ItemSchema::XpShopEntry *entry = m_itemSchema.GetXpShopEntry(campaign, redeemId);
    if (!entry)
    {
        Platform::Print("Armory: no shop entry {} in campaign {}\n", redeemId, campaign);
        return {};
    }

    if (expectedCost && expectedCost != entry->points)
    {
        Platform::Print("Armory: {} costs {} stars, the client expected {}\n", entry->itemName, entry->points, expectedCost);
    }

    if (m_account.xpShopBalance < entry->points)
    {
        Platform::Print("Armory: {} costs {} stars, only {} left\n", entry->itemName, entry->points, m_account.xpShopBalance);
        return {};
    }

    // "lootlist:<list>" rolls one item like a case would, anything else is an item definition name
    std::vector<ItemDesc> rewards;
    constexpr std::string_view LootListPrefix = "lootlist:";
    if (entry->itemName.starts_with(LootListPrefix))
    {
        const LootList *lootList = m_itemSchema.GetLootList(std::string_view{ entry->itemName }.substr(LootListPrefix.size()));
        CaseOpening caseOpening{ m_itemSchema };
        if (!lootList || !caseOpening.SelectItemsFromLootList(*lootList, false, ItemOriginQuestDrop, UnacknowledgedEarned, rewards))
        {
            Platform::Print("Armory: can't roll {}\n", entry->itemName);
            return {};
        }
    }
    else
    {
        std::optional<ItemDefIndex> defIndex = m_itemSchema.ItemDefIndexByName(entry->itemName);
        ItemDesc desc{};
        if (!defIndex || !m_itemSchema.GetItemDesc(*defIndex, ItemOriginQuestDrop, UnacknowledgedEarned, desc))
        {
            Platform::Print("Armory: no item named {}\n", entry->itemName);
            return {};
        }
        rewards.push_back(std::move(desc));
    }

    if (rewards.empty())
    {
        return {};
    }

    InventoryModify modify;
    for (ItemDesc &desc : rewards)
    {
        Item &item = CreateItem(modify, std::move(desc));
        createdId = createdId ? createdId : item.FullIdFor(AccountId());
    }

    m_account.xpShopBalance -= entry->points;
    m_account.xpShopGeneration = static_cast<uint32_t>(time(nullptr));
    modify.MarkAccountChanged(AccountChangeXpShop);

    Platform::Print("Armory: {} for {} stars, {} left\n", entry->itemName, entry->points, m_account.xpShopBalance);
    return FinishChanges(modify, k_EGCItemCustomizationNotification_ClientRedeemMissionReward, { createdId });
}

InventoryChangeMessages Inventory::AckXpShopTracks()
{
    constexpr uint32_t FullTrack = XpShopStarsPerPass * XpShopStarXp;
    size_t before = m_account.xpShopTracks.size();
    std::erase_if(m_account.xpShopTracks, [](uint32_t xp) { return xp >= FullTrack; });
    if (m_account.xpShopTracks.size() == before)
    {
        return {};
    }

    InventoryModify modify;
    modify.MarkAccountChanged(AccountChangeXpShop);
    return FinishChanges(modify, k_EGCItemCustomizationNotification_XpShopAckTracks, {});
}

bool Inventory::ItemHasKeychain(uint64_t itemId)
{
    Item *item = FindItem(itemId);
    return item && item->HasAttribute(AttributeDefIndex::KeychainId0);
}

bool Inventory::ItemHasSticker(uint64_t itemId, uint32_t slot)
{
    Item *item = FindItem(itemId);
    return item && item->HasAttribute(StickerAttributeForSlot(AttributeDefIndex::StickerId0, slot));
}

InventoryChangeMessages Inventory::RemoveKeychain(uint64_t itemId, uint32_t slot)
{
    Item *item = FindItem(itemId);
    if (!item || !item->HasAttribute(AttributeDefIndex::KeychainId0))
    {
        Platform::Print("RemoveKeychain: item {} has no charm\n", itemId);
        return {};
    }

    if (slot != 0)
    {
        Platform::Print("RemoveKeychain: charm slot {}? only slot 0 exists, removing that\n", slot);
    }

    if (!m_account.keychainRemoveCharges)
    {
        Platform::Print("RemoveKeychain: no charm removals left (buy a Remove Keychain Tool Pack, or raise "
                        "\"keychain_remove_charges\" in the \"account\" block of inventory.txt)\n");
        return {};
    }

    InventoryModify modify;

    // the charm goes back to the inventory with everything that made it unique
    uint32_t keychainId = item->GetAttributeValue<uint32_t>(AttributeDefIndex::KeychainId0);
    ItemDesc charm{};
    if (m_itemSchema.GetItemDesc(ItemDefIndex::Keychain, ItemOriginPurchased, UnacknowledgedInvalid, charm))
    {
        charm.rarity = m_itemSchema.KeychainRarity(keychainId);
        charm.attributes.emplace_back(AttributeDefIndex::KeychainId0, keychainId);
        for (AttributeDefIndex extra : { AttributeDefIndex::KeychainSeed0, AttributeDefIndex::KeychainHighlight0, AttributeDefIndex::KeychainSticker0 })
        {
            if (item->HasAttribute(extra))
            {
                charm.attributes.emplace_back(extra, item->GetAttributeValue<uint32_t>(extra));
            }
        }
        CreateItem(modify, std::move(charm));
    }

    m_account.keychainRemoveCharges--;
    modify.MarkAccountChanged(AccountChangeKeychainCharges);

    // a stock weapon that only existed to carry the charm goes away again (like a scraped sticker's)
    bool hasStickers = false;
    for (uint32_t s = 0; s < 6; s++)
    {
        hasStickers |= item->HasAttribute(StickerAttributeForSlot(AttributeDefIndex::StickerId0, s));
    }

    if (item->GetRarity() == Rarity::Default && !hasStickers && !item->HasAttribute(AttributeDefIndex::CustomName))
    {
        uint64_t fakeItemId = static_cast<uint64_t>(item->DefIndex()) | ItemIdDefaultItemMask;
        DestroyItem(modify, item);
        Platform::Print("RemoveKeychain: charm {} back in the inventory, {} removals left\n", keychainId, m_account.keychainRemoveCharges);
        return FinishChanges(modify, k_EGCItemCustomizationNotification_RemoveKeychain, { fakeItemId });
    }

    modify.RemoveItemAttributes(*item, { AttributeDefIndex::KeychainId0, AttributeDefIndex::KeychainOffsetX0,
                                           AttributeDefIndex::KeychainOffsetY0, AttributeDefIndex::KeychainOffsetZ0,
                                           AttributeDefIndex::KeychainSeed0, AttributeDefIndex::KeychainHighlight0,
                                           AttributeDefIndex::KeychainSticker0 });

    Platform::Print("RemoveKeychain: charm {} back in the inventory, {} removals left\n", keychainId, m_account.keychainRemoveCharges);
    return FinishChanges(modify, k_EGCItemCustomizationNotification_RemoveKeychain, { item->FullIdFor(AccountId()) });
}

InventoryChangeMessages Inventory::UnlockCrate(uint64_t crateId, uint64_t keyId)
{
    Item *crate = FindItem(crateId);
    if (!crate)
    {
        assert(false);
        return {};
    }

    // CASE OPENING
    CaseOpening caseOpening{ m_itemSchema };

    ItemDesc temp;
    if (!caseOpening.SelectItemFromCrate(crate->DefIndex(), temp))
    {
        assert(false);
        return {};
    }

    InventoryModify modify;
    Item &item = CreateItem(modify, std::move(temp));
    uint64_t itemId = item.FullIdFor(AccountId());

    if (GetConfig().m_destroyUsedItems)
    {
        assert(keyId != itemId);
        DestroyItem(modify, crate);
        DestroyItemById(modify, keyId);
    }

    return FinishChanges(modify, k_EGCItemCustomizationNotification_UnlockCrate, { itemId });
}

static void CollectRentalItems(const LootList &lootList, std::vector<const LootListItem *> &out)
{
    // rentals give the regular finishes only, the rare special items (knives, gloves) stay random
    if (lootList.isUnusual)
    {
        return;
    }

    for (const LootList *subList : lootList.subLists)
    {
        CollectRentalItems(*subList, out);
    }

    for (const LootListItem &item : lootList.items)
    {
        if (item.CaseRarity() != Rarity::Unusual)
        {
            out.push_back(&item);
        }
    }
}

InventoryChangeMessages Inventory::RentCrate(uint64_t crateId, uint64_t keyId)
{
    Item *crate = FindItem(crateId);
    if (!crate)
    {
        assert(false);
        return {};
    }

    const LootList *lootList = m_itemSchema.GetCrateLootList(crate->DefIndex());
    if (!lootList)
    {
        Platform::Print("RentCrate: no loot list for crate {}\n", FromEnum(crate->DefIndex()));
        return {};
    }

    std::vector<const LootListItem *> entries;
    CollectRentalItems(*lootList, entries);

    constexpr uint32_t RentalSeconds = 7 * 24 * 60 * 60;
    uint32_t expiration = static_cast<uint32_t>(time(nullptr)) + RentalSeconds;

    InventoryModify modify;
    std::vector<uint64_t> itemIds;

    for (const LootListItem *entry : entries)
    {
        ItemDesc desc{};
        if (!m_itemSchema.ItemDescForLootListItem(*entry, false, ItemOriginCrate, UnacknowledgedFoundInCrate, desc))
        {
            continue;
        }

        desc.attributes.emplace_back(AttributeDefIndex::IsRental, 1u);
        desc.attributes.emplace_back(AttributeDefIndex::ExpirationDate, expiration);

        Item &item = CreateItem(modify, std::move(desc));
        itemIds.push_back(item.FullIdFor(AccountId()));
    }

    Platform::Print("RentCrate: {} items from crate {} until {}\n", itemIds.size(), FromEnum(crate->DefIndex()), expiration);

    if (GetConfig().m_destroyUsedItems)
    {
        DestroyItem(modify, crate);
        if (keyId)
        {
            DestroyItemById(modify, keyId);
        }
    }

    InventoryChangeMessages messages = FinishChanges(modify);
    messages.notification.mutable_item_id()->Assign(itemIds.begin(), itemIds.end());
    messages.notification.set_request(k_EGCItemCustomizationNotification_UnlockCrate);
    return messages;
}

InventoryChangeMessages Inventory::TradeUp(uint32_t recipe, std::span<const uint64_t> itemIds, uint64_t &createdId)
{
    createdId = 0;

    uint32_t tier = recipe % 10;
    bool statTrak = (recipe >= 10);
    if (recipe > 15 || tier > 5)
    {
        Platform::Print("TradeUp: unknown recipe {}\n", recipe);
        return {};
    }

    bool unusualOutput = (tier == 5);
    size_t needed = unusualOutput ? 5 : 10;
    Rarity inputRarity = static_cast<Rarity>(FromEnum(Rarity::Common) + tier);

    if (itemIds.size() != needed)
    {
        Platform::Print("TradeUp: recipe {} needs {} items, got {}\n", recipe, needed, itemIds.size());
        return {};
    }

    std::vector<Item *> items;
    std::vector<TradeUpInput> inputs;

    for (uint64_t itemId : itemIds)
    {
        Item *item = FindItem(itemId);
        if (!item || std::find(items.begin(), items.end(), item) != items.end())
        {
            Platform::Print("TradeUp: missing or duplicate item {}\n", itemId);
            return {};
        }

        if (item->GetRarity() != inputRarity || item->HasAttribute(AttributeDefIndex::KillEater) != statTrak)
        {
            Platform::Print("TradeUp: item {} doesn't fit recipe {} (rarity {})\n", itemId, recipe, FromEnum(item->GetRarity()));
            return {};
        }

        items.push_back(item);
        inputs.push_back({
            item->DefIndex(),
            static_cast<uint32_t>(item->GetAttributeValue<float>(AttributeDefIndex::TexturePrefab)),
            item->GetAttributeValue<float>(AttributeDefIndex::TextureWear),
        });
    }

    ItemDesc desc{};
    if (!m_itemSchema.SelectTradeUpOutput(inputs, inputRarity, unusualOutput, statTrak, desc))
    {
        return {};
    }

    InventoryModify modify;
    Item &created = CreateItem(modify, std::move(desc));
    createdId = created.FullIdFor(AccountId());

    Platform::Print("TradeUp: recipe {} -> def {} rarity {}\n", recipe, FromEnum(created.DefIndex()), FromEnum(created.GetRarity()));

    if (GetConfig().m_destroyUsedItems)
    {
        for (Item *item : items)
        {
            DestroyItem(modify, item);
        }
    }

    return FinishChanges(modify);
}

bool Inventory::ExpireRentals(uint32_t now, InventoryChangeMessages &changeMessages)
{
    std::vector<uint64_t> expired;
    for (const auto &[itemId, item] : m_items)
    {
        if (item.GetAttributeValue<uint32_t>(AttributeDefIndex::IsRental)
            && item.GetAttributeValue<uint32_t>(AttributeDefIndex::ExpirationDate) <= now)
        {
            expired.push_back(itemId);
        }
    }

    if (expired.empty())
    {
        return false;
    }

    InventoryModify modify;
    for (uint64_t itemId : expired)
    {
        DestroyItemById(modify, itemId);
    }

    Platform::Print("Rental expired: removed {} items\n", expired.size());
    changeMessages = FinishChanges(modify);
    return true;
}

InventoryChangeMessages Inventory::SetItemPositions(
    const CMsgSetItemPositions &message,
    std::vector<CMsgItemAcknowledged> &acknowledgements)
{
    for (const CMsgSetItemPositions_ItemPosition &position : message.item_positions())
    {
        assert(position.item_id() || position.legacy_item_id());
        uint64_t itemId = position.has_item_id() ? position.item_id() : ComposeItemId(AccountId(), position.legacy_item_id());
        if (!FindItem(itemId))
        {
            assert(false);
            return {};
        }
    }

    InventoryModify modify;

    for (const CMsgSetItemPositions_ItemPosition &position : message.item_positions())
    {
        uint64_t itemId = position.has_item_id() ? position.item_id() : ComposeItemId(AccountId(), position.legacy_item_id());
        Item *item = FindItem(itemId);

        Platform::Print("SetItemPositions: {} --> {}\n", itemId, position.position());
        modify.SetItemInventory(*item, position.position());

        CMsgItemAcknowledged &acknowledgement = acknowledgements.emplace_back();
        item->ToEconItemPreviewDataBlock(*acknowledgement.mutable_iteminfo(), AccountId());
    }

    return FinishChanges(modify);
}

static AttributeDefIndex StickerAttributeForSlot(AttributeDefIndex defIndex, uint32_t slot)
{
    // offset by id, wear, scale, rotation
    uint32_t result = FromEnum(defIndex) + (slot * 4);
    return ToEnum<AttributeDefIndex>(result);
}

// cs2 offsets are 2 attributes per slot
static AttributeDefIndex StickerOffsetAttributeForSlot(AttributeDefIndex defIndex, uint32_t slot)
{
    uint32_t result = FromEnum(defIndex) + (slot * 2);
    return ToEnum<AttributeDefIndex>(result);
}

// cs2 charms go through k_EMsgGCApplySticker too, the tool item has a keychain id instead of a sticker id
InventoryChangeMessages Inventory::ApplyKeychain(const CMsgApplySticker &message, Item &keychain)
{
    InventoryModify modify;

    Item *item = nullptr;
    if (!message.baseitem_defidx())
    {
        item = FindItem(message.item_item_id());
        if (!item)
        {
            assert(false);
            return {};
        }
    }
    else
    {
        ItemDesc desc{};
        m_itemSchema.GetItemDesc(ToEnum<ItemDefIndex>(message.baseitem_defidx()), ItemOriginBaseItem, UnacknowledgedInvalid, desc);
        item = &CreateItem(modify, std::move(desc));
    }

    // only one charm slot exists so far
    modify.SetItemAttribute(*item, AttributeDefIndex::KeychainId0, keychain.GetAttributeValue<uint32_t>(AttributeDefIndex::KeychainId0));
    modify.SetItemAttribute(*item, AttributeDefIndex::KeychainOffsetX0, message.sticker_offset_x());
    modify.SetItemAttribute(*item, AttributeDefIndex::KeychainOffsetY0, message.sticker_offset_y());
    modify.SetItemAttribute(*item, AttributeDefIndex::KeychainOffsetZ0, message.sticker_offset_z());

    // pattern, highlight reel and sticker capsule charms carry extra attributes, copy them over
    for (AttributeDefIndex extra : { AttributeDefIndex::KeychainSeed0, AttributeDefIndex::KeychainHighlight0, AttributeDefIndex::KeychainSticker0 })
    {
        if (keychain.HasAttribute(extra))
        {
            modify.SetItemAttribute(*item, extra, keychain.GetAttributeValue<uint32_t>(extra));
        }
    }

    uint64_t itemId = item->FullIdFor(AccountId());

    if (GetConfig().m_destroyUsedItems)
    {
        DestroyItem(modify, &keychain);
    }

    return FinishChanges(modify, k_EGCItemCustomizationNotification_ApplyKeychain, { itemId });
}

InventoryChangeMessages Inventory::ApplySticker(const CMsgApplySticker &message)
{
    InventoryModify modify;

    assert(message.has_sticker_item_id());

    Item *sticker = FindItem(message.sticker_item_id());
    if (!sticker)
    {
        assert(false);
        return {};
    }

    if (sticker->HasAttribute(AttributeDefIndex::KeychainId0))
    {
        return ApplyKeychain(message, *sticker);
    }

    Item *item = nullptr;
    if (!message.baseitem_defidx())
    {
        item = FindItem(message.item_item_id());
        if (!item)
        {
            assert(false);
            return {};
        }
    }

    // get the sticker kit def index
    uint32_t stickerKit = sticker->GetAttributeValue<uint32_t>(AttributeDefIndex::StickerId0);
    if (!stickerKit)
    {
        assert(false);
        return {};
    }

    if (!item)
    {
        assert(message.baseitem_defidx() && !message.item_item_id());

        ItemDesc desc{};
        m_itemSchema.GetItemDesc(ToEnum<ItemDefIndex>(message.baseitem_defidx()), ItemOriginBaseItem, UnacknowledgedInvalid, desc);
        item = &CreateItem(modify, std::move(desc));
    }
    else
    {
        assert(!message.baseitem_defidx() && message.item_item_id());
    }

    auto attributeStickerId = StickerAttributeForSlot(AttributeDefIndex::StickerId0, message.sticker_slot());
    auto attributeStickerWear = StickerAttributeForSlot(AttributeDefIndex::StickerWear0, message.sticker_slot());

    // add the sticker id attribute
    modify.SetItemAttribute(*item, attributeStickerId, stickerKit);

    // add the sticker wear attribute if this is not a patch (mikkotodo revisit...)
    if (sticker->DefIndex() != ItemDefIndex::Patch)
    {
        modify.SetItemAttribute(*item, attributeStickerWear, message.sticker_wear());
    }

    // cs2 free placement
    if (message.has_sticker_rotation())
    {
        modify.SetItemAttribute(*item, StickerAttributeForSlot(AttributeDefIndex::StickerRotation0, message.sticker_slot()), message.sticker_rotation());
    }

    if (message.has_sticker_scale())
    {
        modify.SetItemAttribute(*item, StickerAttributeForSlot(AttributeDefIndex::StickerScale0, message.sticker_slot()), message.sticker_scale());
    }

    if (message.has_sticker_offset_x() || message.has_sticker_offset_y())
    {
        modify.SetItemAttribute(*item, StickerOffsetAttributeForSlot(AttributeDefIndex::StickerOffsetX0, message.sticker_slot()), message.sticker_offset_x());
        modify.SetItemAttribute(*item, StickerOffsetAttributeForSlot(AttributeDefIndex::StickerOffsetY0, message.sticker_slot()), message.sticker_offset_y());
    }

    uint64_t itemId = item->FullIdFor(AccountId());

    if (GetConfig().m_destroyUsedItems)
    {
        DestroyItem(modify, sticker);
    }

    return FinishChanges(modify, k_EGCItemCustomizationNotification_ApplySticker, { itemId });
}

static void RemoveStickerAttributes(InventoryModify &modify, Item &item, uint32_t slot)
{
    modify.RemoveItemAttributes(item, {
        StickerAttributeForSlot(AttributeDefIndex::StickerId0, slot),
        StickerAttributeForSlot(AttributeDefIndex::StickerWear0, slot),
        StickerAttributeForSlot(AttributeDefIndex::StickerScale0, slot),
        StickerAttributeForSlot(AttributeDefIndex::StickerRotation0, slot),
        StickerOffsetAttributeForSlot(AttributeDefIndex::StickerOffsetX0, slot),
        StickerOffsetAttributeForSlot(AttributeDefIndex::StickerOffsetY0, slot),
    });
}

InventoryChangeMessages Inventory::ScrapeSticker(const CMsgApplySticker &message)
{
    InventoryModify modify;
    Item *item = FindItem(message.item_item_id());
    if (!item)
    {
        assert(false);
        return {};
    }

    auto attributeStickerWear = StickerAttributeForSlot(AttributeDefIndex::StickerWear0, message.sticker_slot());

    // mikkotodo randomize
    float wearIncrement = 1.0f / 9;

    // increment the wear, if there was no war (patches) or it went over 1, remove the sticker/patch
    auto wearUpdate = modify.IncrementItemAttribute(*item, attributeStickerWear, wearIncrement, 0.0f, 1.0f);
    if (wearUpdate != AttributeIncrement::Ok)
    {
        // mikkotodo fix... should this be deduced from the item???
        EGCItemCustomizationNotification request = k_EGCItemCustomizationNotification_RemoveSticker;
        if (wearUpdate == AttributeIncrement::NoAttribute)
        {
            request = k_EGCItemCustomizationNotification_RemovePatch;
        }

        if (item->GetRarity() == Rarity::Default)
        {
            // this was a default weapon clone with a sticker so destroy the entire item
            uint64_t fakeItemId = static_cast<uint64_t>(item->DefIndex()) | ItemIdDefaultItemMask;
            DestroyItem(modify, item);
            return FinishChanges(modify, request, { fakeItemId });
        }

        // remove the sticker
        RemoveStickerAttributes(modify, *item, message.sticker_slot());

        return FinishChanges(modify, request, { item->FullIdFor(AccountId()) });
    }

    return FinishChanges(modify);
}

InventoryChangeMessages Inventory::IncrementKillCountAttribute(uint64_t itemId, uint32_t amount)
{
    Item *item = FindItem(itemId);
    if (!item)
    {
        assert(false);
        return {};
    }

    InventoryModify modify;
    modify.IncrementItemAttribute(*item, AttributeDefIndex::KillEater, static_cast<int>(amount));
    return FinishChanges(modify);
}

InventoryChangeMessages Inventory::NameItem(uint64_t nameTagId, uint64_t itemId, std::string_view name)
{
    InventoryModify modify;
    Item *item = FindItem(itemId);
    if (!item)
    {
        assert(false);
        return {};
    }

    modify.SetItemAttribute(*item, AttributeDefIndex::CustomName, std::string{ name });

    // caskets get updated here...
    if (item->DefIndex() == ItemDefIndex::Casket)
    {
        if (!item->HasAttribute(AttributeDefIndex::CasketItemsCount))
        {
            modify.SetItemAttribute(*item, AttributeDefIndex::CasketItemsCount, 0u);
        }

        uint32_t modifyTime = static_cast<uint32_t>(time(nullptr));
        modify.SetItemAttribute(*item, AttributeDefIndex::CasketModificationDate, modifyTime);
    }

    if (GetConfig().m_destroyUsedItems)
    {
        assert(nameTagId != itemId);
        DestroyItemById(modify, nameTagId);
    }

    return FinishChanges(modify, k_EGCItemCustomizationNotification_NameItem, { itemId });
}

bool Inventory::TryEncapsulateSticker(const CMsgApplySticker &message, InventoryChangeMessages &messages)
{
    uint64_t toolId = message.sticker_item_id();
    bool toolIsBaseItem = (toolId & ItemIdDefaultItemMask) == ItemIdDefaultItemMask;
    Item *tool = (toolId && !toolIsBaseItem) ? FindItem(toolId) : nullptr;
    Item *target = message.item_item_id() ? FindItem(message.item_item_id()) : nullptr;
    if (!target || (toolId && !toolIsBaseItem && !tool))
    {
        return false;
    }

    auto isSticker = [](const Item *item) {
        return item && item->DefIndex() == ItemDefIndex::Sticker && item->HasAttribute(AttributeDefIndex::StickerId0);
    };
    auto slabKeychain = [&](const Item *item) {
        return item ? m_itemSchema.StaticAttributeUint32(item->DefIndex(), AttributeDefIndex::DisplayCaseKeychainId) : 0u;
    };

    Item *sticker = nullptr;
    Item *slab = nullptr;
    if (isSticker(tool) && slabKeychain(target))
    {
        sticker = tool;
        slab = target;
    }
    else if (slabKeychain(tool) && isSticker(target))
    {
        sticker = target;
        slab = tool;
    }
    else if (!tool && isSticker(target))
    {
        sticker = target; // RemoveKeychain(sticker, 0): no slab item involved
    }
    else
    {
        return false;
    }

    // keychain 37 "kc_sticker_display_case" is the only slab so far
    uint32_t keychainId = slab ? slabKeychain(slab) : m_itemSchema.StaticAttributeUint32(ItemDefIndex::StickerSlab, AttributeDefIndex::DisplayCaseKeychainId);
    uint32_t stickerKit = sticker->GetAttributeValue<uint32_t>(AttributeDefIndex::StickerId0);
    if (!keychainId)
    {
        Platform::Print("Sticker slab: no keychain for the slab\n");
        return false;
    }

    ItemDesc charm{};
    if (!m_itemSchema.GetItemDesc(ItemDefIndex::Keychain, ItemOriginCrafted, UnacknowledgedCrafted, charm))
    {
        return false;
    }

    // the slab looks like the sticker inside it
    charm.rarity = sticker->GetRarity();
    if (sticker->GetDesc().quality == Quality::Tournament)
    {
        charm.quality = Quality::Tournament;
    }
    charm.attributes.emplace_back(AttributeDefIndex::KeychainId0, keychainId);
    charm.attributes.emplace_back(AttributeDefIndex::KeychainSticker0, stickerKit);

    InventoryModify modify;
    Item &created = CreateItem(modify, std::move(charm));
    uint64_t createdId = created.FullIdFor(AccountId());
    DestroyItem(modify, sticker);
    if (slab)
    {
        DestroyItem(modify, slab);
    }

    Platform::Print("Sticker slab: sticker kit {} in keychain {}{}\n", stickerKit, keychainId, slab ? "" : " (no slab item)");
    messages = FinishChanges(modify, k_EGCItemCustomizationNotification_EncapsulateSticker, { createdId });
    return true;
}

bool Inventory::PreviewDataBlock(uint64_t itemId, CEconItemPreviewDataBlock &block)
{
    Item *item = FindItem(itemId);
    if (!item)
    {
        return false;
    }

    item->ToEconItemPreviewDataBlock(block, AccountId());

    // inspect data carries the exact float (as its bits), not the wear level of the acknowledge popup
    for (const ItemAttribute &attribute : item->GetDesc().attributes)
    {
        if (attribute.DefIndex() == AttributeDefIndex::TextureWear)
        {
            block.set_paintwear(std::bit_cast<uint32_t>(attribute.ValueAs<float>()));
        }
    }

    return true;
}

InventoryChangeMessages Inventory::StatTrakSwap(uint64_t toolId, uint64_t itemId1, uint64_t itemId2)
{
    Item *tool = FindItem(toolId);
    Item *item1 = FindItem(itemId1);
    Item *item2 = FindItem(itemId2);
    if (!tool || tool->DefIndex() != ItemDefIndex::StatTrakSwapTool || !item1 || !item2 || item1 == item2)
    {
        Platform::Print("StatTrak swap: bad request (tool {} items {} {})\n", toolId, itemId1, itemId2);
        return {};
    }

    // kills can't go to a music kit's MVP counter and back
    if (!item1->HasAttribute(AttributeDefIndex::KillEater) || !item2->HasAttribute(AttributeDefIndex::KillEater)
        || item1->GetAttributeValue<uint32_t>(AttributeDefIndex::KillEaterScoreType)
            != item2->GetAttributeValue<uint32_t>(AttributeDefIndex::KillEaterScoreType))
    {
        Platform::Print("StatTrak swap: items {} and {} don't have the same kind of StatTrak counter\n", itemId1, itemId2);
        return {};
    }

    uint32_t count1 = item1->GetAttributeValue<uint32_t>(AttributeDefIndex::KillEater);
    uint32_t count2 = item2->GetAttributeValue<uint32_t>(AttributeDefIndex::KillEater);

    InventoryModify modify;
    modify.SetItemAttribute(*item1, AttributeDefIndex::KillEater, count2);
    modify.SetItemAttribute(*item2, AttributeDefIndex::KillEater, count1);

    if (GetConfig().m_destroyUsedItems)
    {
        DestroyItem(modify, tool);
    }

    Platform::Print("StatTrak swap: {} <-> {}\n", count1, count2);
    return FinishChanges(modify, k_EGCItemCustomizationNotification_StatTrakSwap, { itemId1, itemId2 });
}

InventoryChangeMessages Inventory::NameBaseItem(uint64_t nameTagId, uint32_t defIndex, std::string_view name)
{
    InventoryModify modify;

    ItemDesc desc{};
    m_itemSchema.GetItemDesc(ToEnum<ItemDefIndex>(defIndex), ItemOriginBaseItem, UnacknowledgedInvalid, desc);

    Item &item = CreateItem(modify, std::move(desc));
    uint64_t itemId = item.FullIdFor(AccountId());

    modify.SetItemAttribute(item, AttributeDefIndex::CustomName, std::string{ name });

    if (GetConfig().m_destroyUsedItems)
    {
        assert(nameTagId != itemId);
        DestroyItemById(modify, nameTagId);
    }

    // mikkotodo def index???
    return FinishChanges(modify, k_EGCItemCustomizationNotification_NameBaseItem, { itemId });
}

InventoryChangeMessages Inventory::RemoveItemName(uint64_t itemId)
{
    InventoryModify modify;
    Item *item = FindItem(itemId);
    if (!item)
    {
        assert(false);
        return {};
    }

    if (item->GetRarity() == Rarity::Default)
    {
        uint64_t fakeItemId = static_cast<uint64_t>(item->DefIndex()) | ItemIdDefaultItemMask;
        DestroyItem(modify, item);
        return FinishChanges(modify, k_EGCItemCustomizationNotification_RemoveItemName, { fakeItemId });
    }

    modify.RemoveItemAttributes(*item, { AttributeDefIndex::CustomName });

    return FinishChanges(modify, k_EGCItemCustomizationNotification_RemoveItemName, { item->FullIdFor(AccountId()) });
}

Item *Inventory::FindItem(uint64_t itemId)
{
    auto it = m_items.find(itemId);
    if (it != m_items.end())
    {
        return &it->second;
    }

    return nullptr;
}

static void EmbedStorageReference(InventoryModify &modify, Item &item, uint64_t storageId)
{
    uint32_t low = (storageId & UINT32_MAX);
    modify.SetItemAttribute(item, AttributeDefIndex::CasketIdLow, low);

    uint32_t high = (storageId >> 32) & UINT32_MAX;
    modify.SetItemAttribute(item, AttributeDefIndex::CasketIdHigh, high);

    modify.RemoveItemEquips(item);
}

InventoryChangeMessages Inventory::CasketItemAdd(uint64_t casketId, uint64_t itemId)
{
    InventoryModify modify;
    Item *storage = FindItem(casketId);
    if (!storage)
    {
        assert(false);
        return {};
    }

    Item *target = FindItem(itemId);
    if (!target)
    {
        assert(false);
        return {};
    }

    if (storage->DefIndex() != ItemDefIndex::Casket)
    {
        assert(false);
        return {};
    }

    auto result = modify.IncrementItemAttribute(*storage, AttributeDefIndex::CasketItemsCount, 1, 1000);
    if (result == AttributeIncrement::OutOfRange)
    {
        return FinishChanges(modify, k_EGCItemCustomizationNotification_CasketTooFull, { casketId });
    }

    if (result != AttributeIncrement::Ok)
    {
        assert(false);
        return {};
    }

    uint32_t modifyTime = static_cast<uint32_t>(time(nullptr));
    modify.SetItemAttribute(*storage, AttributeDefIndex::CasketModificationDate, modifyTime);

    EmbedStorageReference(modify, *target, casketId);

    return FinishChanges(modify, k_EGCItemCustomizationNotification_CasketAdded, { casketId });
}

InventoryChangeMessages Inventory::CasketItemExtract(uint64_t casketId, uint64_t itemId)
{
    InventoryModify modify;
    Item *storage = FindItem(casketId);
    if (!storage)
    {
        assert(false);
        return {};
    }

    Item *target = FindItem(itemId);
    if (!target)
    {
        assert(false);
        return {};
    }

    if (storage->DefIndex() != ItemDefIndex::Casket)
    {
        assert(false);
        return {};
    }

    auto result = modify.IncrementItemAttribute(*storage, AttributeDefIndex::CasketItemsCount, -1, 1000);
    if (result != AttributeIncrement::Ok)
    {
        assert(false);
        return {};
    }

    uint32_t modifyTime = static_cast<uint32_t>(time(nullptr));
    modify.SetItemAttribute(*storage, AttributeDefIndex::CasketModificationDate, modifyTime);

    modify.RemoveItemAttributes(*target, { AttributeDefIndex::CasketIdLow, AttributeDefIndex::CasketIdHigh });

    return FinishChanges(modify, k_EGCItemCustomizationNotification_CasketRemoved, { casketId });
}

InventoryChangeMessages Inventory::PurchaseItems(const std::vector<uint32_t> &defIndexes, std::vector<uint64_t> &itemIds)
{
    itemIds.clear();
    itemIds.reserve(defIndexes.size());

    InventoryModify modify;

    for (uint32_t defIndex : defIndexes)
    {
        std::vector<ItemDesc> descs;
        std::vector<ItemDesc> casketedDescs;
        m_itemSchema.GetPurchaseItemDescs(ToEnum<ItemDefIndex>(defIndex), descs, casketedDescs);

        // fucked up handling of the rio2022 caskets
        // FIXME: the tournament event id is missing!!!
        uint64_t storageId = 0;

        for (ItemDesc &desc : descs)
        {
            Item &item = CreateItem(modify, std::move(desc));
            const uint64_t itemId = item.FullIdFor(AccountId());
            itemIds.push_back(itemId);

            if (casketedDescs.size())
            {
                storageId = itemId;

                modify.SetItemAttribute(item,
                    AttributeDefIndex::CasketItemsCount,
                    static_cast<uint32_t>(casketedDescs.size()));
            }
        }

        for (ItemDesc &desc : casketedDescs)
        {
            // ack them (FIXME: do this properly)
            desc.inventory = 1;
            Item &item = CreateItem(modify, std::move(desc));
            EmbedStorageReference(modify, item, storageId);
        }
    }

    return FinishChanges(modify);
}

bool Inventory::UnequipItem(InventoryModify &modify, uint64_t itemId)
{
    uint32_t defIndex, paintKitIndex;
    if (IsDefaultItemId(itemId, defIndex, paintKitIndex))
    {
        // not supported
        assert(false);
        return false;
    }

    Item *item = FindItem(itemId);
    if (!item)
    {
        assert(false);
        return false;
    }

    modify.RemoveItemEquips(*item);

    return true;
}

// this goes through everything on purpose
void Inventory::UnequipItem(InventoryModify &modify, uint32_t classId, uint32_t slotId)
{
    // check non default items first
    for (auto &pair : m_items)
    {
        modify.RemoveItemEquip(pair.second, classId, slotId);
    }

    // check default equips, just delete the references locally
    auto predicate = [classId, slotId](const DefaultEquip &equip)
    {
        return equip.classId == classId && equip.slotId == slotId;
    };

    if (std::erase_if(m_defaultEquips, predicate))
    {
        // count as a change so the version bumps and inventory.txt gets written
        modify.MarkDefaultEquipChanged(DefaultEquip{ 0, classId, slotId });
    }
}

bool Inventory::DestroyItemById(InventoryModify &modify, uint64_t itemId)
{
    auto it = m_items.find(itemId);
    if (it == m_items.end())
    {
        return false;
    }

    Item &item = it->second;

    // schizo: if this item was in a casket, decrement the casket item count
    Item *casket = FindItem(GetCasketId(item));
    if (casket && casket->DefIndex() == ItemDefIndex::Casket)
    {
        modify.IncrementItemAttribute(*casket, AttributeDefIndex::CasketItemsCount, -1);
        modify.SetItemAttribute(*casket, AttributeDefIndex::CasketModificationDate, static_cast<uint32_t>(time(nullptr)));
    }

    // schizo: if this was a casket, take all of the items out
    if (item.DefIndex() == ItemDefIndex::Casket)
    {
        for (auto &pair : m_items)
        {
            if (GetCasketId(pair.second) == itemId)
            {
                modify.RemoveItemAttributes(pair.second, { AttributeDefIndex::CasketIdLow, AttributeDefIndex::CasketIdHigh });
            }
        }
    }

    // an active terminal takes its offer with it (expired, ended or bought)
    std::vector<uint64_t> offers;
    if (item.GetDesc().quality == Quality::Volatile)
    {
        for (const auto &[otherId, other] : m_items)
        {
            if (GetCasketId(other) == itemId)
            {
                offers.push_back(otherId);
            }
        }
    }

    m_items.erase(it);

    for (uint64_t offerId : offers)
    {
        DestroyItemById(modify, offerId);
    }

    RemoveCachedIcon(static_cast<uint32_t>(itemId));

    modify.MarkItemDestroyed(static_cast<uint32_t>(itemId));
    return true;
}

void Inventory::DestroyItem(InventoryModify &modify, Item *item)
{
    bool destroyed = DestroyItemById(modify, item->FullIdFor(AccountId()));
    if (!destroyed)
    {
        // how is this possible???
        assert(false);
    }
}

void Inventory::RemoveCachedIcon(uint32_t lowId)
{
    // csgo_gc deleted cached item icons from csgo/resource/flash here. CS2 has no
    // such cache and cs2_gc never deletes anything in the game folder.
    (void)lowId;
}

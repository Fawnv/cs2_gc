// account level progress: player level and xp, the weekly care package, and the
// "account" block of inventory.txt that keeps them (plus charm removals, Armory and terminals)
#include "stdafx.h"
#include "inventory.h"
#include "case_opening.h"
#include "config.h"
#include "inventory_messages.h"
#include "inventory_modify.h"
#include "keyvalue.h"
#include "random.h"
#include <chrono>
#include <cmath>

// "1 2 3" <-> numbers
template<typename T>
static std::vector<T> SplitNumbers(std::string_view text)
{
    std::vector<T> out;
    for (size_t start = 0; start < text.size();)
    {
        size_t end = (std::min)(text.find(' ', start), text.size());
        if (end > start)
        {
            out.push_back(FromString<T>(text.substr(start, end - start)));
        }
        start = end + 1;
    }
    return out;
}

template<typename T>
static std::string JoinNumbers(const std::vector<T> &numbers)
{
    std::string out;
    for (T number : numbers)
    {
        out += (out.empty() ? "" : " ") + std::to_string(number);
    }
    return out;
}

void Inventory::ReadAccount(const KeyValue *accountKey)
{
    m_account = {};

    if (!accountKey)
    {
        // a new inventory: charges and level from the config
        m_account.keychainRemoveCharges = GetConfig().m_keychainRemoveCharges;
        m_account.level = static_cast<uint32_t>(std::clamp(GetConfig().m_level, 1, static_cast<int>(MaxLevel)));
        m_account.xp = static_cast<uint32_t>(std::clamp(GetConfig().m_xp, 0, static_cast<int>(XpPerLevel - 1)));
        return;
    }

    m_account.keychainRemoveCharges = accountKey->GetNumber<uint32_t>("keychain_remove_charges", 0);
    m_account.xpShopBalance = accountKey->GetNumber<uint32_t>("xpshop_balance", 0);
    m_account.xpShopGeneration = accountKey->GetNumber<uint32_t>("xpshop_generation", 0);
    m_account.xpShopTracks = SplitNumbers<uint32_t>(accountKey->GetString("xpshop_tracks"));

    // inventories from before levels were kept here start from the config
    m_account.level = accountKey->GetNumber<uint32_t>("player_level", static_cast<uint32_t>(GetConfig().m_level));
    m_account.xp = accountKey->GetNumber<uint32_t>("player_xp", static_cast<uint32_t>(GetConfig().m_xp));
    m_account.level = std::clamp(m_account.level, 1u, MaxLevel);
    m_account.xp = (std::min)(m_account.xp, XpPerLevel - 1);
    m_account.xpWeek = accountKey->GetNumber<uint32_t>("xp_week", 0);
    m_account.xpEarned = accountKey->GetNumber<uint32_t>("xp_earned", 0);
    m_account.xpEarnedWeek = accountKey->GetNumber<uint32_t>("xp_earned_week", 0);

    m_account.storeGeneration = accountKey->GetNumber<uint32_t>("care_package_time", 0);
    m_account.storeBalance = accountKey->GetNumber<uint32_t>("care_package_picks", 0);
    m_account.storeItems = SplitNumbers<uint64_t>(accountKey->GetString("care_package_items"));


    // "terminals" { "<def>" { "seen" "<faux id>:<time> ..." "claimed" "<reward>:<time> ..." } }
    const KeyValue *terminalsKey = accountKey->GetSubkey("terminals");
    if (terminalsKey)
    {
        for (const KeyValue &terminalKey : terminalsKey->Subkeys())
        {
            AccountState::Volatile &history = m_account.volatileHistory[FromString<uint32_t>(terminalKey.Name())];
            auto readPairs = [](std::string_view text, auto &out) {
                using First = typename std::decay_t<decltype(out)>::value_type::first_type;
                for (size_t start = 0; start < text.size();)
                {
                    size_t end = (std::min)(text.find(' ', start), text.size());
                    std::string_view pair = text.substr(start, end - start);
                    size_t colon = pair.find(':');
                    if (colon != std::string_view::npos)
                    {
                        out.emplace_back(FromString<First>(pair.substr(0, colon)), FromString<uint32_t>(pair.substr(colon + 1)));
                    }
                    start = end + 1;
                }
            };
            readPairs(terminalKey.GetString("seen"), history.seen);
            readPairs(terminalKey.GetString("claimed"), history.claimed);
        }
    }
}

void Inventory::WriteAccount(KeyValueWrite &write) const
{
    write.BeginBlock("account");
    write.AddNumber("keychain_remove_charges", m_account.keychainRemoveCharges);
    write.AddNumber("xpshop_balance", m_account.xpShopBalance);
    write.AddString("xpshop_tracks", JoinNumbers(m_account.xpShopTracks));
    write.AddNumber("xpshop_generation", m_account.xpShopGeneration);

    write.AddNumber("player_level", m_account.level);
    write.AddNumber("player_xp", m_account.xp);
    write.AddNumber("xp_week", m_account.xpWeek);
    write.AddNumber("xp_earned", m_account.xpEarned);
    write.AddNumber("xp_earned_week", m_account.xpEarnedWeek);

    write.AddNumber("care_package_time", m_account.storeGeneration);
    write.AddNumber("care_package_picks", m_account.storeBalance);
    write.AddString("care_package_items", JoinNumbers(m_account.storeItems));

    if (!m_account.volatileHistory.empty())
    {
        write.BeginBlock("terminals");
        for (const auto &[defIndex, history] : m_account.volatileHistory)
        {
            auto joinPairs = [](const auto &pairs) {
                std::string out;
                for (const auto &[first, when] : pairs)
                {
                    out += std::format("{}{}:{}", out.empty() ? "" : " ", first, when);
                }
                return out;
            };
            write.BeginBlock(std::to_string(defIndex));
            write.AddString("seen", joinPairs(history.seen));
            write.AddString("claimed", joinPairs(history.claimed));
            write.EndBlock();
        }
        write.EndBlock();
    }

    write.EndBlock();
}

void Inventory::AccountPersonalStore(CSOAccountItemPersonalStore &out) const
{
    out.set_generation_time(m_account.storeGeneration);
    out.set_redeemable_balance(m_account.storeBalance);
    for (uint64_t itemId : m_account.storeItems)
    {
        out.add_items(itemId);
    }
}

void Inventory::BuildAccountObjects(SOCacheMessages &messages, uint32_t changes)
{
    if (changes & AccountChangeKeychainCharges)
    {
        CSOAccountKeychainRemoveToolCharges charges;
        AccountKeychainCharges(charges);
        AddToMultipleObjects(messages.updated, SOTypeAccountKeychainRemoveToolCharges, charges);
    }

    if (changes & AccountChangeXpShop)
    {
        CSOAccountXpShop xpShop;
        AccountXpShop(xpShop);
        AddToMultipleObjects(messages.updated, SOTypeAccountXpShop, xpShop);
    }

    if ((changes & AccountChangePersonalStore) && m_account.storeGeneration)
    {
        CSOAccountItemPersonalStore store;
        AccountPersonalStore(store);
        AddToMultipleObjects(messages.updated, SOTypeAccountItemPersonalStore, store);
    }

    if (changes & AccountChangeVolatile)
    {
        for (const auto &[defIndex, history] : m_account.volatileHistory)
        {
            CSOVolatileItemOffer offer;
            offer.set_defidx(defIndex);
            for (auto [fauxId, when] : history.seen)
            {
                offer.add_faux_itemid(fauxId);
                offer.add_generation_time(when);
            }
            AddToMultipleObjects(messages.updated, SOTypeVolatileItemOffer, offer);

            CSOVolatileItemClaimedRewards rewards;
            rewards.set_defidx(defIndex);
            for (auto [reward, when] : history.claimed)
            {
                rewards.add_reward(reward);
                rewards.add_generation_time(when);
            }
            AddToMultipleObjects(messages.updated, SOTypeVolatileItemClaimedRewards, rewards);
        }
    }

    if (changes & AccountChangeLevel)
    {
        CSOPersonaDataPublic personaData;
        personaData.set_player_level(m_account.level);
        personaData.set_elevated_state(true);
        AddToMultipleObjects(messages.updated, SOTypePersonaDataPublic, personaData);
    }
}

uint32_t Inventory::XpWeekStart(uint32_t now)
{
    // the xp week starts on Wednesday 00:00 UTC; 1970-01-07 was a Wednesday
    constexpr int64_t Day = 24 * 60 * 60;
    int64_t days = static_cast<int64_t>(now) / Day;
    int64_t weeks = (days - 6) / 7;
    return static_cast<uint32_t>((weeks * 7 + 6) * Day);
}

bool Inventory::CarePackageEarnedThisWeek() const
{
    return m_account.xpWeek == XpWeekStart(static_cast<uint32_t>(time(nullptr)));
}

InventoryChangeMessages Inventory::AddXp(uint32_t xp, std::string_view reason)
{
    if (!xp)
    {
        return {};
    }

    InventoryModify modify;
    GrantXp(modify, xp, reason);
    InventoryChangeMessages messages = FinishChanges(modify);
    messages.levelChanged = true;
    return messages;
}

void Inventory::FixPaintedQualities()
{
    // painted guns older builds (and csgo_gc) made with the prefab's normal quality: make them
    // unique, or no trade up contract takes them
    InventoryModify modify;
    size_t fixed = 0;
    for (auto &[itemId, item] : m_items)
    {
        // guns only (defs below 500): knives and gloves are unusual, not unique
        if (item.GetDesc().quality == Quality::Normal && FromEnum(item.DefIndex()) < 500
            && item.GetAttributeValue<float>(AttributeDefIndex::TexturePrefab) > 0)
        {
            ItemDesc desc = item.GetDesc();
            desc.quality = Quality::Unique;
            modify.UpdateFromDesc(item, std::move(desc));
            fixed++;
        }
    }

    if (fixed)
    {
        FinishChanges(modify);
        Platform::Print("Inventory: {} painted weapons were normal quality, now unique (trade up contracts take them)\n", fixed);
    }
}

void Inventory::FixStatTrakStorageUnits()
{
    // a StatTrak counter (attribute 80) on a storage unit crashes the client (an access violation
    // under panorama: the counter goes on the item's model, a storage unit has none); the
    // inventory editor could put one there. Medals, cases and the like have models and keep theirs
    InventoryModify modify;
    size_t fixed = 0;
    for (auto &[itemId, item] : m_items)
    {
        if (item.DefIndex() == ItemDefIndex::Casket
            && (item.HasAttribute(AttributeDefIndex::KillEater) || item.HasAttribute(AttributeDefIndex::KillEaterScoreType)))
        {
            ItemDesc desc = item.GetDesc();
            std::erase_if(desc.attributes, [](const ItemAttribute &attribute) {
                return attribute.DefIndex() == AttributeDefIndex::KillEater || attribute.DefIndex() == AttributeDefIndex::KillEaterScoreType;
            });
            modify.UpdateFromDesc(item, std::move(desc));
            fixed++;
        }
    }

    if (fixed)
    {
        FinishChanges(modify);
        Platform::Print("Inventory: removed StatTrak from {} storage unit(s), the game crashes on one\n", fixed);
    }
}

void Inventory::RefreshCarePackage()
{
    if (!m_account.storeBalance)
    {
        return;
    }

    // picks left: older builds gave the offered items inventory 0, which the client shows as
    // new items to acknowledge; and the main menu opens the care package popup only for a
    // generation_time it hasn't seen yet (cl_redemption_reset_timestamp), so a new one on every
    // start brings it back until the picks are made
    InventoryModify modify;
    for (uint64_t itemId : m_account.storeItems)
    {
        Item *item = FindItem(itemId);
        uint32_t inventory = item ? item->GetDesc().inventory : 1;
        if (item && item->HasAttribute(AttributeDefIndex::FreeRewardStatus)
            && (!inventory || (inventory & InventoryUnacknowledged(UnacknowledgedInvalid))))
        {
            modify.SetItemInventory(*item, NextInventoryPosition());
        }
    }

    m_account.storeGeneration = static_cast<uint32_t>(time(nullptr));
    modify.MarkAccountChanged(AccountChangePersonalStore);
    FinishChanges(modify);
    Platform::Print("Care package: {} picks left, the main menu will offer it again\n", m_account.storeBalance);
}

uint32_t Inventory::NextInventoryPosition() const
{
    uint32_t highest = 0;
    for (const auto &[itemId, item] : m_items)
    {
        uint32_t inventory = item.GetDesc().inventory;
        if (!(inventory & InventoryUnacknowledged(UnacknowledgedInvalid)))
        {
            highest = (std::max)(highest, inventory);
        }
    }
    return highest + 1;
}

uint32_t Inventory::WeeklyXpEarned() const
{
    return m_account.xpEarnedWeek == XpWeekStart(static_cast<uint32_t>(time(nullptr))) ? m_account.xpEarned : 0;
}

InventoryChangeMessages Inventory::AddMatchXp(uint32_t baseXp, std::string_view reason)
{
    uint32_t week = XpWeekStart(static_cast<uint32_t>(time(nullptr)));
    if (m_account.xpEarnedWeek != week)
    {
        m_account.xpEarnedWeek = week;
        m_account.xpEarned = 0;
    }

    // walk the tiers: the part of the match that fits under a threshold gets that tier's rate
    struct Tier
    {
        double upTo;
        double multiplier;
    };
    static constexpr Tier Tiers[] = {
        { 4500, 4.0 },
        { 7500, 2.0 },
        { 11167, 1.0 },
        { 1e12, 0.175 },
    };

    double earned = m_account.xpEarned;
    double left = baseXp;
    double gained = 0;
    for (const Tier &tier : Tiers)
    {
        if (left <= 0)
        {
            break;
        }
        if (earned >= tier.upTo)
        {
            continue;
        }

        double take = (std::min)(left, (tier.upTo - earned) / tier.multiplier);
        gained += take * tier.multiplier;
        earned += take * tier.multiplier;
        left -= take;
    }

    uint32_t xp = static_cast<uint32_t>(std::lround(gained));
    m_account.xpEarned += xp;
    Platform::Print("Match xp: {} base -> {} with the weekly bonus ({} earned this week)\n", baseXp, xp, m_account.xpEarned);
    return AddXp(xp, reason);
}

void Inventory::GrantXp(InventoryModify &modify, uint32_t xp, std::string_view reason)
{
    uint32_t levelBefore = m_account.level;
    uint32_t total = m_account.xp + xp;
    uint32_t levelUps = 0;
    while (total >= XpPerLevel)
    {
        // level 40 is the top: its bar starts over and still counts for the weekly drop
        total -= XpPerLevel;
        m_account.level = (std::min)(m_account.level + 1, MaxLevel);
        levelUps++;
    }
    m_account.xp = total;
    modify.MarkAccountChanged(AccountChangeLevel);

    if (levelUps && !CarePackageEarnedThisWeek())
    {
        m_account.xpWeek = XpWeekStart(static_cast<uint32_t>(time(nullptr)));
        FillCarePackage(modify);
    }

    Platform::Print("XP: +{} for {}, level {} -> {} ({} / {} xp)\n", xp, reason, levelBefore, m_account.level,
        m_account.xp, XpPerLevel);
}

// the weekly drop pool: CS2's "timed drops" collections, the newer map collections and recent cases
static constexpr std::string_view CarePackageCollections[] = {
    "set_timed_drops_achroma",
    "set_timed_drops_cool",
    "set_timed_drops_exuberant",
    "set_timed_drops_neutral",
    "set_timed_drops_warm",
    "set_train_2025",
    "set_overpass_2024",
};

static constexpr std::string_view CarePackageCases[] = {
    "crate_community_33", // Kilowatt
    "crate_community_34", // Gallery
    "crate_community_35", // Fever
};

void Inventory::FillCarePackage(InventoryModify &modify)
{
    // the old offer goes away with the new week
    for (uint64_t itemId : m_account.storeItems)
    {
        Item *item = FindItem(itemId);
        if (item && item->HasAttribute(AttributeDefIndex::FreeRewardStatus))
        {
            DestroyItem(modify, item);
        }
    }
    m_account.storeItems.clear();

    CaseOpening caseOpening{ m_itemSchema };
    for (size_t slot = 0; slot < CarePackageSize * 4 && m_account.storeItems.size() < CarePackageSize; slot++)
    {
        std::vector<ItemDesc> descs;
        if (Random::Uint32(0, 3) == 0)
        {
            // a case one time in four
            std::string_view name = CarePackageCases[Random::Uint32(0, static_cast<uint32_t>(std::size(CarePackageCases) - 1))];
            std::optional<ItemDefIndex> defIndex = m_itemSchema.ItemDefIndexByName(name);
            ItemDesc desc{};
            if (defIndex && m_itemSchema.GetItemDesc(*defIndex, ItemOriginLevelUpReward, UnacknowledgedLevelUpReward, desc))
            {
                descs.push_back(std::move(desc));
            }
        }
        else
        {
            std::string_view name = CarePackageCollections[Random::Uint32(0, static_cast<uint32_t>(std::size(CarePackageCollections) - 1))];
            const LootList *lootList = m_itemSchema.GetLootList(name);
            if (lootList)
            {
                caseOpening.SelectItemsFromLootList(*lootList, false, static_cast<ItemOrigin>(ItemOriginLevelUpReward),
                    UnacknowledgedLevelUpReward, descs);
            }
        }

        if (descs.empty())
        {
            continue;
        }

        // an ordinary acknowledged position: inventory 0 (or the unacknowledged bit) makes the
        // client show the "new items" popup for all four. The free reward status attribute keeps
        // them out of the inventory until picked, the main menu's care package popup shows them
        ItemDesc &desc = descs.front();
        desc.inventory = NextInventoryPosition();
        desc.attributes.emplace_back(AttributeDefIndex::FreeRewardStatus, 1u);
        Item &item = CreateItem(modify, std::move(desc));
        m_account.storeItems.push_back(item.FullIdFor(AccountId()));
    }

    m_account.storeGeneration = static_cast<uint32_t>(time(nullptr));
    m_account.storeBalance = CarePackagePicks;
    modify.MarkAccountChanged(AccountChangePersonalStore);

    Platform::Print("Weekly care package: {} items, pick {}\n", m_account.storeItems.size(), m_account.storeBalance);
}

InventoryChangeMessages Inventory::RedeemFreeRewards(std::span<const uint64_t> itemIds)
{
    if (!m_account.storeBalance || itemIds.empty())
    {
        Platform::Print("Care package: nothing to pick (picks left {})\n", m_account.storeBalance);
        return {};
    }

    InventoryModify modify;
    std::vector<uint64_t> picked;
    for (uint64_t itemId : itemIds)
    {
        Item *item = FindItem(itemId);
        bool offered = std::find(m_account.storeItems.begin(), m_account.storeItems.end(), itemId) != m_account.storeItems.end();
        if (!item || !offered || !item->HasAttribute(AttributeDefIndex::FreeRewardStatus) || picked.size() >= m_account.storeBalance)
        {
            Platform::Print("Care package: can't pick {}\n", itemId);
            continue;
        }

        modify.RemoveItemAttributes(*item, { AttributeDefIndex::FreeRewardStatus });
        modify.SetItemInventory(*item, InventoryUnacknowledged(UnacknowledgedLevelUpReward));
        picked.push_back(itemId);
    }

    if (picked.empty())
    {
        return {};
    }

    m_account.storeBalance -= static_cast<uint32_t>(picked.size());
    if (!m_account.storeBalance)
    {
        // the ones not picked are gone
        for (uint64_t itemId : m_account.storeItems)
        {
            Item *item = FindItem(itemId);
            if (item && item->HasAttribute(AttributeDefIndex::FreeRewardStatus))
            {
                DestroyItem(modify, item);
            }
        }
    }
    modify.MarkAccountChanged(AccountChangePersonalStore);

    Platform::Print("Care package: picked {} items, {} picks left\n", picked.size(), m_account.storeBalance);

    InventoryChangeMessages messages = FinishChanges(modify);
    messages.notification.mutable_item_id()->Assign(picked.begin(), picked.end());
    messages.notification.set_request(k_EGCItemCustomizationNotification_ClientRedeemFreeReward);
    return messages;
}

// "prestige coin <year>", "prestige coin <year> level 2"... of the newest year up to `year` in the schema
static std::vector<ItemDefIndex> PrestigeLadder(const ItemSchema &schema, int year)
{
    std::vector<ItemDefIndex> ladder;
    for (; year >= 2015 && ladder.empty(); year--)
    {
        std::string base = std::format("prestige coin {}", year);
        std::optional<ItemDefIndex> first = schema.ItemDefIndexByName(base);
        if (!first)
        {
            continue;
        }

        ladder.push_back(*first);
        for (int level = 2;; level++)
        {
            std::optional<ItemDefIndex> next = schema.ItemDefIndexByName(std::format("{} level {}", base, level));
            if (!next)
            {
                break;
            }
            ladder.push_back(*next);
        }
    }
    return ladder;
}

std::optional<Inventory::PrestigeCoin> Inventory::NextPrestigeCoin() const
{
    using namespace std::chrono;
    int year = static_cast<int>(year_month_day{ floor<days>(system_clock::now()) }.year());
    std::vector<ItemDefIndex> ladder = PrestigeLadder(m_itemSchema, year);
    if (ladder.empty())
    {
        return std::nullopt;
    }

    // the highest level of that year owned
    size_t owned = 0;
    uint64_t ownedId = 0;
    for (const auto &[itemId, item] : m_items)
    {
        auto it = std::find(ladder.begin(), ladder.end(), item.DefIndex());
        size_t level = (it == ladder.end()) ? 0 : static_cast<size_t>(it - ladder.begin()) + 1;
        if (level > owned)
        {
            owned = level;
            ownedId = item.FullIdFor(AccountId());
        }
    }

    PrestigeCoin coin;
    coin.defIndex = ladder[(std::min)(owned, ladder.size() - 1)];
    coin.upgradeId = ownedId;
    return coin;
}

InventoryChangeMessages Inventory::RedeemPrestigeCoin(ItemDefIndex requested, uint64_t &itemId, std::string &error)
{
    itemId = 0;
    if (m_account.level < MaxLevel)
    {
        error = std::format("level {}, the medal comes at {}", m_account.level, MaxLevel);
        return {};
    }

    std::optional<PrestigeCoin> coin = NextPrestigeCoin();
    if (!coin || coin->defIndex != requested)
    {
        error = std::format("asked for {}, the next medal is {}", FromEnum(requested), coin ? FromEnum(coin->defIndex) : 0u);
        return {};
    }

    ItemDesc desc{};
    if (!m_itemSchema.GetItemDesc(coin->defIndex, ItemOriginEarned, UnacknowledgedEarned, desc))
    {
        error = std::format("no item {} in the schema", FromEnum(coin->defIndex));
        return {};
    }

    InventoryModify modify;
    Item *owned = coin->upgradeId ? FindItem(coin->upgradeId) : nullptr;
    if (owned)
    {
        // the same item becomes the next level, equips and all
        modify.UpdateFromDesc(*owned, std::move(desc));
        itemId = coin->upgradeId;
    }
    else
    {
        Item &created = CreateItem(modify, std::move(desc));
        itemId = created.FullIdFor(AccountId());
    }

    uint32_t levelBefore = m_account.level;
    m_account.level = 1;
    m_account.xp = 0;
    modify.MarkAccountChanged(AccountChangeLevel);
    Platform::Print("Service medal: {} {} ({}), level {} -> 1\n", owned ? "upgraded to" : "got", FromEnum(coin->defIndex),
        m_itemSchema.ItemDefName(coin->defIndex), levelBefore);

    InventoryChangeMessages messages = FinishChanges(modify);
    messages.levelChanged = true;
    return messages;
}

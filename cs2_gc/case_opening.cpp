#include "stdafx.h"
#include "case_opening.h"
#include "config.h"
#include "item.h"
#include "item_schema.h"
#include "random.h"

struct RarityGroup
{
    std::span<const LootListItem *const> items;
    float weight;
};

CaseOpening::CaseOpening(const ItemSchema &itemSchema)
    : m_itemSchema{ itemSchema }
{
}

// returns true if there are unusuals (stattrak able)
static bool GetLootListItems(const LootList &lootList, std::vector<const LootListItem *> &items)
{
    assert(!lootList.allEntriesAsAdditionalDrops);
    bool unusuals = lootList.isUnusual;

    for (const LootList *other : lootList.subLists)
    {
        unusuals |= GetLootListItems(*other, items);
    }

    for (const LootListItem &item : lootList.items)
    {
        items.push_back(&item);
    }

    return unusuals;
}

static bool CompareRarity(const LootListItem *a, const LootListItem *b)
{
    return a->CaseRarity() < b->CaseRarity();
}

bool CaseOpening::SelectItemFromCrate(ItemDefIndex crateDefIndex, ItemDesc &desc)
{
    const LootList *lootList = m_itemSchema.GetCrateLootList(crateDefIndex);
    if (!lootList)
    {
        assert(false);
        return false;
    }

    std::vector<ItemDesc> rewards;
    if (!SelectItemsFromLootList(*lootList, false, ItemOriginCrate, UnacknowledgedFoundInCrate, rewards))
    {
        return false;
    }

    assert(rewards.size() == 1);
    desc = std::move(rewards.front());

    // a weapon out of a tournament package (the crate has a "tournament event id") is a souvenir
    uint32_t eventId = m_itemSchema.StaticAttributeUint32(crateDefIndex, AttributeDefIndex::TournamentEventId);
    bool weapon = std::any_of(desc.attributes.begin(), desc.attributes.end(),
        [](const ItemAttribute &attribute) { return attribute.DefIndex() == AttributeDefIndex::TexturePrefab; });
    if (eventId && weapon)
    {
        MakeSouvenir(eventId, desc);
    }

    return true;
}

void CaseOpening::MakeSouvenir(uint32_t eventId, ItemDesc &desc)
{
    // like the real ones: souvenir quality, the match's two teams, and the event logo, both teams
    // and the match MVP's autograph as gold stickers. The match is made up, packages carry none
    desc.quality = Quality::Tournament;
    std::erase_if(desc.attributes, [](const ItemAttribute &attribute) {
        return attribute.DefIndex() == AttributeDefIndex::KillEater || attribute.DefIndex() == AttributeDefIndex::KillEaterScoreType;
    });
    desc.attributes.emplace_back(AttributeDefIndex::TournamentEventId, eventId);

    const ItemSchema::TournamentStickers *stickers = m_itemSchema.GetTournamentStickers(eventId);
    if (!stickers || stickers->teams.size() < 2)
    {
        return;
    }

    std::vector<uint32_t> teamIds;
    for (const auto &[teamId, kit] : stickers->teams)
    {
        teamIds.push_back(teamId);
    }
    uint32_t first = Random::Uint32(0, static_cast<uint32_t>(teamIds.size() - 1));
    uint32_t second = Random::Uint32(0, static_cast<uint32_t>(teamIds.size() - 2));
    second += (second >= first) ? 1 : 0;
    uint32_t team0 = teamIds[first];
    uint32_t team1 = teamIds[second];
    desc.attributes.emplace_back(AttributeDefIndex::TournamentEventTeam0Id, team0);
    desc.attributes.emplace_back(AttributeDefIndex::TournamentEventTeam1Id, team1);

    std::vector<uint32_t> kits;
    if (stickers->organizer)
    {
        kits.push_back(stickers->organizer);
    }
    kits.push_back(stickers->teams.at(team0));
    kits.push_back(stickers->teams.at(team1));

    std::vector<uint32_t> mvps;
    for (uint32_t teamId : { team0, team1 })
    {
        auto it = stickers->players.find(teamId);
        if (it != stickers->players.end())
        {
            mvps.insert(mvps.end(), it->second.begin(), it->second.end());
        }
    }
    if (!mvps.empty())
    {
        kits.push_back(Random::Select(std::span<const uint32_t>{ mvps }));
    }

    constexpr AttributeDefIndex StickerSlots[] = { AttributeDefIndex::StickerId0, AttributeDefIndex::StickerId1,
        AttributeDefIndex::StickerId2, AttributeDefIndex::StickerId3 };
    for (size_t slot = 0; slot < kits.size() && slot < std::size(StickerSlots); slot++)
    {
        desc.attributes.emplace_back(StickerSlots[slot], kits[slot]);
    }
}

bool CaseOpening::SelectItemsFromLootList(
    const LootList &lootList,
    bool forceStatTrak,
    ItemOrigin origin,
    UnacknowledgedType unacknowledgedType,
    std::vector<ItemDesc> &descs)
{
    assert(!lootList.items.empty() || !lootList.subLists.empty());
    forceStatTrak |= lootList.willProduceStatTrak;

    if (lootList.allEntriesAsAdditionalDrops)
    {
        for (const LootList *other : lootList.subLists)
        {
            if (!SelectItemsFromLootList(*other, forceStatTrak, origin, unacknowledgedType, descs))
            {
                return false;
            }
        }

        for (const LootListItem &item : lootList.items)
        {
            ItemDesc desc{};
            if (!m_itemSchema.ItemDescForLootListItem(item, forceStatTrak, origin, unacknowledgedType, desc))
            {
                return false;
            }

            descs.push_back(std::move(desc));
        }

        return true;
    }

    std::vector<const LootListItem *> lootListItems;
    lootListItems.reserve(32); // overkill
    bool containsUnusuals = GetLootListItems(lootList, lootListItems);

    // must be sorted for SelectLootListItem
    std::sort(lootListItems.begin(), lootListItems.end(), CompareRarity);

    const LootListItem *lootListItem = SelectLootListItem(lootListItems);
    if (!lootListItem)
    {
        assert(false);
        return false;
    }

    bool statTrak = forceStatTrak || ShouldMakeStatTrak(*lootListItem, lootList, containsUnusuals);

    ItemDesc desc{};
    if (!m_itemSchema.ItemDescForLootListItem(*lootListItem, statTrak, origin, unacknowledgedType, desc))
    {
        return false;
    }

    if (lootList.highlightKeychainId && !lootList.highlightReels.empty())
    {
        // 2025+ souvenir packages: the souvenir comes with a highlight reel charm attached
        uint32_t reel = Random::Select(std::span<const uint32_t>{ lootList.highlightReels });
        desc.attributes.emplace_back(AttributeDefIndex::KeychainId0, lootList.highlightKeychainId);
        desc.attributes.emplace_back(AttributeDefIndex::KeychainHighlight0, reel);
    }

    descs.push_back(std::move(desc));
    return true;
}

const LootListItem *CaseOpening::SelectLootListItem(std::span<const LootListItem *const> sortedItems)
{
    if (sortedItems.size() == 1)
    {
        return sortedItems.front();
    }

    std::vector<RarityGroup> groups;
    groups.reserve(sortedItems.size()); // overkill

    float totalWeight = 0;

    // group consecutive items of the same rarity
    for (auto first = sortedItems.begin(); first != sortedItems.end();)
    {
        Rarity rarity = (*first)->CaseRarity();

        auto last = first + 1;
        while (last != sortedItems.end() && (*last)->CaseRarity() == rarity)
        {
            last++;
        }

        float weight = GetConfig().GetRarityWeight(rarity);
        groups.push_back({ { first, last }, weight });
        totalWeight += weight;

        first = last;
    }

    // play the game of chance...
    float value = Random::Float(0.0f, totalWeight);
    float accum = 0.0f;

    for (const RarityGroup &group : groups)
    {
        accum += group.weight;
        if (value < accum)
        {
            // within a rarity: by weight (doppler phases and gems share one finish)
            return SelectWeightedLootListItem(group.items);
        }
    }

    assert(false);
    return nullptr;
}

bool CaseOpening::ShouldMakeStatTrak(const LootListItem &item, const LootList &lootList, bool containsUnusuals)
{
    if (lootList.willProduceStatTrak)
    {
        // must be stattrak
        return true;
    }

    if (!containsUnusuals)
    {
        // not a chance
        return false;
    }

    // unusual stattraks only valid below id 1000
    if (item.quality == Quality::Unusual
        && item.itemInfo->m_defIndex >= ToEnum<ItemDefIndex>(1000u))
    {
        return false;
    }

    // roll the dice
    return (Random::Uint32(1, 10) == 1);
}

// the upgrader: skins in, a chance at one dearer skin (see Inventory::Upgrade)
#include "stdafx.h"
#include "inventory.h"
#include "inventory_messages.h"
#include "inventory_modify.h"
#include "random.h"
#include "skin_prices.h"
#include <cmath>

InventoryChangeMessages Inventory::TouchItem(uint64_t itemId)
{
    Item *item = FindItem(itemId);
    if (!item)
    {
        return {};
    }

    InventoryModify modify;
    ItemDesc desc = item->GetDesc();
    modify.UpdateFromDesc(*item, std::move(desc));
    return FinishChanges(modify);
}

uint32_t Inventory::UpgradeTier(Rarity rarity, Quality quality)
{
    if (quality == Quality::Unusual)
    {
        return 7; // knives and gloves, above covert
    }

    uint32_t tier = FromEnum(rarity);
    return (tier >= 1 && tier <= 7) ? tier : 0;
}

InventoryChangeMessages Inventory::Upgrade(std::span<const uint64_t> depositIds, const UpgradeOffer &target, UpgradePriceFn depositPrice,
    UpgradeResult &result, std::optional<float> roll)
{
    result = {};
    if (depositIds.empty() || depositIds.size() > UpgradeMaxDeposits)
    {
        result.error = std::format("{} deposits, 1 to {} can go in", depositIds.size(), UpgradeMaxDeposits);
        return {};
    }

    if (!target.price)
    {
        result.error = std::format("the target {} paint {} wear {} isn't on sale", FromEnum(target.defIndex), target.paintKit, target.wear);
        return {};
    }

    std::vector<Item *> deposits;
    uint64_t worth = 0;
    for (uint64_t itemId : depositIds)
    {
        Item *item = FindItem(itemId);
        if (!item || std::find(deposits.begin(), deposits.end(), item) != deposits.end())
        {
            result.error = std::format("item {} is missing or twice", itemId);
            return {};
        }

        if (item->GetAttributeValue<float>(AttributeDefIndex::TexturePrefab) <= 0.0f || item->HasAttribute(AttributeDefIndex::IsRental))
        {
            result.error = std::format("item {} isn't a painted skin of your own", itemId);
            return {};
        }

        uint64_t price = depositPrice ? depositPrice(*item) : 0;
        if (!price)
        {
            result.error = std::format("item {} has no price (not on sale in its wear)", itemId);
            return {};
        }

        worth += price;
        deposits.push_back(item);
    }

    // an upgrade goes to something dearer: at the target's price or above it'd be a sure thing
    if (worth >= target.price)
    {
        result.error = std::format("the deposits are worth {} and the target {}: pick a dearer one", worth, target.price);
        return {};
    }

    const LootListItem *lootItem = m_itemSchema.FindPaintedLootListItem(target.defIndex, target.paintKit);
    float minWear = lootItem ? (std::max)(SkinPrices::WearMin(target.wear), lootItem->minFloat) : 0.0f;
    float maxWear = lootItem ? (std::min)(SkinPrices::WearMax(target.wear), lootItem->maxFloat) : 0.0f;
    if (!lootItem || target.wear < 0 || target.wear >= SkinPrices::WearCount || minWear >= maxWear)
    {
        result.error = std::format("no such target {} paint {} wear {}", FromEnum(target.defIndex), target.paintKit, target.wear);
        return {};
    }

    result.chance = static_cast<double>(worth) / static_cast<double>(target.price);
    float rolled = roll ? *roll : Random::Float(0.0f, 1.0f);
    result.won = rolled < result.chance;

    InventoryModify modify;
    for (Item *item : deposits)
    {
        DestroyItem(modify, item);
    }

    if (result.won)
    {
        // gloves have no StatTrak (defs from 1000 up)
        bool statTrak = target.statTrak && FromEnum(target.defIndex) < 1000;
        ItemDesc desc{};
        if (m_itemSchema.ItemDescForLootListItem(*lootItem, statTrak, ItemOriginCrafted, UnacknowledgedCrafted, desc))
        {
            // the wear that was priced, within the finish's own range (a hair below the next wear's start)
            float wear = Random::Float(minWear, maxWear);
            wear = (std::min)(wear, std::nextafter(maxWear, minWear));
            for (ItemAttribute &attribute : desc.attributes)
            {
                if (attribute.DefIndex() == AttributeDefIndex::TextureWear)
                {
                    attribute.Value() = wear;
                }
            }
            Item &created = CreateItem(modify, std::move(desc));
            result.itemId = created.FullIdFor(AccountId());
        }
        else
        {
            result.error = "the target couldn't be made";
            result.won = false;
        }
    }

    result.done = true;
    Platform::Print("Upgrader: {} deposits worth {} for {} paint {} wear {}{} worth {}, chance {:.4f}%, roll {:.4f}%: {}\n",
        deposits.size(), worth, FromEnum(target.defIndex), target.paintKit, SkinPrices::WearName(target.wear),
        target.statTrak ? " StatTrak" : "", target.price, result.chance * 100.0, rolled * 100.0, result.won ? "won" : "lost");
    return FinishChanges(modify);
}

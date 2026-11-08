// CS2 terminals (Sealed Genesis Terminal, Sealed Dead Hand Terminal), worked out from
// panorama/scripts/popups/popup_offers_laptop*.js and client.dll:
//  - unsealing (OpenCrate on the sealed terminal) turns it into an active terminal: same def,
//    quality 14 "volatile", a rental with an expiration date, notification crate_unlock
//  - InventoryAPI.UseToolWithIntArg(terminal, terminal, n) is OpenCrate { tool = subject = the
//    terminal, points_remaining = n, volatile_limit = the max price setting }: the next offer.
//    The offer is an item stored in the terminal like in a storage unit, with its price in
//    "purchase price"; "quest points remaining" on the terminal is the offer number, negative
//    for the final one. Asking again after the final offer ends the terminal
//  - the reply is notification casket_contents (1012) for the terminal, the client then looks
//    for the item stored in it. k_EMsgGCVolatileItemLoadContents (CMsgOpenCrate) asks for the
//    same when the terminal is opened again
//  - buying is a store purchase "<terminal def>(<terminal id>)"; the bought item keeps its id
//  - SO type 20 CSOVolatileItemOffer lists the offers seen (faux item ids) for the collection
//    catalog, a full rarity tier unlocks bonus xp: ClaimVolatileReward -> SO type 21
#include "stdafx.h"
#include "inventory.h"
#include "case_opening.h"
#include "config.h"
#include "inventory_messages.h"
#include "inventory_modify.h"
#include "random.h"

static uint32_t AttributeAsUint32(const ItemDesc &desc, AttributeDefIndex defIndex, uint32_t fallback = 0)
{
    for (const ItemAttribute &attribute : desc.attributes)
    {
        if (attribute.DefIndex() == defIndex)
        {
            if (const uint32_t *value = std::get_if<uint32_t>(&attribute.Value()))
            {
                return *value;
            }
            if (const float *value = std::get_if<float>(&attribute.Value()))
            {
                return static_cast<uint32_t>(*value);
            }
        }
    }
    return fallback;
}

static float AttributeAsFloat(const ItemDesc &desc, AttributeDefIndex defIndex, float fallback)
{
    for (const ItemAttribute &attribute : desc.attributes)
    {
        if (attribute.DefIndex() == defIndex)
        {
            if (const float *value = std::get_if<float>(&attribute.Value()))
            {
                return *value;
            }
        }
    }
    return fallback;
}

static uint64_t ItemCasketId(const Item &item)
{
    uint64_t low = item.GetAttributeValue<uint32_t>(AttributeDefIndex::CasketIdLow);
    uint64_t high = item.GetAttributeValue<uint32_t>(AttributeDefIndex::CasketIdHigh);
    return low ? ((high << 32) | low) : 0;
}

// InventoryAPI.GetFauxItemIDFromDefAndPaintIndex and ...UB1 (client.dll)
static uint64_t FauxItemId(uint32_t defIndex, uint32_t paintKit, uint32_t extra = 0)
{
    return 0xF000000000000000ull | (static_cast<uint64_t>(extra & 0xff) << 32) | ((paintKit & 0xffff) << 16) | (defIndex & 0xffff);
}

// prices are made up, in cents of the store currency: by rarity, wear and StatTrak, +-15%
static uint32_t TerminalOfferPrice(const ItemDesc &desc)
{
    double price = 0;
    if (desc.quality == Quality::Unusual)
    {
        price = 49999;
    }
    else
    {
        switch (desc.rarity)
        {
        case Rarity::Rare: price = 69; break;
        case Rarity::Mythical: price = 299; break;
        case Rarity::Legendary: price = 1299; break;
        case Rarity::Ancient: price = 4999; break;
        default: price = 39; break;
        }
    }

    float wear = AttributeAsFloat(desc, AttributeDefIndex::TextureWear, 0.25f);
    price *= wear < 0.07f ? 1.5 : wear < 0.15f ? 1.15 : wear < 0.38f ? 1.0 : wear < 0.45f ? 0.85 : 0.75;
    if (desc.quality == Quality::Strange)
    {
        price *= 2.5;
    }
    price *= 0.85 + Random::Uint32(0, 30) / 100.0;

    // x.x9
    uint32_t cents = static_cast<uint32_t>(price / 10 + 0.5) * 10;
    return cents > 10 ? cents - 1 : 9;
}

bool Inventory::IsSealedTerminal(uint64_t itemId)
{
    Item *item = FindItem(itemId);
    return item && item->GetDesc().quality != Quality::Volatile && m_itemSchema.VolatileContainerType(item->DefIndex()) == 1;
}

bool Inventory::IsActiveTerminal(uint64_t itemId)
{
    Item *item = FindItem(itemId);
    return item && item->GetDesc().quality == Quality::Volatile;
}

Item *Inventory::TerminalOffer(uint64_t containerId)
{
    for (auto &[itemId, item] : m_items)
    {
        if (ItemCasketId(item) == containerId)
        {
            return &item;
        }
    }
    return nullptr;
}

InventoryChangeMessages Inventory::UnsealTerminal(uint64_t terminalId, uint64_t &containerId)
{
    containerId = 0;
    Item *terminal = FindItem(terminalId);
    if (!terminal)
    {
        return {};
    }

    ItemDesc desc{};
    if (!m_itemSchema.GetItemDesc(terminal->DefIndex(), ItemOriginCrate, UnacknowledgedFoundInCrate, desc))
    {
        return {};
    }

    desc.quality = Quality::Volatile;
    desc.attributes.emplace_back(AttributeDefIndex::IsRental, 1u);
    desc.attributes.emplace_back(AttributeDefIndex::ExpirationDate, static_cast<uint32_t>(time(nullptr)) + TerminalSeconds);

    InventoryModify modify;
    Item &container = CreateItem(modify, std::move(desc));
    containerId = container.FullIdFor(AccountId());

    if (GetConfig().m_destroyUsedItems)
    {
        DestroyItem(modify, terminal);
    }

    Platform::Print("Terminal {} unsealed: {}, {} offers, {} hours\n", FromEnum(container.DefIndex()), containerId,
        TerminalOffers, TerminalSeconds / 3600);
    return FinishChanges(modify, k_EGCItemCustomizationNotification_UnlockCrate, { containerId });
}

InventoryChangeMessages Inventory::NextTerminalOffer(uint64_t containerId, uint32_t priceLimit)
{
    Item *container = FindItem(containerId);
    if (!container || container->GetDesc().quality != Quality::Volatile)
    {
        Platform::Print("Terminal: {} is not an active terminal\n", containerId);
        return {};
    }

    bool started = container->HasAttribute(AttributeDefIndex::QuestPointsRemaining);
    int32_t number = static_cast<int32_t>(container->GetAttributeValue<uint32_t>(AttributeDefIndex::QuestPointsRemaining));

    InventoryModify modify;

    // asked again after the final offer: the terminal is done (it takes the offer with it)
    if (started && number <= 0)
    {
        Platform::Print("Terminal {}: final offer declined, terminal closed\n", containerId);
        DestroyItem(modify, container);
        return FinishChanges(modify, k_EGCItemCustomizationNotification_CasketContents, { containerId });
    }

    if (Item *declined = TerminalOffer(containerId))
    {
        DestroyItem(modify, declined);
    }

    // roll from the terminal's collection, staying under the price limit if one is set
    CaseOpening caseOpening{ m_itemSchema };
    ItemDesc offer{};
    uint32_t price = 0;
    bool found = false;
    for (int attempt = 0; attempt < 64; attempt++)
    {
        ItemDesc candidate{};
        if (!caseOpening.SelectItemFromCrate(container->DefIndex(), candidate))
        {
            break;
        }

        uint32_t candidatePrice = TerminalOfferPrice(candidate);
        bool fits = !priceLimit || candidatePrice <= priceLimit;
        if (!found || fits || candidatePrice < price)
        {
            offer = std::move(candidate);
            price = candidatePrice;
            found = true;
        }
        if (fits)
        {
            break;
        }
    }

    if (!found)
    {
        Platform::Print("Terminal {}: no loot list for def {}\n", containerId, FromEnum(container->DefIndex()));
        return {};
    }

    uint32_t next = static_cast<uint32_t>(number < 0 ? -number : number) + 1;
    int32_t stored = next >= TerminalOffers ? -static_cast<int32_t>(next) : static_cast<int32_t>(next);

    // the collection catalog: knives and gloves all count as the one gold item
    uint64_t fauxId = (offer.quality == Quality::Unusual)
        ? FauxItemId(FromEnum(container->DefIndex()), 1, 3)
        : FauxItemId(FromEnum(offer.defIndex), AttributeAsUint32(offer, AttributeDefIndex::TexturePrefab));
    AccountState::Volatile &history = m_account.volatileHistory[FromEnum(container->DefIndex())];
    if (std::none_of(history.seen.begin(), history.seen.end(), [&](const auto &seen) { return seen.first == fauxId; }))
    {
        history.seen.emplace_back(fauxId, static_cast<uint32_t>(time(nullptr)));
        modify.MarkAccountChanged(AccountChangeVolatile);
    }

    offer.inventory = 1; // stored, not in the backpack
    offer.origin = ItemOriginPurchased;
    offer.attributes.emplace_back(AttributeDefIndex::PurchasePrice, price);
    Item &item = CreateItem(modify, std::move(offer));
    modify.SetItemAttribute(item, AttributeDefIndex::CasketIdLow, static_cast<uint32_t>(containerId & UINT32_MAX));
    modify.SetItemAttribute(item, AttributeDefIndex::CasketIdHigh, static_cast<uint32_t>(containerId >> 32));
    modify.SetItemAttribute(*container, AttributeDefIndex::QuestPointsRemaining, static_cast<uint32_t>(stored));

    Platform::Print("Terminal {}: offer {}{} def {} rarity {} for {} (limit {})\n", containerId, next,
        stored < 0 ? " (final)" : "", FromEnum(item.DefIndex()), FromEnum(item.GetRarity()), price, priceLimit);
    return FinishChanges(modify, k_EGCItemCustomizationNotification_CasketContents, { containerId });
}

InventoryChangeMessages Inventory::TerminalContents(uint64_t containerId)
{
    InventoryModify modify;
    return FinishChanges(modify, k_EGCItemCustomizationNotification_CasketContents, { containerId });
}

uint64_t Inventory::TerminalOfferFor(uint32_t terminalDef, uint64_t containerId, uint64_t cost, uint64_t *foundContainer)
{
    auto check = [&](uint64_t id, bool byCost) -> uint64_t {
        Item *container = FindItem(id);
        if (!container || container->GetDesc().quality != Quality::Volatile || FromEnum(container->DefIndex()) != terminalDef)
        {
            return 0;
        }
        Item *offer = TerminalOffer(id);
        if (offer && byCost && offer->GetAttributeValue<uint32_t>(AttributeDefIndex::PurchasePrice) != cost)
        {
            offer = nullptr;
        }
        if (offer && foundContainer)
        {
            *foundContainer = id;
        }
        return offer ? offer->FullIdFor(AccountId()) : 0;
    };

    if (uint64_t offerId = containerId ? check(containerId, false) : 0)
    {
        return offerId;
    }

    // the store line item didn't say which terminal: the one of that def whose offer costs that much
    for (const auto &[itemId, item] : m_items)
    {
        if (uint64_t offerId = check(itemId, true))
        {
            return offerId;
        }
    }
    return 0;
}

InventoryChangeMessages Inventory::BuyTerminalOffer(uint64_t containerId, uint64_t &offerId)
{
    offerId = 0;
    Item *container = FindItem(containerId);
    Item *offer = TerminalOffer(containerId);
    if (!container || !offer)
    {
        Platform::Print("Terminal {}: nothing to buy\n", containerId);
        return {};
    }

    offerId = offer->FullIdFor(AccountId());

    InventoryModify modify;
    modify.RemoveItemAttributes(*offer, { AttributeDefIndex::CasketIdLow, AttributeDefIndex::CasketIdHigh });
    modify.SetItemAttribute(*offer, AttributeDefIndex::PurchaserAccountId, AccountId());
    modify.SetItemInventory(*offer, InventoryUnacknowledged(UnacknowledgedPurchased));
    DestroyItem(modify, container);

    Platform::Print("Terminal {}: bought {} for {}\n", containerId, offerId,
        offer->GetAttributeValue<uint32_t>(AttributeDefIndex::PurchasePrice));
    return FinishChanges(modify);
}

InventoryChangeMessages Inventory::ClaimTerminalReward(uint32_t terminalDef, uint32_t reward)
{
    AccountState::Volatile &history = m_account.volatileHistory[terminalDef];
    if (std::any_of(history.claimed.begin(), history.claimed.end(), [&](const auto &claimed) { return claimed.first == reward; }))
    {
        Platform::Print("Terminal {}: reward {} already claimed\n", terminalDef, reward);
        return {};
    }

    // the amounts are a guess: more for rarer tiers, the most for the gold item
    uint32_t xp = (reward == 99) ? 3000 : 500 * (reward > 2 ? reward - 2 : 1);

    InventoryModify modify;
    history.claimed.emplace_back(reward, static_cast<uint32_t>(time(nullptr)));
    modify.MarkAccountChanged(AccountChangeVolatile);
    GrantXp(modify, xp, std::format("terminal {} collection tier {}", terminalDef, reward));

    InventoryChangeMessages messages = FinishChanges(modify);
    messages.levelChanged = true;
    return messages;
}

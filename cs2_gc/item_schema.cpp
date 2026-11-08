#include "stdafx.h"
#include "item_schema.h"
#include "case_opening.h"
#include "config.h"
#include "item.h"
#include "keyvalue.h"
#include "mod_loader.h"
#include "random.h"
#include "vpk.h"

constexpr const char *ItemSchemaVpkPath = "scripts/items/items_game.txt";

// items_game.txt: a loose file if item_schema_path is set in config.txt,
// otherwise read straight out of game\csgo\pak01_dir.vpk (read only)
// text is the raw items_game.txt, for the parts KeyValueRead can't keep (it merges repeated keys)
static bool MergeItemSchema(KeyValueRead &read, std::string &text)
{
    const std::string &override = GetConfig().m_itemSchemaPath;
    if (!override.empty())
    {
        if (FILE *f = Platform::OpenFile(override, "rb"))
        {
            char buffer[65536];
            size_t got;
            while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
            {
                text.append(buffer, got);
            }
            fclose(f);
        }
        return read.MergeToFromFile(override.c_str());
    }

    std::vector<char> data;

    // the client reads items_game.txt from the mods vpk when a mod adds items, read the same file
    // so the gc knows their rarity, wear range and loot lists
    const std::string &modsVpk = ModLoader::BuiltVpkPath();
    if (!modsVpk.empty() && VpkReadFile(modsVpk, ItemSchemaVpkPath, data))
    {
        Platform::Print("Loaded {} from {} (mods, {} bytes)\n", ItemSchemaVpkPath, modsVpk, data.size());
        text.assign(data.data(), data.size());
        return read.MergeToFromBuffer(ItemSchemaVpkPath, data.data(), data.size());
    }

    std::string vpkPath = Platform::GamePath("pak01_dir.vpk");
    if (!VpkReadFile(vpkPath, ItemSchemaVpkPath, data))
    {
        Platform::Print("Could not read {} from {}\n", ItemSchemaVpkPath, vpkPath);
        return false;
    }

    Platform::Print("Loaded {} from {} ({} bytes)\n", ItemSchemaVpkPath, vpkPath, data.size());
    text.assign(data.data(), data.size());
    return read.MergeToFromBuffer(ItemSchemaVpkPath, data.data(), data.size());
}

// ideally this would get parsed from the item schema...
static Rarity ItemRarityFromString(std::string_view name)
{
    const std::pair<std::string_view, Rarity> rarityNames[] = {
        { "default", Rarity::Default },
        { "common", Rarity::Common },
        { "uncommon", Rarity::Uncommon },
        { "rare", Rarity::Rare },
        { "mythical", Rarity::Mythical },
        { "legendary", Rarity::Legendary },
        { "ancient", Rarity::Ancient },
        { "immortal", Rarity::Immortal },
        { "unusual", Rarity::Unusual },
    };

    for (const auto &pair : rarityNames)
    {
        if (pair.first == name)
        {
            return pair.second;
        }
    }

    assert(false);
    return Rarity::Common;
}

// ideally this would get parsed from the item schema...
static Quality ItemQualityFromString(std::string_view name)
{
    const std::pair<std::string_view, Quality> qualityNames[] = {
        { "normal", Quality::Normal },
        { "genuine", Quality::Genuine },
        { "vintage", Quality::Vintage },
        { "unusual", Quality::Unusual },
        { "unique", Quality::Unique },
        { "community", Quality::Community },
        { "developer", Quality::Developer },
        { "selfmade", Quality::Selfmade },
        { "customized", Quality::Customized },
        { "strange", Quality::Strange },
        { "completed", Quality::Completed },
        { "haunted", Quality::Haunted },
        { "tournament", Quality::Tournament },
    };

    for (const auto &pair : qualityNames)
    {
        if (pair.first == name)
        {
            return pair.second;
        }
    }

    assert(false);
    return Quality::Unique; // i guess???
}

static std::optional<AttributeType> AttributeTypeFromKeyValue(const KeyValue &key)
{
    std::string_view type = key.GetString("attribute_type");
    if (type.size())
    {
        if (type == "float")
        {
            return AttributeType::Float;
        }
        else if (type == "uint32")
        {
            return AttributeType::Uint32;
        }
        else if (type == "string")
        {
            return AttributeType::String;
        }
        else if (type == "vector")
        {
            // only static attributes of the AUG/SG 553 scopes use it, never stored on items
            return std::nullopt;
        }
        else
        {
            // not supported, fall back to float
            Platform::Print("Unsupported attribute type {}\n", type);
            return std::nullopt;
        }
    }

    bool integer = key.GetNumber<int>("stored_as_integer");
    return integer ? AttributeType::Uint32 : AttributeType::Float;
}

ItemInfo::ItemInfo(ItemDefIndex defIndex)
    : m_defIndex{ defIndex }
    , m_rarity{ Rarity::Common }
    , m_quality{ Quality::Normal }
    , m_level{ 1 }
    , m_isCoupon{ false }
    , m_isSelfOpeningPurchase{ false }
    , m_storageIcon{ 0 }
    , m_willProduceStatTrak{ false }
{
    // RecursiveParseItem parses the rest
}

PaintKitInfo::PaintKitInfo(const KeyValue &key)
    : m_defIndex{ FromString<uint32_t>(key.Name()) }
    , m_rarity{ Rarity::Common } // rarity is not set here, done in ParsePaintKitRarities
{
    m_minFloat = key.GetNumber<float>("wear_remap_min", 0.0f);
    m_maxFloat = key.GetNumber<float>("wear_remap_max", 1.0f);
    m_descriptionTag = key.GetString("description_tag");
}

StickerKitInfo::StickerKitInfo(const KeyValue &key)
    : m_defIndex{ FromString<uint32_t>(key.Name()) }
    , m_rarity{ Rarity::Common }
{
    std::string_view rarity = key.GetString("item_rarity");
    if (rarity.size())
    {
        m_rarity = ItemRarityFromString(rarity);
    }
}

MusicDefinitionInfo::MusicDefinitionInfo(const KeyValue &key)
    : m_defIndex{ FromString<uint32_t>(key.Name()) }
{
    assert(m_defIndex);
}

Rarity LootListItem::CaseRarity() const
{
    if (quality == Quality::Unusual)
    {
        return Rarity::Unusual;
    }

    return rarity;
}

ItemSchema::ItemSchema()
{
    // we want to load the extra item schema first
    std::string extraPath = Platform::DataPath("items_extra.txt");
    KeyValueRead itemSchemaRead{ extraPath.c_str() };
    if (!itemSchemaRead.Good())
    {
        // not fatal: items_game.txt alone is enough for inventories, equips and most cases,
        // only the hand-made extras (unusual loot lists etc.) are missing then
        Platform::Print("{} is missing, knives/gloves won't drop from cases\n", extraPath);
    }

    // append the public item schema (with the mods' items if CSForge built some)
    std::string itemsGameText;
    if (!MergeItemSchema(itemSchemaRead, itemsGameText))
    {
        Platform::Error("Could not load scripts/items/items_game.txt from the game files.\n"
                        "Check that -cs2path points at the CS2 install folder.");
    }

    const KeyValue &itemSchema = itemSchemaRead.Root();

    const KeyValue *itemsGame = itemSchema.GetSubkey("items_game");
    if (!itemsGame)
    {
        assert(false);
        return;
    }

    const KeyValue *attributesKey = itemsGame->GetSubkey("attributes");
    if (attributesKey)
    {
        ParseAttributes(attributesKey);
    }

    const KeyValue *itemsKey = itemsGame->GetSubkey("items");
    if (itemsKey)
    {
        ParseItems(itemsKey, itemsGame->GetSubkey("prefabs"));
    }

    // FIXME: abysmal dogshit
    const KeyValue *alternateIcons2 = itemsGame->GetSubkey("alternate_icons2");
    const KeyValue *casketIcons = alternateIcons2 ? alternateIcons2->GetSubkey("casket_icons") : nullptr;
    if (casketIcons)
    {
        for (auto &pair : m_itemInfo)
        {
            for (const KeyValue &icon : casketIcons->Subkeys())
            {
                if (pair.second.m_imageInventory == icon.GetString("icon_path"))
                {
                    pair.second.m_storageIcon = FromString<uint32_t>(icon.Name());
                }
            }
        }
    }

    const KeyValue *stickerKitsKey = itemsGame->GetSubkey("sticker_kits");
    if (stickerKitsKey)
    {
        ParseStickerKits(stickerKitsKey);
    }

    const KeyValue *paintKitsKey = itemsGame->GetSubkey("paint_kits");
    if (paintKitsKey)
    {
        ParsePaintKits(paintKitsKey);
    }

    const KeyValue *paintKitsRarityKey = itemsGame->GetSubkey("paint_kits_rarity");
    if (paintKitsRarityKey)
    {
        ParsePaintKitRarities(paintKitsRarityKey);
    }

    const KeyValue *musicDefinitionsKey = itemsGame->GetSubkey("music_definitions");
    if (musicDefinitionsKey)
    {
        ParseMusicDefinitions(musicDefinitionsKey);
    }

    const KeyValue *keychainDefinitionsKey = itemsGame->GetSubkey("keychain_definitions");
    if (keychainDefinitionsKey)
    {
        ParseKeychainDefinitions(keychainDefinitionsKey);
    }

    // before the loot lists, souvenir packages reference these
    const KeyValue *highlightReelsKey = itemsGame->GetSubkey("highlight_reels");
    if (highlightReelsKey)
    {
        ParseHighlightReels(highlightReelsKey);
    }

    const KeyValue *lootListsKey = itemsGame->GetSubkey("client_loot_lists");
    if (lootListsKey)
    {
        ParseLootLists(lootListsKey);
    }

    const KeyValue *revolvingLootListsKey = itemsGame->GetSubkey("revolving_loot_lists");
    if (revolvingLootListsKey)
    {
        ParseRevolvingLootLists(revolvingLootListsKey);
    }

    // after the crates, collections get linked to their case's knives/gloves
    const KeyValue *itemSetsKey = itemsGame->GetSubkey("item_sets");
    if (itemSetsKey)
    {
        ParseCollections(itemSetsKey);
    }

    ParseXpShop(itemsGameText);
}

// KeyValues text tokens: quoted strings (with \ escapes kept as they are), bare words, { and }.
// // comments and [$platform] conditions are dropped. Stops after the first block closes.
static void TokenizeKeyValues(std::string_view text, std::vector<std::string_view> &tokens)
{
    size_t i = 0;
    size_t counted = tokens.size();
    int depth = 0;
    while (i < text.size())
    {
        for (; counted < tokens.size(); counted++)
        {
            if (tokens[counted] == "{" || tokens[counted] == "}")
            {
                depth += tokens[counted] == "{" ? 1 : -1;
                if (depth == 0)
                {
                    return;
                }
            }
        }

        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            i++;
        }
        else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            i = text.find('\n', i);
            i = i == std::string_view::npos ? text.size() : i;
        }
        else if (c == '{' || c == '}')
        {
            tokens.push_back(text.substr(i++, 1));
        }
        else if (c == '[')
        {
            i = text.find(']', i);
            i = i == std::string_view::npos ? text.size() : i + 1;
        }
        else if (c == '"')
        {
            size_t start = ++i;
            while (i < text.size() && text[i] != '"')
            {
                i += text[i] == '\\' ? 2 : 1;
            }
            tokens.push_back(text.substr(start, (std::min)(i, text.size()) - start));
            i++;
        }
        else
        {
            size_t start = i;
            while (i < text.size() && !strchr(" \t\r\n{}\"", text[i]))
            {
                i++;
            }
            tokens.push_back(text.substr(start, i - start));
        }
    }
}

// "seasonaloperations" from the raw text: KeyValueRead merges the repeated
// "operational_point_redeemable" blocks into one, and the shop needs every one of them in order
void ItemSchema::ParseXpShop(std::string_view itemsGameText)
{
    // items_game.txt has one "seasonaloperations" section per season
    constexpr std::string_view SectionName = "\"seasonaloperations\"";
    for (size_t at = itemsGameText.find(SectionName); at != std::string_view::npos; at = itemsGameText.find(SectionName, at + 1))
    {
        std::vector<std::string_view> tokens;
        TokenizeKeyValues(itemsGameText.substr(at), tokens);
        ParseXpShopSection(tokens);
    }
}

void ItemSchema::ParseXpShopSection(std::span<const std::string_view> tokens)
{

    // tokens[0] is the section name, then { season { ... } season { ... } }
    size_t pos = 1;
    auto skipBlock = [&]() {
        for (int depth = 0; pos < tokens.size();)
        {
            std::string_view token = tokens[pos++];
            depth += token == "{" ? 1 : token == "}" ? -1 : 0;
            if (depth == 0)
            {
                return;
            }
        }
    };

    if (pos >= tokens.size() || tokens[pos] != "{")
    {
        return;
    }
    pos++;

    while (pos + 1 < tokens.size() && tokens[pos] != "}")
    {
        std::string_view season = tokens[pos++];
        if (tokens[pos] != "{")
        {
            pos++; // a plain value, not a season
            continue;
        }
        pos++;

        bool xpShop = false;
        std::vector<XpShopEntry> entries;
        while (pos + 1 < tokens.size() && tokens[pos] != "}")
        {
            std::string_view key = tokens[pos++];
            if (tokens[pos] != "{")
            {
                xpShop |= key == "redeemable_goods" && tokens[pos] == "xpshop";
                pos++;
                continue;
            }

            if (key != "operational_point_redeemable")
            {
                skipBlock();
                continue;
            }

            pos++;
            XpShopEntry entry{};
            while (pos + 1 < tokens.size() && tokens[pos] != "}")
            {
                std::string_view field = tokens[pos++];
                if (tokens[pos] == "{")
                {
                    skipBlock();
                    continue;
                }

                std::string_view value = tokens[pos++];
                if (field == "points")
                {
                    entry.points = FromString<uint32_t>(value);
                }
                else if (field == "item_name")
                {
                    entry.itemName = value;
                }
            }
            pos++; // }
            entry.redeemId = Crc32(entry.itemName);
            entries.push_back(std::move(entry));
        }
        pos++; // }

        if (xpShop)
        {
            m_xpShop[FromString<uint32_t>(season)] = std::move(entries);
        }
    }
}

std::span<const ItemSchema::XpShopEntry> ItemSchema::GetXpShopEntries(uint32_t campaign) const
{
    auto it = m_xpShop.find(campaign);
    if (it == m_xpShop.end() && m_xpShop.size() == 1)
    {
        it = m_xpShop.begin(); // only one Armory season exists, take it whatever the client calls it
    }

    if (it == m_xpShop.end())
    {
        return {};
    }

    return it->second;
}

const ItemSchema::XpShopEntry *ItemSchema::GetXpShopEntry(uint32_t campaign, uint32_t redeemId) const
{
    for (const XpShopEntry &entry : GetXpShopEntries(campaign))
    {
        if (entry.redeemId == redeemId)
        {
            return &entry;
        }
    }

    return nullptr;
}

uint32_t ItemSchema::VolatileContainerType(ItemDefIndex defIndex) const
{
    return StaticAttributeUint32(defIndex, AttributeDefIndex::VolatileContainer);
}

uint32_t ItemSchema::StaticAttributeUint32(ItemDefIndex defIndex, AttributeDefIndex attributeIndex) const
{
    auto it = m_itemInfo.find(defIndex);
    if (it == m_itemInfo.end())
    {
        return 0;
    }

    for (const ItemAttributeInfo &attribute : it->second.m_attributes)
    {
        if (attribute.defIndex == attributeIndex)
        {
            return std::visit([](const auto &value) -> uint32_t {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return FromString<uint32_t>(value);
                }
                else
                {
                    return static_cast<uint32_t>(value);
                }
            }, attribute.value);
        }
    }

    return 0;
}

const LootList *ItemSchema::GetLootList(std::string_view name) const
{
    auto it = m_lootLists.find(name);
    return it != m_lootLists.end() ? &it->second : nullptr;
}

std::vector<ItemSchema::PaintedItem> ItemSchema::PaintedItems() const
{
    // every list's own items: sub lists are lists of the map too, so nothing is missed
    std::map<std::pair<uint32_t, uint32_t>, PaintedItem> unique;
    for (const auto &[name, lootList] : m_lootLists)
    {
        for (const LootListItem &item : lootList.items)
        {
            if (item.type != LootListItemPaintable || !item.kitDefIndex || item.rarity < Rarity::Common || item.rarity > Rarity::Immortal)
            {
                continue;
            }

            PaintedItem painted{ item.itemInfo->m_defIndex, item.kitDefIndex, item.rarity, item.quality, item.minFloat, item.maxFloat };
            unique.try_emplace({ FromEnum(painted.defIndex), painted.paintKit }, painted);
        }
    }

    std::vector<PaintedItem> out;
    out.reserve(unique.size());
    for (const auto &[key, painted] : unique)
    {
        out.push_back(painted);
    }
    return out;
}

const LootListItem *ItemSchema::FindPaintedLootListItem(ItemDefIndex defIndex, uint32_t paintKit) const
{
    for (const auto &[name, lootList] : m_lootLists)
    {
        for (const LootListItem &item : lootList.items)
        {
            if (item.type == LootListItemPaintable && item.itemInfo->m_defIndex == defIndex && item.kitDefIndex == paintKit)
            {
                return &item;
            }
        }
    }
    return nullptr;
}

std::string_view ItemSchema::ItemNameToken(ItemDefIndex defIndex) const
{
    auto it = m_itemInfo.find(defIndex);
    return it != m_itemInfo.end() ? std::string_view{ it->second.m_itemNameToken } : std::string_view{};
}

std::string_view ItemSchema::PaintKitNameToken(uint32_t paintKit) const
{
    const PaintKitInfo *info = PaintKitInfoById(paintKit);
    return info ? std::string_view{ info->m_descriptionTag } : std::string_view{};
}

std::string_view ItemSchema::ItemDefName(ItemDefIndex defIndex) const
{
    auto it = m_itemInfo.find(defIndex);
    return it != m_itemInfo.end() ? std::string_view{ it->second.m_name } : std::string_view{};
}

std::optional<ItemDefIndex> ItemSchema::ItemDefIndexByName(std::string_view name) const
{
    for (const auto &[defIndex, info] : m_itemInfo)
    {
        if (info.m_name == name)
        {
            return defIndex;
        }
    }

    return std::nullopt;
}

Rarity ItemSchema::KeychainRarity(uint32_t keychainId) const
{
    for (const auto &[name, info] : m_keychainInfo)
    {
        if (info.m_defIndex == keychainId)
        {
            return info.m_rarity;
        }
    }

    return Rarity::Common;
}

static uint64_t CollectionKey(ItemDefIndex defIndex, uint32_t paintKit)
{
    return (static_cast<uint64_t>(FromEnum(defIndex)) << 32) | paintKit;
}

void ItemSchema::ParseCollections(const KeyValue *itemSetsKey)
{
    for (const KeyValue &setKey : itemSetsKey->Subkeys())
    {
        const KeyValue *itemsKey = setKey.GetSubkey("items");
        if (!setKey.GetNumber<int>("is_collection") || !itemsKey)
        {
            continue;
        }

        Collection collection;
        collection.name = setKey.Name();

        for (const KeyValue &entryKey : itemsKey->Subkeys())
        {
            LootListItem item;
            if (ParseLootListItem(item, entryKey.Name()) && item.type == LootListItemPaintable)
            {
                collection.items.push_back(item);
            }
        }

        if (collection.items.empty())
        {
            continue;
        }

        uint32_t index = static_cast<uint32_t>(m_collections.size());
        for (const LootListItem &item : collection.items)
        {
            m_collectionByItem.try_emplace(CollectionKey(item.itemInfo->m_defIndex, item.kitDefIndex), index);
        }

        m_collections.push_back(std::move(collection));
    }

    for (const auto &[series, lootList] : m_revolvingLootLists)
    {
        LinkCollectionToCrate(lootList);
    }
}

static void CollectCrateLists(const LootList &lootList, std::vector<const LootList *> &unusualLists, const LootListItem *&regularItem)
{
    if (lootList.isUnusual)
    {
        unusualLists.push_back(&lootList);
        return;
    }

    for (const LootList *subList : lootList.subLists)
    {
        CollectCrateLists(*subList, unusualLists, regularItem);
    }

    if (!regularItem)
    {
        for (const LootListItem &item : lootList.items)
        {
            if (item.type == LootListItemPaintable && item.quality != Quality::Unusual)
            {
                regularItem = &item;
                break;
            }
        }
    }
}

void ItemSchema::LinkCollectionToCrate(const LootList &crateLootList)
{
    std::vector<const LootList *> unusualLists;
    const LootListItem *regularItem = nullptr;
    CollectCrateLists(crateLootList, unusualLists, regularItem);

    if (unusualLists.empty() || !regularItem)
    {
        return;
    }

    auto it = m_collectionByItem.find(CollectionKey(regularItem->itemInfo->m_defIndex, regularItem->kitDefIndex));
    if (it == m_collectionByItem.end())
    {
        return;
    }

    // first case wins (e.g. Gallery and Kilowatt share the Kukri list anyway)
    Collection &collection = m_collections[it->second];
    if (collection.unusualLists.empty())
    {
        collection.unusualLists = std::move(unusualLists);
    }
}

const ItemSchema::Collection *ItemSchema::CollectionForItem(ItemDefIndex defIndex, uint32_t paintKit) const
{
    auto it = m_collectionByItem.find(CollectionKey(defIndex, paintKit));
    return (it != m_collectionByItem.end()) ? &m_collections[it->second] : nullptr;
}

const PaintKitInfo *ItemSchema::PaintKitInfoById(uint32_t paintKit) const
{
    for (const auto &[name, info] : m_paintKitInfo)
    {
        if (info.m_defIndex == paintKit)
        {
            return &info;
        }
    }

    return nullptr;
}

static void CollectUnusualItems(const LootList &lootList, bool knivesOnly, std::vector<const LootListItem *> &out)
{
    for (const LootList *subList : lootList.subLists)
    {
        CollectUnusualItems(*subList, knivesOnly, out);
    }

    for (const LootListItem &item : lootList.items)
    {
        // knives are def indices 500-526ish, gloves start at 4725 and can't be stattrak
        if (!knivesOnly || item.itemInfo->m_defIndex < ToEnum<ItemDefIndex>(1000u))
        {
            out.push_back(&item);
        }
    }
}

bool ItemSchema::SelectTradeUpOutput(std::span<const TradeUpInput> inputs, Rarity inputRarity,
    bool unusualOutput, bool statTrak, ItemDesc &desc) const
{
    Rarity outputRarity = static_cast<Rarity>(FromEnum(inputRarity) + 1);

    // every input picks its own collection with equal chance, then one of that
    // collection's outcomes; inputs whose collection has no outcome don't count
    std::vector<std::vector<const LootListItem *>> outcomes;
    float wearSum = 0.0f;

    for (const TradeUpInput &input : inputs)
    {
        // wear normalized to the input's own range (cs2 2025 rules)
        const PaintKitInfo *paintKit = PaintKitInfoById(input.paintKit);
        float range = paintKit ? (paintKit->m_maxFloat - paintKit->m_minFloat) : 1.0f;
        float normalized = (range > 0.0f && paintKit) ? (input.wear - paintKit->m_minFloat) / range : input.wear;
        wearSum += std::clamp(normalized, 0.0f, 1.0f);

        const Collection *collection = CollectionForItem(input.defIndex, input.paintKit);
        if (!collection)
        {
            continue;
        }

        std::vector<const LootListItem *> candidates;
        if (unusualOutput)
        {
            for (const LootList *unusualList : collection->unusualLists)
            {
                CollectUnusualItems(*unusualList, statTrak, candidates);
            }
        }
        else
        {
            for (const LootListItem &item : collection->items)
            {
                if (item.rarity == outputRarity)
                {
                    candidates.push_back(&item);
                }
            }
        }

        if (!candidates.empty())
        {
            outcomes.push_back(std::move(candidates));
        }
    }

    if (outcomes.empty())
    {
        Platform::Print("Trade up: none of the inputs' collections have an outcome\n");
        return false;
    }

    std::span<const LootListItem *const> pickedOutcomes = Random::Select(std::span{ outcomes });
    const LootListItem *output = SelectWeightedLootListItem(pickedOutcomes);

    if (!ItemDescForLootListItem(*output, statTrak, ItemOriginCrafted, UnacknowledgedCrafted, desc))
    {
        return false;
    }

    // output wear = average normalized input wear mapped onto the output's range
    float wear = output->minFloat + (wearSum / inputs.size()) * (output->maxFloat - output->minFloat);
    for (ItemAttribute &attribute : desc.attributes)
    {
        if (attribute.DefIndex() == AttributeDefIndex::TextureWear)
        {
            attribute.Value() = wear;
        }
    }

    return true;
}

AttributeType ItemSchema::GetAttributeType(AttributeDefIndex defIndex) const
{
    auto it = m_attributeTypes.find(defIndex);
    if (it == m_attributeTypes.end())
    {
        assert(false);
        return AttributeType::Float;
    }

    return it->second;
}

// garbage to allow lookup from both const and non const item info
static auto GetAttributeInfo(auto &itemInfo, AttributeDefIndex defIndex)
{
    for (auto &attribute : itemInfo.m_attributes)
    {
        if (attribute.defIndex == defIndex)
        {
            return &attribute;
        }
    }

    return static_cast<decltype(itemInfo.m_attributes.data())>(nullptr);
}

static uint32_t SupplyCrateSeriesFromItemInfo(const ItemInfo& info)
{
    const ItemAttributeInfo *attribute = GetAttributeInfo(info, AttributeDefIndex::SupplyCrateSeries);
    if (!attribute)
    {
        return 0;
    }

    // this can be either a float or an int
    if (std::holds_alternative<float>(attribute->value))
    {
        return static_cast<uint32_t>(std::get<float>(attribute->value));
    }
    else if (std::holds_alternative<uint32_t>(attribute->value))
    {
        return std::get<uint32_t>(attribute->value);
    }

    assert(0);
    return 0;
}

const LootList *ItemSchema::GetCrateLootList(ItemDefIndex crateDefIndex) const
{
    auto itemSearch = m_itemInfo.find(crateDefIndex);
    if (itemSearch == m_itemInfo.end())
    {
        assert(false);
        return nullptr;
    }

    uint32_t supplyCrateSeries = SupplyCrateSeriesFromItemInfo(itemSearch->second);
    if (!supplyCrateSeries)
    {
        assert(false);
        return nullptr;
    }

    auto lootListSearch = m_revolvingLootLists.find(supplyCrateSeries);
    if (lootListSearch == m_revolvingLootLists.end())
    {
        assert(false);
        return nullptr;
    }

    return &lootListSearch->second;
}


uint32_t ItemSchema::GetCrateSupplyCrateSeries(ItemDefIndex crateDefIndex) const
{
    auto itemSearch = m_itemInfo.find(crateDefIndex);
    if (itemSearch == m_itemInfo.end())
    {
        assert(false);
        return 0;
    }

    return SupplyCrateSeriesFromItemInfo(itemSearch->second);
}

bool ItemSchema::ItemDescForLootListItem(
    const LootListItem &lootListItem,
    bool statTrak,
    ItemOrigin origin,
    UnacknowledgedType unacknowledgedType,
    ItemDesc &desc) const
{
    if (!GetItemDesc(lootListItem.itemInfo->m_defIndex, origin, unacknowledgedType, desc))
    {
        assert(false);
        return false;
    }

    // quality override, stattrak makes it strange if it's not an unusual
    if (statTrak && lootListItem.quality != Quality::Unusual)
    {
        desc.quality = Quality::Strange;
    }
    else
    {
        desc.quality = lootListItem.quality;
    }

    // weapon prefabs say "item_quality" "normal" (the stock gun), a painted one from a case, a
    // contract or a drop is unique like the real GC makes it: the trade up recipes ask for
    // "*quality" "unique,tournament" (strange for StatTrak), normal ones can't go in a contract
    if (desc.quality == Quality::Normal && lootListItem.type == LootListItemPaintable)
    {
        desc.quality = Quality::Unique;
    }

    // rarity override
    assert(lootListItem.rarity >= Rarity::Common && lootListItem.rarity <= Rarity::Immortal);
    desc.rarity = lootListItem.rarity;

    // setup type specficic attributes

    if (lootListItem.type == LootListItemSticker || lootListItem.type == LootListItemPatch)
    {
        // mikkotodo anything else?
        desc.attributes.emplace_back(
            AttributeDefIndex::StickerId0,
            lootListItem.kitDefIndex);
    }
    else if (lootListItem.type == LootListItemSpray)
    {
        desc.attributes.emplace_back(
            AttributeDefIndex::StickerId0,
            lootListItem.kitDefIndex);

        // add AttributeSpraysRemaining when it's unsealed (mikkotodo how does the real gc do this)

        desc.attributes.emplace_back(
            AttributeDefIndex::SprayTintId,
            Random::Uint32(FromEnum(GraffitiTint::Min), FromEnum(GraffitiTint::Max)));
    }
    else if (lootListItem.type == LootListItemMusicKit)
    {
        desc.attributes.emplace_back(
            AttributeDefIndex::MusicId,
            lootListItem.kitDefIndex);
    }
    else if (lootListItem.type == LootListItemKeychain)
    {
        desc.attributes.emplace_back(
            AttributeDefIndex::KeychainId0,
            lootListItem.kitDefIndex);
    }
    else if (lootListItem.type == LootListItemPaintable)
    {
        desc.attributes.emplace_back(
            AttributeDefIndex::TexturePrefab,
            static_cast<float>(lootListItem.kitDefIndex));

        desc.attributes.emplace_back(
            AttributeDefIndex::TextureSeed,
            static_cast<float>(Random::Uint32(0, 1000)));

        // mikkotodo how does the float distribution work?
        desc.attributes.emplace_back(
            AttributeDefIndex::TextureWear,
            Random::Float(lootListItem.minFloat, lootListItem.maxFloat));
    }
    else if (lootListItem.type == LootListItemNoAttribute)
    {
        // nothing
    }
    else
    {
        assert(false);
    }

    if (statTrak)
    {
        assert((lootListItem.type == LootListItemMusicKit) || (lootListItem.type == LootListItemPaintable));

        desc.attributes.emplace_back(AttributeDefIndex::KillEater, 0u);

        // mikkotodo fix magic
        uint32_t scoreType = (lootListItem.type == LootListItemMusicKit) ? 1 : 0;
        desc.attributes.emplace_back(AttributeDefIndex::KillEaterScoreType, scoreType);
    }

    return true;
}

bool ItemSchema::GetItemDesc(ItemDefIndex defIndex, ItemOrigin origin, UnacknowledgedType unacknowledgedType, ItemDesc &desc) const
{
    auto itemSearch = m_itemInfo.find(defIndex);
    if (itemSearch == m_itemInfo.end())
    {
        assert(false);
        return false;
    }

    const ItemInfo &itemInfo = itemSearch->second;

    // urgh wtf is this crap
    if (itemInfo.m_isCoupon)
    {
        assert(itemInfo.m_lootListName.size());
        auto lootListSearch = m_lootLists.find(itemInfo.m_lootListName);
        if (lootListSearch == m_lootLists.end())
        {
            assert(false);
            return false;
        }

        const LootList &lootList = lootListSearch->second;
        assert(lootList.subLists.size() == 0 && lootList.items.size() == 1);
        assert(!lootList.willProduceStatTrak && !lootList.isUnusual && !lootList.allEntriesAsAdditionalDrops);

        const LootListItem &lootListItem = lootList.items.front();

        bool statTrak = itemInfo.m_willProduceStatTrak
            && (lootListItem.type == LootListItemMusicKit || lootListItem.type == LootListItemPaintable);

        return ItemDescForLootListItem(
            lootListItem,
            statTrak,
            origin,
            unacknowledgedType,
            desc);
    }

    desc.inventory = InventoryUnacknowledged(unacknowledgedType);
    desc.defIndex = defIndex;
    desc.level = itemInfo.m_level;
    desc.quality = itemInfo.m_quality;
    desc.flags = 0;
    desc.origin = origin;
    desc.inUse = false;
    desc.rarity = itemInfo.m_rarity;

    return true;
}

bool ItemSchema::GetPurchaseItemDescs(ItemDefIndex defIndex, std::vector<ItemDesc> &items, std::vector<ItemDesc> &casketedItems) const
{
    auto itemSearch = m_itemInfo.find(defIndex);
    if (itemSearch == m_itemInfo.end())
    {
        assert(false);
        return false;
    }

    const ItemInfo &info = itemSearch->second;
    if (info.m_isSelfOpeningPurchase)
    {
        auto listSearch = m_lootLists.find(info.m_lootListName);
        if (listSearch == m_lootLists.end())
        {
            assert(false);
            return false;
        }

        // self opening purchases??? CASE OPENING
        CaseOpening caseOpening{ *this };

        if (!caseOpening.SelectItemsFromLootList(
                listSearch->second,
                info.m_willProduceStatTrak,
                ItemOriginPurchased,
                UnacknowledgedPurchased,
                items))
        {
            assert(false);
            return false;
        }

        // rio2022 caskets... this list contains the caskets contents,
        // the actual item we want is <name>_self_opening_purchase
        auto casketList = m_lootLists.find(info.m_name + "_self_opening_purchase");
        if (casketList != m_lootLists.end())
        {
            casketedItems = std::move(items);
            items.clear();

            if (!caseOpening.SelectItemsFromLootList(
                    casketList->second,
                    false,
                    ItemOriginPurchased,
                    UnacknowledgedPurchased,
                    items))
            {
                assert(false);
                return false;
            }

            assert(items.size() == 1);
            ItemDesc &casketDesc = items.front();
            assert(items.front().defIndex == ItemDefIndex::Casket);

            if (info.m_storageIcon)
            {
                // we need to drip out the casket manually
                casketDesc.attributes.emplace_back(AttributeDefIndex::AlternateIcon, info.m_storageIcon);
            }
        }

        return true;
    }

    ItemDesc desc{};
    if (!GetItemDesc(defIndex, ItemOriginPurchased, UnacknowledgedPurchased, desc))
    {
        return false;
    }

    items.push_back(std::move(desc));
    return true;
}

void ItemSchema::ParseItems(const KeyValue *itemsKey, const KeyValue *prefabsKey)
{
    m_itemInfo.reserve(itemsKey->Subkeys().size());

    for (const KeyValue &itemKey : itemsKey->Subkeys())
    {
        if (itemKey.Name() == "default")
        {
            // ignore this
            continue;
        }

        ItemDefIndex defIndex = ToEnum<ItemDefIndex>(FromString<uint32_t>(itemKey.Name()));
        auto emplace = m_itemInfo.try_emplace(defIndex, defIndex);

        ParseItemRecursive(emplace.first->second, itemKey, prefabsKey);

        auto &itemInfo = emplace.first->second;
        if (itemInfo.m_isCoupon || itemInfo.m_isSelfOpeningPurchase)
        {
            assert(itemInfo.m_lootListName.size());
        }
    }
}

// i hate my life
static std::vector<std::string_view> SplitString(std::string_view input, char delimiter)
{
    size_t offset = 0;
    std::vector<std::string_view> result;

    while (true)
    {
        size_t i = input.find(delimiter, offset);
        if (i == std::string_view::npos)
        {
            result.emplace_back(input.substr(offset));
            break;
        }

        result.emplace_back(input.substr(offset, i - offset));
        offset = i + 1;
    }

    return result;
}

void ItemSchema::ParseItemRecursive(ItemInfo &info, const KeyValue &itemKey, const KeyValue *prefabsKey)
{
    std::string_view prefabString = itemKey.GetString("prefab");
    if (prefabString.size() && prefabsKey)
    {
        // might have multiple specifications in a single statement
        std::vector<std::string_view> prefabNames = SplitString(prefabString, ' ');
        for (std::string_view prefabName : prefabNames)
        {
            const KeyValue *prefabKey = prefabsKey->GetSubkey(prefabName);
            if (prefabKey)
            {
                ParseItemRecursive(info, *prefabKey, prefabsKey);
            }
            else
            {
                // not available to us mortals...
                Platform::Print("No such prefab '{}'\n", prefabName);
            }
        }
    }

    std::string_view name = itemKey.GetString("name");
    if (name.size())
    {
        info.m_name = name;
    }

    std::string_view itemName = itemKey.GetString("item_name");
    if (itemName.size())
    {
        info.m_itemNameToken = itemName;
    }

    std::string_view image = itemKey.GetString("image_inventory");
    if (image.size())
    {
        info.m_imageInventory = image;
    }

    std::string_view quality = itemKey.GetString("item_quality");
    if (quality.size())
    {
        info.m_quality = ItemQualityFromString(quality);
    }

    std::string_view rarity = itemKey.GetString("item_rarity");
    if (rarity.size())
    {
        info.m_rarity = ItemRarityFromString(rarity);
    }

    uint32_t minLevel = itemKey.GetNumber<uint32_t>("min_ilevel", 0);
    uint32_t maxLevel = itemKey.GetNumber<uint32_t>("max_ilevel", 0);
    if (minLevel && maxLevel)
    {
        assert(minLevel == maxLevel);
        info.m_level = minLevel;
    }

    std::string_view itemType = itemKey.GetString("item_type");
    if (itemType.size())
    {
        info.m_isCoupon = (itemType == "coupon");
        info.m_isSelfOpeningPurchase = (itemType == "self_opening_purchase");
    }

    std::string_view lootListName = itemKey.GetString("loot_list_name");
    if (lootListName.size())
    {
        info.m_lootListName = lootListName;
    }

    info.m_willProduceStatTrak = itemKey.GetNumber("will_produce_stattrak", info.m_willProduceStatTrak);

    const KeyValue *attributes = itemKey.GetSubkey("attributes");
    if (attributes)
    {
        info.m_attributes.reserve(attributes->Subkeys().size());

        for (const KeyValue &attributeKey : attributes->Subkeys())
        {
            ParseItemAttribute(info, attributeKey);
        }
    }
}

void ItemSchema::ParseItemAttribute(ItemInfo &itemInfo, const KeyValue &attributeKey)
{
    auto it = m_attributeDefIndexes.find(attributeKey.Name());
    if (it == m_attributeDefIndexes.end())
    {
        // can't resolve def index, ignore
        Platform::Print("No def index for attribute {}, ignoring\n", attributeKey.Name());
        return;
    }

    // see if an attribute by this def index already exists to use as a base (FIXME: unfuck this)
    ItemAttributeInfo *attribute = GetAttributeInfo(itemInfo, it->second);
    if (!attribute)
    {
        attribute = &itemInfo.m_attributes.emplace_back();
        attribute->defIndex = it->second;
    }

    // 2 representations...
    std::string_view valueString;
    if (attributeKey.Subkeys().size())
    {
        valueString = attributeKey.GetString("value");
    }
    else
    {
        valueString = attributeKey.String();
    }

    if (valueString.size())
    {
        switch (GetAttributeType(attribute->defIndex))
        {
        case AttributeType::Float:
            attribute->value = FromString<float>(valueString);
            break;

        case AttributeType::Uint32:
            // FIXME: old builds need more robust parsing!!! can have signed values or even floats here
            attribute->value = static_cast<uint32_t>(FromString<int>(valueString));
            break;

        case AttributeType::String:
            attribute->value = std::string{ valueString };
            break;
        }
    }
}

void ItemSchema::ParseAttributes(const KeyValue *attributesKey)
{
    m_attributeTypes.reserve(attributesKey->Subkeys().size());
    m_attributeDefIndexes.reserve(attributesKey->Subkeys().size());

    for (const KeyValue &attributeKey : attributesKey->Subkeys())
    {
        uint32_t defIndex = FromString<uint32_t>(attributeKey.Name());
        assert(defIndex);

        std::optional<AttributeType> type = AttributeTypeFromKeyValue(attributeKey);
        if (!type.has_value())
        {
            // unknown type, logged by AttributeTypeFromKeyValue
            continue;
        }

        m_attributeTypes.try_emplace(ToEnum<AttributeDefIndex>(defIndex), *type);

        m_attributeDefIndexes.try_emplace(
            std::string{ attributeKey.GetString("name") },
            ToEnum<AttributeDefIndex>(defIndex));
    }
}

void ItemSchema::ParseStickerKits(const KeyValue *stickerKitsKey)
{
    m_stickerKitInfo.reserve(stickerKitsKey->Subkeys().size());

    for (const KeyValue &stickerKitKey : stickerKitsKey->Subkeys())
    {
        std::string_view name = stickerKitKey.GetString("name");

        m_stickerKitInfo.emplace(std::piecewise_construct,
            std::forward_as_tuple(name),
            std::forward_as_tuple(stickerKitKey));

        // gold team, organizer and autograph stickers go on souvenirs
        uint32_t eventId = stickerKitKey.GetNumber<uint32_t>("tournament_event_id", 0);
        if (eventId && name.ends_with("_gold") && !stickerKitKey.GetString("tournament_team_id").empty())
        {
            uint32_t kit = FromString<uint32_t>(stickerKitKey.Name());
            uint32_t teamId = stickerKitKey.GetNumber<uint32_t>("tournament_team_id", 0);
            TournamentStickers &stickers = m_tournamentStickers[eventId];
            if (stickerKitKey.GetNumber<uint32_t>("tournament_player_id", 0))
            {
                stickers.players[teamId].push_back(kit);
            }
            else if (teamId)
            {
                stickers.teams[teamId] = kit;
            }
            else
            {
                stickers.organizer = kit;
            }
        }
    }
}

const ItemSchema::TournamentStickers *ItemSchema::GetTournamentStickers(uint32_t eventId) const
{
    auto it = m_tournamentStickers.find(eventId);
    return it != m_tournamentStickers.end() ? &it->second : nullptr;
}

void ItemSchema::ParsePaintKits(const KeyValue *paintKitsKey)
{
    m_paintKitInfo.reserve(paintKitsKey->Subkeys().size());

    for (const KeyValue &paintKitKey : paintKitsKey->Subkeys())
    {
        std::string_view name = paintKitKey.GetString("name");

        m_paintKitInfo.emplace(std::piecewise_construct,
            std::forward_as_tuple(name),
            std::forward_as_tuple(paintKitKey));
    }
}

void ItemSchema::ParsePaintKitRarities(const KeyValue *raritiesKey)
{
    for (const KeyValue &key : raritiesKey->Subkeys())
    {
        PaintKitInfo *paintKitInfo = PaintKitInfoByName(key.Name());
        if (!paintKitInfo)
        {
            //assert(false);
            //Platform::Print("No such paint kit '%s'!!!\n", std::string{ key.Name() }.c_str());
            continue;
        }

        assert(paintKitInfo->m_rarity == Rarity::Common);
        paintKitInfo->m_rarity = ItemRarityFromString(key.String());
    }
}

void ItemSchema::ParseKeychainDefinitions(const KeyValue *keychainDefinitionsKey)
{
    m_keychainInfo.reserve(m_keychainInfo.size() + keychainDefinitionsKey->Subkeys().size());

    for (const KeyValue &keychainKey : keychainDefinitionsKey->Subkeys())
    {
        std::string_view name = keychainKey.GetString("name");

        m_keychainInfo.emplace(std::piecewise_construct,
            std::forward_as_tuple(name),
            std::forward_as_tuple(keychainKey));
    }
}

void ItemSchema::ParseMusicDefinitions(const KeyValue *musicDefinitionsKey)
{
    m_musicDefinitionInfo.reserve(musicDefinitionsKey->Subkeys().size());

    for (const KeyValue &musicDefinitionKey : musicDefinitionsKey->Subkeys())
    {
        std::string_view name = musicDefinitionKey.GetString("name");

        m_musicDefinitionInfo.emplace(std::piecewise_construct,
            std::forward_as_tuple(name),
            std::forward_as_tuple(musicDefinitionKey));
    }
}

static void ParseAttributeAndItemName(std::string_view input, std::string_view &attribute, std::string_view &item)
{
    // fallbacks (mikkotodo unfuck)
    attribute = {};
    item = input;

    if (input[0] != '[')
        return;

    size_t attribEnd = input.find(']', 1);
    if (attribEnd == std::string_view::npos)
    {
        assert(false);
        return;
    }

    attribute = input.substr(1, attribEnd - 1);
    item = input.substr(attribEnd + 1);

    assert(attribute.size() && item.size());
}

static LootListItemType LootListItemTypeFromName(std::string_view name, std::string_view attributeName)
{
    if (attributeName.empty())
    {
        return LootListItemNoAttribute;
    }

    const std::pair<std::string_view, LootListItemType> mapNames[] = {
        { "sticker", LootListItemSticker },
        { "spray", LootListItemSpray },
        { "patch", LootListItemPatch },
        { "musickit", LootListItemMusicKit },
        { "keychain", LootListItemKeychain }
    };

    for (const auto &pair : mapNames)
    {
        if (pair.first == name)
        {
            return pair.second;
        }
    }

    return LootListItemPaintable;
}

void ItemSchema::ParseHighlightReels(const KeyValue *highlightReelsKey)
{
    m_highlightReels.reserve(m_highlightReels.size() + highlightReelsKey->Subkeys().size());

    for (const KeyValue &reelKey : highlightReelsKey->Subkeys())
    {
        HighlightReel &reel = m_highlightReels.emplace_back();
        reel.index = FromString<uint32_t>(reelKey.Name());
        reel.id = reelKey.GetString("id");
        reel.map = reelKey.GetString("map");
    }
}

// "match_highlight_reel_keychain" "kc_aus2025" inside crate_aus2025_promo_de_inferno:
// attach a kc_aus2025 charm with one of the aus2025 de_inferno highlight reels
bool ItemSchema::ParseHighlightReelKeychain(LootList &lootList, std::string_view lootListName, std::string_view keychainName)
{
    StickerKitInfo *keychain = KeychainInfoByName(keychainName);
    if (!keychain)
    {
        Platform::Print("No such keychain {} for {}\n", keychainName, lootListName);
        return false;
    }

    // kc_aus2025 -> reels named aus2025_*
    std::string_view event = keychainName;
    if (event.starts_with("kc_"))
    {
        event.remove_prefix(3);
    }

    // crate_aus2025_promo_de_inferno -> de_inferno
    std::string_view map;
    size_t promo = lootListName.find("_promo_");
    if (promo != std::string_view::npos)
    {
        map = lootListName.substr(promo + 7);
    }

    lootList.highlightKeychainId = keychain->m_defIndex;
    lootList.highlightReels.clear();

    for (const HighlightReel &reel : m_highlightReels)
    {
        if (reel.id.size() > event.size()
            && reel.id.starts_with(event)
            && reel.id[event.size()] == '_'
            && (map.empty() || reel.map == map))
        {
            lootList.highlightReels.push_back(reel.index);
        }
    }

    if (lootList.highlightReels.empty())
    {
        // still drop the souvenir, just without the charm
        Platform::Print("No highlight reels for {} ({} {})\n", lootListName, event, map);
        lootList.highlightKeychainId = 0;
    }

    return true;
}

enum class DopplerFamily
{
    None,
    Doppler, // am_doppler_phase1-4 + am_ruby/sapphire/blackpearl_marbleized (and _b, _widow variants)
    GammaDoppler, // am_gamma_doppler_phase1-4 + am_emerald_marbleized
};

static DopplerFamily DopplerFamilyOf(std::string_view paintKit)
{
    if (!paintKit.starts_with("am_"))
    {
        return DopplerFamily::None;
    }

    if (paintKit.find("gamma_doppler") != std::string_view::npos || paintKit.find("emerald_marbleized") != std::string_view::npos)
    {
        return DopplerFamily::GammaDoppler;
    }

    if (paintKit.find("doppler") != std::string_view::npos || paintKit.find("_marbleized") != std::string_view::npos)
    {
        return DopplerFamily::Doppler;
    }

    return DopplerFamily::None;
}

// In the real game Doppler is one finish among the others of a case, its phases split
// that chance and the gems (Ruby, Sapphire, Black Pearl, Emerald) are much rarer than a
// phase. Valve doesn't publish the exact split, a gem is weighted at 1/5 of a phase here
// (4 phases + 3 gems: each phase ~22% of the dopplers, each gem ~4%).
constexpr float DopplerPhaseWeight = 1.0f;
constexpr float DopplerGemWeight = 0.2f;

static void WeightDopplerFinishes(std::vector<LootListItem> &items, const std::vector<std::string_view> &paintKits)
{
    assert(items.size() == paintKits.size());

    // per knife/glove: (def index, family) -> raw weight sum
    std::map<std::pair<ItemDefIndex, DopplerFamily>, float> totals;

    for (size_t i = 0; i < items.size(); i++)
    {
        DopplerFamily family = DopplerFamilyOf(paintKits[i]);
        if (family != DopplerFamily::None)
        {
            bool gem = paintKits[i].find("marbleized") != std::string_view::npos;
            items[i].weight = gem ? DopplerGemWeight : DopplerPhaseWeight;
            totals[{ items[i].itemInfo->m_defIndex, family }] += items[i].weight;
        }
    }

    // scale so all phases and gems of one knife add up to one regular finish
    for (size_t i = 0; i < items.size(); i++)
    {
        DopplerFamily family = DopplerFamilyOf(paintKits[i]);
        if (family != DopplerFamily::None)
        {
            items[i].weight /= totals[{ items[i].itemInfo->m_defIndex, family }];
        }
    }
}

const LootListItem *SelectWeightedLootListItem(std::span<const LootListItem *const> items)
{
    assert(!items.empty());

    float total = 0.0f;
    for (const LootListItem *item : items)
    {
        total += item->weight;
    }

    float value = Random::Float(0.0f, total);
    for (const LootListItem *item : items)
    {
        value -= item->weight;
        if (value < 0.0f)
        {
            return item;
        }
    }

    // float rounding
    return items.back();
}

void ItemSchema::ParseLootLists(const KeyValue *lootListsKey)
{
    m_lootLists.reserve(m_lootLists.size() + lootListsKey->Subkeys().size());

    for (const KeyValue &lootListKey : lootListsKey->Subkeys())
    {
        LootList lootList{};
        bool isMissingEntries = false;
        std::vector<std::string_view> paintKitNames; // parallel to lootList.items

        // not correct... but a pretty good guess
        bool unusual = (lootListKey.Name().find("unusual") != std::string_view::npos);
        lootList.isUnusual = unusual;

        for (const KeyValue &entryKey : lootListKey.Subkeys())
        {
            std::string_view entryName = entryKey.Name();

            // check for options that we ignore
            if (entryName == "contains_patches_representing_organizations"
                || entryName == "contains_stickers_autographed_by_proplayers"
                || entryName == "contains_stickers_representing_organizations"
                || entryName == "limit_description_to_number_rnd"
                || entryName == "public_list_contents"
                || entryName == "attribute_templates")
            {
                continue;
            }

            if (entryName == "will_produce_stattrak")
            {
                lootList.willProduceStatTrak = true;
                continue;
            }

            if (entryName == "all_entries_as_additional_drops")
            {
                lootList.allEntriesAsAdditionalDrops = true;
                continue;
            }

            if (entryName == "match_highlight_reel_keychain")
            {
                if (!ParseHighlightReelKeychain(lootList, lootListKey.Name(), entryKey.String()))
                {
                    isMissingEntries = true;
                }
                continue;
            }

            // check if it's another loot list
            auto listSearch = m_lootLists.find(entryKey.Name());
            if (listSearch != m_lootLists.end())
            {
                lootList.subLists.push_back(&listSearch->second);
                continue;
            }

            // check for an item
            LootListItem item;
            if (ParseLootListItem(item, entryName))
            {
                if (unusual)
                {
                    // override the quality here...
                    item.quality = Quality::Unusual;
                }

                lootList.items.push_back(item);

                std::string_view paintKitName, itemName;
                ParseAttributeAndItemName(entryName, paintKitName, itemName);
                paintKitNames.push_back(paintKitName);
            }
            else
            {
                // this is expected to happen when using the new
                // items_extra.txt with an old game and items_game.txt
                isMissingEntries = true;
            }
        }

        if (unusual)
        {
            WeightDopplerFinishes(lootList.items, paintKitNames);
        }

        if (isMissingEntries)
        {
            Platform::Print("Ignoring loot list {} due to missing entries\n", lootListKey.Name());
            continue;
        }

        [[maybe_unused]] auto [it, inserted] = m_lootLists.try_emplace(
            std::string{ lootListKey.Name() },
            std::move(lootList));
        assert(inserted);
    }
}

static Rarity PaintedItemRarity(Rarity itemRarity, Rarity paintKitRarity)
{
    int rarityValue = (static_cast<int>(itemRarity) - 1) + static_cast<int>(paintKitRarity);
    if (rarityValue < 0)
    {
        return Rarity::Default;
    }

    Rarity rarity = static_cast<Rarity>(rarityValue);
    if (rarity > Rarity::Ancient)
    {
        if (paintKitRarity == Rarity::Immortal)
        {
            return Rarity::Immortal;
        }

        return Rarity::Ancient;
    }

    return rarity;
}

// mikkotodo rewrite this function
bool ItemSchema::ParseLootListItem(LootListItem &item, std::string_view name)
{
    // check for an attribute + item combo
    std::string_view attributeName, itemName;
    ParseAttributeAndItemName(name, attributeName, itemName);

    const ItemInfo *itemInfo = ItemInfoByName(itemName);
    if (!itemInfo)
    {
        Platform::Print("WARNING: no such item {}\n", itemName);
        return false;
    }

    item.itemInfo = itemInfo;
    item.type = LootListItemTypeFromName(itemName, attributeName);

    // until proven otherwise...
    item.rarity = itemInfo->m_rarity;
    item.quality = itemInfo->m_quality;

    if (item.type == LootListItemNoAttribute)
    {
        // no attribute
    }
    else if (item.type == LootListItemSticker || item.type == LootListItemSpray || item.type == LootListItemPatch)
    {
        // the attribute is the sticker name
        StickerKitInfo *stickerKitInfo = StickerKitInfoByName(attributeName);
        if (!stickerKitInfo)
        {
            Platform::Print("WARNING: No such sticker kit {}\n", attributeName);
            return false;
        }

        item.kitDefIndex = stickerKitInfo->m_defIndex;

        // pretty sure this is literally the same as PaintedItemRarity
        item.rarity = PaintedItemRarity(itemInfo->m_rarity, stickerKitInfo->m_rarity);
    }
    else if (item.type == LootListItemKeychain)
    {
        // the attribute is the keychain definition name, e.g. [kc_wpn_ak_base]keychain
        StickerKitInfo *keychainInfo = KeychainInfoByName(attributeName);
        if (!keychainInfo)
        {
            Platform::Print("WARNING: No such keychain {}\n", attributeName);
            return false;
        }

        item.kitDefIndex = keychainInfo->m_defIndex;
        item.rarity = PaintedItemRarity(itemInfo->m_rarity, keychainInfo->m_rarity);
    }
    else if (item.type == LootListItemMusicKit)
    {
        // the attribute is the music definition name
        MusicDefinitionInfo *musicDefinitionInfo = MusicDefinitionInfoByName(attributeName);
        if (!musicDefinitionInfo)
        {
            Platform::Print("WARNING: No such music definition {}\n", attributeName);
            return false;
        }

        item.kitDefIndex = musicDefinitionInfo->m_defIndex;
    }
    else
    {
        // probably a paint kit
        assert(item.type == LootListItemPaintable);
        PaintKitInfo *paintKitInfo = PaintKitInfoByName(attributeName);
        if (!paintKitInfo)
        {
            Platform::Print("WARNING: No such paint kit {}\n", attributeName);
            return false;
        }

        item.kitDefIndex = paintKitInfo->m_defIndex;
        item.minFloat = paintKitInfo->m_minFloat;
        item.maxFloat = paintKitInfo->m_maxFloat;

        // paint kits affect the item rarity
        item.rarity = PaintedItemRarity(itemInfo->m_rarity, paintKitInfo->m_rarity);
    }

    return true;
}

void ItemSchema::ParseRevolvingLootLists(const KeyValue *revolvingLootListsKey)
{
    m_revolvingLootLists.reserve(revolvingLootListsKey->Subkeys().size());

    for (const KeyValue &revolvingLootListKey : revolvingLootListsKey->Subkeys())
    {
        uint32_t index;
        std::string_view lootListName;

        if (FromString<uint32_t>(revolvingLootListKey.Name(), index))
        {
            // ok, name stores index and string stores name
            lootListName = revolvingLootListKey.String();
        }
        else
        {
            // otherwise assume name stores name and string stores index...
            index = FromString<uint32_t>(revolvingLootListKey.String());
            lootListName = revolvingLootListKey.Name();
        }

        assert(index && lootListName.size());

        auto it = m_lootLists.find(lootListName);
        if (it == m_lootLists.end())
        {
            continue;
        }

        m_revolvingLootLists.try_emplace(index, it->second);
    }
}

ItemInfo *ItemSchema::ItemInfoByName(std::string_view name)
{
    for (auto &pair : m_itemInfo)
    {
        const ItemInfo &info = pair.second;
        if (info.m_name == name)
        {
            return &pair.second;
        }
    }

    return nullptr;
}

StickerKitInfo *ItemSchema::StickerKitInfoByName(std::string_view name)
{
    auto it = m_stickerKitInfo.find(name);
    if (it == m_stickerKitInfo.end())
    {
        assert(false);
        return nullptr;
    }

    return &it->second;
}

PaintKitInfo *ItemSchema::PaintKitInfoByName(std::string_view name)
{
    auto it = m_paintKitInfo.find(name);
    if (it == m_paintKitInfo.end())
    {
        //assert(false);
        return nullptr;
    }

    return &it->second;
}

StickerKitInfo *ItemSchema::KeychainInfoByName(std::string_view name)
{
    auto it = m_keychainInfo.find(name);
    if (it == m_keychainInfo.end())
    {
        return nullptr;
    }

    return &it->second;
}

MusicDefinitionInfo *ItemSchema::MusicDefinitionInfoByName(std::string_view name)
{
    auto it = m_musicDefinitionInfo.find(name);
    if (it == m_musicDefinitionInfo.end())
    {
        assert(false);
        return nullptr;
    }

    return &it->second;
}

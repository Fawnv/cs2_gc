#pragma once

#include "gc_const_csgo.h"
#include "item_schema_const.h"
#include "item.h" // FIXME: avoid dragging this in...

class KeyValue;

struct ItemDesc;

enum class AttributeType
{
    Float,
    Uint32,
    String
};

struct ItemAttributeInfo
{
    AttributeDefIndex defIndex;
    ItemAttributeValue value;
};

class ItemInfo
{
public:
    explicit ItemInfo(ItemDefIndex defIndex);

    ItemDefIndex m_defIndex;
    std::string m_name;
    std::string m_itemNameToken; // item_name, e.g. #SFUI_WPNHUD_AK47
    std::string m_imageInventory;
    Rarity m_rarity;
    Quality m_quality;
    uint32_t m_level;
    bool m_isCoupon;
    bool m_isSelfOpeningPurchase;
    uint32_t m_storageIcon; // rio2022 caskets only...
    std::string m_lootListName;
    bool m_willProduceStatTrak;

    std::vector<ItemAttributeInfo> m_attributes;
};

class PaintKitInfo
{
public:
    explicit PaintKitInfo(const KeyValue &key);

    uint32_t m_defIndex;
    Rarity m_rarity;
    float m_minFloat;
    float m_maxFloat;
    std::string m_descriptionTag; // the finish's name, e.g. #PaintKit_cu_awp_cobra_tag (Redline)
};

class StickerKitInfo
{
public:
    explicit StickerKitInfo(const KeyValue &key);

    uint32_t m_defIndex;
    Rarity m_rarity;
};

class MusicDefinitionInfo
{
public:
    MusicDefinitionInfo(const KeyValue &key);

    uint32_t m_defIndex;
};

enum LootListItemType
{
    LootListItemNoAttribute,
    LootListItemPaintable,
    LootListItemSticker,
    LootListItemSpray,
    LootListItemPatch,
    LootListItemMusicKit,
    LootListItemKeychain, // cs2 charms
};

struct LootListItem
{
    // for case opening: returns RarityUnusual for items of unusual quality
    Rarity CaseRarity() const;

    const ItemInfo *itemInfo{};
    LootListItemType type{ LootListItemNoAttribute };

    // paint/sticker/music kit
    uint32_t kitDefIndex{};

    // paint kit wear
    float minFloat{};
    float maxFloat{};

    // might differ from those specified in itemInfo
    // (based on paint kits, stattrak etc.)
    Rarity rarity{};
    Quality quality{};

    // chance relative to the other items of the same rarity: 1 for a regular finish,
    // the doppler phases and gems of a knife share one finish's worth between them
    float weight{ 1.0f };
};

// weighted pick by LootListItem::weight
const LootListItem *SelectWeightedLootListItem(std::span<const LootListItem *const> items);

struct LootList
{
    // we either have items or sublists, never both
    std::vector<LootListItem> items;
    std::vector<const LootList *> subLists;
    bool willProduceStatTrak{};
    bool isUnusual{};
    bool allEntriesAsAdditionalDrops{};

    // cs2 souvenir packages (2025+): the dropped item gets a souvenir charm
    // showing a random highlight reel of this tournament and map
    uint32_t highlightKeychainId{};
    std::vector<uint32_t> highlightReels;
};

// one input of a trade up contract
struct TradeUpInput
{
    ItemDefIndex defIndex;
    uint32_t paintKit;
    float wear;
};

class ItemSchema
{
public:
    ItemSchema();

    // trade up contracts: picks the output for inputs of one rarity.
    // unusualOutput: 5 coverts -> knife/gloves of a case the inputs' collections belong to
    bool SelectTradeUpOutput(std::span<const TradeUpInput> inputs, Rarity inputRarity,
        bool unusualOutput, bool statTrak, ItemDesc &desc) const;

    AttributeType GetAttributeType(AttributeDefIndex defIndex) const;

    // for case opening
    const LootList *GetCrateLootList(ItemDefIndex crateDefIndex) const;
    uint32_t GetCrateSupplyCrateSeries(ItemDefIndex crateDefIndex) const;

    // for case opening FIXME: do we want to keep this here???
    bool ItemDescForLootListItem(
        const LootListItem &lootListItem,
        bool statTrak,
        ItemOrigin origin,
        UnacknowledgedType unacknowledgedType,
        ItemDesc &desc) const;

    // item creation
    bool GetItemDesc(ItemDefIndex defIndex, ItemOrigin origin, UnacknowledgedType unacknowledgedType, ItemDesc &desc) const;
    bool GetPurchaseItemDescs(ItemDefIndex defIndex, std::vector<ItemDesc> &items, std::vector<ItemDesc> &casketedItems) const;

    // Armory: the seasonaloperations entries with "redeemable_goods" "xpshop", in file order.
    // item_name is "lootlist:<client loot list>" or an item definition name
    struct XpShopEntry
    {
        uint32_t points;
        std::string itemName;
        uint32_t redeemId; // CRC-32 of item_name: the redeem_id of ClientRedeemMissionReward
    };
    const XpShopEntry *GetXpShopEntry(uint32_t campaign, uint32_t redeemId) const;
    std::span<const XpShopEntry> GetXpShopEntries(uint32_t campaign) const;

    const LootList *GetLootList(std::string_view name) const;

    // painted guns, knives and gloves the loot lists can drop, one per def + paint kit, sorted
    // (the upgrader's targets)
    struct PaintedItem
    {
        ItemDefIndex defIndex;
        uint32_t paintKit;
        Rarity rarity;
        Quality quality; // unusual for knives and gloves
        float minFloat;
        float maxFloat;
    };
    std::vector<PaintedItem> PaintedItems() const;
    // localization tokens of a definition's name and a finish's name (the market's names are the
    // English texts of these)
    std::string_view ItemNameToken(ItemDefIndex defIndex) const;
    std::string_view PaintKitNameToken(uint32_t paintKit) const;
    const LootListItem *FindPaintedLootListItem(ItemDefIndex defIndex, uint32_t paintKit) const;
    std::optional<ItemDefIndex> ItemDefIndexByName(std::string_view name) const;
    std::string_view ItemDefName(ItemDefIndex defIndex) const;
    Rarity KeychainRarity(uint32_t keychainId) const;

    // the definition's "volatile container" attribute: 1 for collection terminals (Genesis,
    // Dead Hand), 3 for the dynamic sticker capsules, 0 for anything else
    uint32_t VolatileContainerType(ItemDefIndex defIndex) const;
    // a uint32 attribute set on the definition in items_game, 0 if it isn't
    uint32_t StaticAttributeUint32(ItemDefIndex defIndex, AttributeDefIndex attribute) const;

    // souvenirs: a tournament's gold stickers (sticker_kits with tournament_event_id, "_gold" names)
    struct TournamentStickers
    {
        uint32_t organizer{}; // tournament_team_id 0, the event logo
        std::map<uint32_t, uint32_t> teams; // team id -> team sticker
        std::map<uint32_t, std::vector<uint32_t>> players; // team id -> player autographs
    };
    const TournamentStickers *GetTournamentStickers(uint32_t eventId) const;

private:
    void ParseXpShop(std::string_view itemsGameText);
    void ParseXpShopSection(std::span<const std::string_view> tokens);
    std::unordered_map<uint32_t, std::vector<XpShopEntry>> m_xpShop; // campaign (season) -> entries
    std::unordered_map<uint32_t, TournamentStickers> m_tournamentStickers; // event id -> gold stickers

    void ParseItems(const KeyValue *itemsKey, const KeyValue *prefabsKey);
    void ParseItemRecursive(ItemInfo &info, const KeyValue &itemKey, const KeyValue *prefabsKey);
    void ParseItemAttribute(ItemInfo &itemInfo, const KeyValue &attributeKey);
    void ParseAttributes(const KeyValue *attributesKey);
    void ParseStickerKits(const KeyValue *stickerKitsKey);
    void ParsePaintKits(const KeyValue *paintKitsKey);
    void ParsePaintKitRarities(const KeyValue *raritiesKey);
    void ParseMusicDefinitions(const KeyValue *musicDefinitionsKey);
    void ParseKeychainDefinitions(const KeyValue *keychainDefinitionsKey);
    void ParseHighlightReels(const KeyValue *highlightReelsKey);
    bool ParseHighlightReelKeychain(LootList &lootList, std::string_view lootListName, std::string_view keychainName);
    void ParseLootLists(const KeyValue *lootListsKey);
    void ParseRevolvingLootLists(const KeyValue *revolvingLootListsKey);
    void ParseCollections(const KeyValue *itemSetsKey);
    void LinkCollectionToCrate(const LootList &crateLootList);

    const PaintKitInfo *PaintKitInfoById(uint32_t paintKit) const;

    bool ParseLootListItem(LootListItem &item, std::string_view name);

    // internal slop
    ItemInfo *ItemInfoByName(std::string_view name);
    StickerKitInfo *StickerKitInfoByName(std::string_view name);
    PaintKitInfo *PaintKitInfoByName(std::string_view name);
    MusicDefinitionInfo *MusicDefinitionInfoByName(std::string_view name);
    StickerKitInfo *KeychainInfoByName(std::string_view name);

    std::unordered_map<ItemDefIndex, ItemInfo> m_itemInfo;
    std::unordered_map<AttributeDefIndex, AttributeType> m_attributeTypes;

    // these shouldn't be needed at runtime, need to try factoring out later
    StringMap<AttributeDefIndex> m_attributeDefIndexes;
    StringMap<StickerKitInfo> m_stickerKitInfo;
    StringMap<PaintKitInfo> m_paintKitInfo;
    StringMap<MusicDefinitionInfo> m_musicDefinitionInfo;

    // keychain_definitions have the same shape as sticker kits (id, name, item_rarity)
    StringMap<StickerKitInfo> m_keychainInfo;

    struct HighlightReel
    {
        uint32_t index; // value of the "keychain slot 0 highlight" attribute
        std::string id; // e.g. aus2025_chopper2kvsmouzonmirage1
        std::string map; // e.g. de_mirage
    };

    std::vector<HighlightReel> m_highlightReels;

    // item_sets with is_collection, for trade ups
    struct Collection
    {
        std::string name;
        std::vector<LootListItem> items;

        // knives/gloves of the case this collection comes from (empty for map collections)
        std::vector<const LootList *> unusualLists;
    };

    std::vector<Collection> m_collections;
    std::unordered_map<uint64_t, uint32_t> m_collectionByItem; // (def index << 32 | paint kit) -> m_collections index

    const Collection *CollectionForItem(ItemDefIndex defIndex, uint32_t paintKit) const;

    StringMap<LootList> m_lootLists;

    std::unordered_map<uint32_t, const LootList &> m_revolvingLootLists;
};

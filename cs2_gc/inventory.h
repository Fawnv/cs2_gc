#pragma once

#include "gc_const_csgo.h"
#include "inventory_editor.h"
#include "item.h"
#include "item_schema.h"

class CMsgAdjustEquipSlots;
class CMsgApplySticker;
class CMsgSetItemPositions;
class CMsgItemAcknowledged;
class CSOAccountKeychainRemoveToolCharges;
class CSOAccountXpShop;
class CSOAccountItemPersonalStore;
class CEconItemPreviewDataBlock;

class CMsgSOCacheSubscribed;
class CMsgSOMultipleObjects;
class CMsgSOSingleObject;

enum EGCItemCustomizationNotification : int;

class InventoryModify;
struct InventoryChangeMessages;
struct SOCacheMessages;

// CS2 loadout entry, sent to the game as CSOEconEquipSlot (SO type 3)
struct EquipSlotValue
{
    uint64_t itemId; // real item id, or ItemIdDefaultItemMask | def index for default items
    uint32_t defIndex;

    bool operator==(const EquipSlotValue &) const = default;
};

// { class id, slot id } -> what's in it
using EquipSlotMap = std::map<std::pair<uint32_t, uint32_t>, EquipSlotValue>;

struct SOCacheState
{
    bool subscribed{};
    std::unordered_set<uint64_t> items;
    EquipSlotMap equipSlots; // what the game was last told
};

class Inventory
{
public:
    Inventory(uint64_t steamId);
    ~Inventory();

    // called on client welcome
    void SetOldProtocol(bool oldProtocol) { m_oldProtocol = oldProtocol; }

    bool Update(InventoryChangeMessages &changeMessages);

    void BuildCacheSubscription(CMsgSOCacheSubscribed &message, int level, bool server);

    InventoryChangeMessages EquipItem(uint64_t itemId, uint32_t classId, uint32_t slotId);
    InventoryChangeMessages AdjustEquipSlots(const CMsgAdjustEquipSlots &message);
    InventoryChangeMessages RemoveItem(uint64_t itemId);
    InventoryChangeMessages UseItem(uint64_t itemId);
    InventoryChangeMessages UnlockCrate(uint64_t crateId, uint64_t keyId);

    // cs2 7 day rental: every regular item of the case (no knives/gloves), all expiring together
    InventoryChangeMessages RentCrate(uint64_t crateId, uint64_t keyId);

    // trade up contract (k_EMsgGCCraft), recipe is the items_game.txt recipe index the client picked:
    // 0-4 ten items -> next rarity, 5 five coverts -> knife/gloves, +10 for stattrak.
    // createdId is 0 if the contract was rejected
    InventoryChangeMessages TradeUp(uint32_t recipe, std::span<const uint64_t> itemIds, uint64_t &createdId);

    // removes rented items whose expiration date is <= now, returns false if nothing expired
    bool ExpireRentals(uint32_t now, InventoryChangeMessages &changeMessages);
    InventoryChangeMessages ApplySticker(const CMsgApplySticker &message);
    InventoryChangeMessages ApplyKeychain(const CMsgApplySticker &message, Item &keychain);
    InventoryChangeMessages ScrapeSticker(const CMsgApplySticker &message);
    InventoryChangeMessages IncrementKillCountAttribute(uint64_t itemId, uint32_t amount);
    InventoryChangeMessages NameItem(uint64_t nameTagId, uint64_t itemId, std::string_view name);
    // inspect links (+csgo_econ_action_preview S..A..D..): false if the item isn't ours
    bool PreviewDataBlock(uint64_t itemId, CEconItemPreviewDataBlock &block);

    // Sticker Slab: ApplySticker with a sticker and a slab (either way round), or InventoryAPI.RemoveKeychain
    // on a sticker. The sticker becomes a charm of the slab's keychain (37) showing it. false if the
    // request isn't one
    bool TryEncapsulateSticker(const CMsgApplySticker &message, InventoryChangeMessages &messages);

    // StatTrak Swap Tool (k_EMsgGCStatTrakSwap): the two StatTrak counters trade places
    InventoryChangeMessages StatTrakSwap(uint64_t toolId, uint64_t itemId1, uint64_t itemId2);
    InventoryChangeMessages NameBaseItem(uint64_t nameTagId, uint32_t defIndex, std::string_view name);
    InventoryChangeMessages RemoveItemName(uint64_t itemId);
    InventoryChangeMessages CasketItemAdd(uint64_t casketId, uint64_t itemId);
    InventoryChangeMessages CasketItemExtract(uint64_t casketId, uint64_t itemId);

    InventoryChangeMessages SetItemPositions(const CMsgSetItemPositions &message, std::vector<CMsgItemAcknowledged> &acknowledgements);
    InventoryChangeMessages PurchaseItems(const std::vector<uint32_t> &defIndexes, std::vector<uint64_t> &itemIds);

    // charm removal (InventoryAPI.RemoveKeychain): uses a charge, the charm goes back to the inventory
    InventoryChangeMessages RemoveKeychain(uint64_t itemId, uint32_t slot);
    bool ItemHasKeychain(uint64_t itemId);
    bool ItemHasSticker(uint64_t itemId, uint32_t slot);
    uint32_t KeychainRemoveCharges() const { return m_account.keychainRemoveCharges; }

    // Armory (k_EMsgGCCStrike15_v2_ClientRedeemMissionReward): buy the shop entry whose item_name
    // has CRC-32 `redeemId` for its stars. createdId is 0 if nothing was bought
    InventoryChangeMessages RedeemXpShopReward(uint32_t campaign, uint32_t redeemId, uint32_t expectedCost, uint64_t &createdId);
    // the client saw its finished passes (Client2GcAckXPShopTracks): drop them so new ones fit
    InventoryChangeMessages AckXpShopTracks();
    uint32_t XpShopBalance() const { return m_account.xpShopBalance; }
    size_t XpShopTrackCount() const { return m_account.xpShopTracks.size(); }

    // Armory pass: 40 stars of 300 xp, at most 5 passes active (client.dll StoreAPI.GetXpShopMaxTrackLevel,
    // GetXpShopStarXp, GetXpShopMaxTracks). Offline there's no xp to earn them with, a pass is full right away
    static constexpr uint32_t XpShopStarsPerPass = 40;
    static constexpr uint32_t XpShopStarXp = 300;
    static constexpr size_t XpShopMaxTracks = 5;
    static constexpr uint32_t KeychainRemoveChargesPerPack = 3; // "items count" of Remove Keychain Tool Pack

    // terminals (Genesis, Dead Hand): unseal one into an active terminal, a rental volatile container
    // that holds one offer at a time. The client asks for the next offer with OpenCrate on the
    // container (points_remaining = the offer number it has, volatile_limit = max price)
    bool IsSealedTerminal(uint64_t itemId);
    bool IsActiveTerminal(uint64_t itemId);
    InventoryChangeMessages UnsealTerminal(uint64_t terminalId, uint64_t &containerId);
    InventoryChangeMessages NextTerminalOffer(uint64_t containerId, uint32_t priceLimit);
    // k_EMsgGCVolatileItemLoadContents: the client reopened an active terminal
    InventoryChangeMessages TerminalContents(uint64_t containerId);
    // store purchase "<terminal def>(<container id>)": the current offer, 0 if there is none.
    // Without the container id the line item's cost has to be the offer's price (or it's a
    // sealed terminal bought from the store)
    uint64_t TerminalOfferFor(uint32_t terminalDef, uint64_t containerId, uint64_t cost, uint64_t *foundContainer = nullptr);
    InventoryChangeMessages BuyTerminalOffer(uint64_t containerId, uint64_t &offerId);
    // collection xp (k_EMsgGCCStrike15_v2_VolatileItemClaimReward): reward = rarity, 99 for the gold item
    InventoryChangeMessages ClaimTerminalReward(uint32_t terminalDef, uint32_t reward);
    static constexpr uint32_t TerminalOffers = 5;
    static constexpr uint32_t TerminalSeconds = 24 * 60 * 60;

    // levels: 5000 xp each, up to 40 (MyPersonaAPI.GetXpPerLevel). The first level up of a week
    // (Wednesday 00:00 UTC) earns the weekly care package: 4 items to pick 2 from
    InventoryChangeMessages AddXp(uint32_t xp, std::string_view reason);
    // the upgrader (an in-game window CSForge adds): skins in, one dearer skin out, with a chance of
    // what the deposits are worth over what the target is (50 for 100: 50%). A target worth no more
    // than the deposits is refused. The deposits are gone either way
    struct UpgradeResult
    {
        bool done{};
        bool won{};
        double chance{};
        uint64_t itemId{}; // the new item when won
        std::string error;
    };
    struct UpgradeOffer
    {
        ItemDefIndex defIndex{};
        uint32_t paintKit{};
        int wear{}; // 0 Factory New .. 4 Battle-Scarred: the new item's float is in that range
        bool statTrak{};
        uint64_t price{}; // in the deposits' unit, 0 = not on sale
    };
    using UpgradePriceFn = uint64_t (*)(const Item &item); // a deposit's worth, 0 = none
    // the color tier in the window: the rarity, 7 for knives and gloves
    static uint32_t UpgradeTier(Rarity rarity, Quality quality);
    InventoryChangeMessages Upgrade(std::span<const uint64_t> depositIds, const UpgradeOffer &target, UpgradePriceFn depositPrice,
        UpgradeResult &result, std::optional<float> roll = std::nullopt);
    static constexpr size_t UpgradeMaxDeposits = 10;
    // sends an item to the client again as it is: the upgrader window takes an inventory update
    // that leaves its deposit in place as "refused"
    InventoryChangeMessages TouchItem(uint64_t itemId);

    // match xp gets CS2's weekly bonus on top, by the xp earned this week: x4 up to 4500,
    // x2 up to 7500, x1 up to 11167, x0.175 past that
    InventoryChangeMessages AddMatchXp(uint32_t baseXp, std::string_view reason);
    uint32_t WeeklyXpEarned() const;
    uint32_t PlayerLevel() const { return m_account.level; }
    uint32_t PlayerXp() const { return m_account.xp; } // within the level
    bool CarePackageEarnedThisWeek() const;
    // StoreAPI.StoreRedeemFreeRewards -> CMsgGCCstrike15_v2_ClientRedeemFreeReward
    InventoryChangeMessages RedeemFreeRewards(std::span<const uint64_t> itemIds);
    const std::vector<uint64_t> &CarePackageItems() const { return m_account.storeItems; }
    // service medals (k_EMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin, both ways): at level 40 the
    // medal of the current year, "prestige coin <year>" and then "... level 2" up to 6 (the newest
    // year in the schema if this one isn't there yet). One of that year owned already turns into the
    // next level in place (upgradeId), so a displayed one stays displayed; at the top level it stays
    // as it is. Getting it starts the levels over at 1
    struct PrestigeCoin
    {
        ItemDefIndex defIndex{};
        uint64_t upgradeId{}; // the medal of that year owned now, 0 if none
    };
    std::optional<PrestigeCoin> NextPrestigeCoin() const;
    InventoryChangeMessages RedeemPrestigeCoin(ItemDefIndex requested, uint64_t &itemId, std::string &error);

    static constexpr uint32_t XpPerLevel = 5000;
    static constexpr uint32_t MaxLevel = 40;
    static constexpr size_t CarePackageSize = 4;
    static constexpr uint32_t CarePackagePicks = 2;
    static uint32_t XpWeekStart(uint32_t now);

    // FIXME: unfuck this!!!
    void ResetGameServerCache()
    {
        m_gameServerCache.subscribed = false;
        m_gameServerCache.items.clear();
    }

private:
    uint32_t AccountId() const;

    // increment version, build messages for the game, send changes to editor and write to file
    InventoryChangeMessages FinishChanges(
        InventoryModify &modify,
        std::optional<EGCItemCustomizationNotification> customizationType = std::nullopt,
        std::initializer_list<uint64_t> customizationItemIds = {});

    void BuildSOCacheMessages(
        const InventoryModify &modify,
        SOCacheState &state,
        SOCacheMessages &messages,
        bool server);

    // allocates an unique low item id
    // pass zero as lowItemId if you don't want a specific one
    uint32_t GetLowItemId(uint32_t lowItemId);

    // used to create items from inventory.txt
    void CreateItem(uint32_t lowId, const KeyValue &kv);

    // used to create items at runtime
    Item &CreateItem(InventoryModify &modify, ItemDesc &&desc);

    // find an existing item by id, returns nullptr if not found
    Item *FindItem(uint64_t itemId);

    void ReadFromFile();
    void WriteToFile() const;

    bool EquipItem(InventoryModify &modify, uint64_t itemId, uint32_t classId, uint32_t slotId);
    bool UnequipItem(InventoryModify &modify, uint64_t itemId);
    void UnequipItem(InventoryModify &modify, uint32_t classId, uint32_t slotId);

    bool DestroyItemById(InventoryModify &modify, uint64_t itemId);
    void DestroyItem(InventoryModify &modify, Item *item);

    // helpers for serializing items to CMsgSOMultipleObjects and CMsgSOSingleObject
    void AddToMultipleObjects(CMsgSOMultipleObjects &message, SOTypeId type, const google::protobuf::MessageLite &object);
    void ToSingleObject(CMsgSOSingleObject &message, SOTypeId type, const google::protobuf::MessageLite &object);

    // CS2 equip slots: collect current state and send the difference to the game
    void CollectEquipSlots(EquipSlotMap &out) const;
    void BuildEquipSlotMessages(SOCacheState &state, SOCacheMessages &messages);

    void SendFullInventoryToEditor();
    void SendChangesToEditor(const InventoryModify &modify);

    void RemoveCachedIcon(uint32_t lowId);

    const uint64_t m_steamId;
    ItemSchema m_itemSchema;
    bool m_oldProtocol{ false };
    uint32_t m_lastLowItemId{};
    uint64_t m_version{ 1 };
    std::unordered_map<uint64_t, Item> m_items;
    std::vector<DefaultEquip> m_defaultEquips;

    SOCacheState m_clientCache;
    SOCacheState m_gameServerCache;

    InventoryEditor m_editor;

    // stupid hack since we don't want to show casketed items in the editor
    std::unordered_set<uint32_t> m_editorVisibleItems;

    // rental expiry is checked every few seconds, not every frame
    uint32_t m_nextRentalCheck{};

    // account level state, "account" in inventory.txt
    struct AccountState
    {
        uint32_t keychainRemoveCharges{};
        uint32_t xpShopBalance{}; // Armory stars to spend
        std::vector<uint32_t> xpShopTracks; // xp of every active pass
        uint32_t xpShopGeneration{};

        uint32_t level{};
        uint32_t xp{}; // within the level
        uint32_t xpWeek{}; // XpWeekStart of the week the last care package came in
        uint32_t xpEarned{}; // match xp earned (with the bonus) in the week xpEarnedWeek
        uint32_t xpEarnedWeek{};

        // weekly care package (CSOAccountItemPersonalStore)
        uint32_t storeGeneration{};
        uint32_t storeBalance{}; // picks left
        std::vector<uint64_t> storeItems;

        // terminals, by terminal def: offers seen (faux item ids) and collection xp claimed
        struct Volatile
        {
            std::vector<std::pair<uint64_t, uint32_t>> seen; // faux id, time
            std::vector<std::pair<uint32_t, uint32_t>> claimed; // reward, time
        };
        std::map<uint32_t, Volatile> volatileHistory;
    };
    AccountState m_account;

    void ReadAccount(const KeyValue *accountKey);
    void WriteAccount(KeyValueWrite &write) const;
    void BuildAccountObjects(SOCacheMessages &messages, uint32_t changes);
    void AccountPersonalStore(CSOAccountItemPersonalStore &out) const;
    Item *TerminalOffer(uint64_t containerId);
    void FillCarePackage(InventoryModify &modify);
    uint32_t NextInventoryPosition() const; // after the highest acknowledged one
    void RefreshCarePackage(); // on load, while picks are left
    void FixPaintedQualities(); // on load
    void FixStatTrakStorageUnits(); // on load
    void GrantXp(InventoryModify &modify, uint32_t xp, std::string_view reason);

    void AccountKeychainCharges(CSOAccountKeychainRemoveToolCharges &out) const;
    void AccountXpShop(CSOAccountXpShop &out) const;
    InventoryChangeMessages UseXpShopTicket(Item &ticket);
    InventoryChangeMessages UseKeychainRemoveToolPack(Item &pack);
    InventoryChangeMessages UseTournamentPass(Item &pass, std::string_view name);
};

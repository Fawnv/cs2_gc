// these could be parsed from the item schema but reduce code complexity by hardcoding them
// FIXME: update code so we can use the automatically generated one!!!
#pragma once

enum class Rarity : uint32_t
{
    Default = 0,
    Common = 1,
    Uncommon = 2,
    Rare = 3,
    Mythical = 4,
    Legendary = 5,
    Ancient = 6,
    Immortal = 7,

    Unusual = 99
};

enum class Quality : uint32_t
{
    Normal = 0,
    Genuine = 1,
    Vintage = 2,
    Unusual = 3,
    Unique = 4,
    Community = 5,
    Developer = 6,
    Selfmade = 7,
    Customized = 8,
    Strange = 9,
    Completed = 10,
    Haunted = 11,
    Tournament = 12,
    Highlight = 13,
    Volatile = 14 // an unsealed terminal
};

enum class GraffitiTint : uint32_t
{
    Min = 1,
    Max = 19
};

enum class ItemDefIndex : uint32_t
{
    KeychainRemoveTool = 65, // base item, the "tool" a charm removal uses
    Casket = 1201,
    Sticker = 1209,
    MusicKit = 1314,
    StatTrakSwapTool = 1324,
    StickerSlab = 4000, // "sticker_display_case": wraps a sticker into a charm
    Spray = 1348,
    SprayPaint = 1349,
    XpShopTicket = 1354, // Armory Pass
    Keychain = 1355, // a charm in the inventory
    KeychainRemoveToolPack = 4950, // 3 charm removals
    Patch = 4609
};

enum class AttributeDefIndex : uint32_t
{
    TexturePrefab = 6,
    TextureSeed = 7,
    TextureWear = 8,
    SupplyCrateSeries = 68,
    AlternateIcon = 70,
    KillEater = 80,
    KillEaterScoreType = 81,

    CustomName = 111,

    // ugh
    StickerId0 = 113,
    StickerWear0 = 114,
    StickerScale0 = 115,
    StickerRotation0 = 116,
    StickerId1 = 117,
    StickerWear1 = 118,
    StickerScale1 = 119,
    StickerRotation1 = 120,
    StickerId2 = 121,
    StickerWear2 = 122,
    StickerScale2 = 123,
    StickerRotation2 = 124,
    StickerId3 = 125,
    StickerWear3 = 126,
    StickerScale3 = 127,
    StickerRotation3 = 128,
    StickerId4 = 129,
    StickerWear4 = 130,
    StickerScale4 = 131,
    StickerRotation4 = 132,
    StickerId5 = 133,
    StickerWear5 = 134,
    StickerScale5 = 135,
    StickerRotation5 = 136,

    MusicId = 166,
    QuestId = 168,

    SpraysRemaining = 232,
    SprayTintId = 233,

    TournamentEventId = 137, // souvenirs and tournament items
    TournamentEventStageId = 138,
    TournamentEventTeam0Id = 139,
    TournamentEventTeam1Id = 140,
    QuestPointsRemaining = 169, // terminals: offer number, negative for the final one
    FreeRewardStatus = 277, // weekly care package item not picked yet

    CasketItemsCount = 270,
    CasketModificationDate = 271,
    CasketIdLow = 272,
    CasketIdHigh = 273,

    // cs2 case rental: rented items carry both, the date is a unix timestamp
    ExpirationDate = 183,
    IsRental = 297,
    CanOpenForRental = 298, // static attribute on rentable cases

    // cs2 terminals
    VolatileContainer = 315, // static on terminal defs
    PurchasePrice = 316, // price of a terminal offer, cents
    PurchaserAccountId = 317, // original owner certificate

    // cs2: sticker placement, 2 per slot (x, y)
    StickerOffsetX0 = 278,
    StickerOffsetY0 = 279,

    // cs2: charms
    KeychainId0 = 299,
    KeychainOffsetX0 = 300,
    KeychainOffsetY0 = 301,
    KeychainOffsetZ0 = 302,
    KeychainSeed0 = 306,
    KeychainHighlight0 = 314,
    KeychainSticker0 = 321,
    DisplayCaseKeychainId = 322, // static on the Sticker Slab (4000): the charm it turns into
};

// not really item schema!!! it does have these defs, but out of date
// better location would be gc_const_csgo.h
enum LoadoutSlot : uint32_t
{
    LoadoutSlotGraffiti = 56
};

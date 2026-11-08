#pragma once

#include "gc_const_csgo.h"
#include "item_schema_const.h" // rarity constants

struct RarityWeight
{
    Rarity rarity;
    float weight;
};

// for Platform::Print calls
enum LogOutput
{
    LogOutputNone, // don't output anything
    LogOutputConsole, // game console
    LogOutputFile // game console and gc_log.txt
};

class GCConfig
{
public:
    void Load();

    float GetRarityWeight(Rarity rarity) const;

    LogOutput m_logOutput{ LogOutputFile }; // console + gc_log.txt in the data dir

    bool m_showCsgoGCServersOnly{ true };

    // empty: read scripts/items/items_game.txt from game\csgo\pak01_dir.vpk
    std::string m_itemSchemaPath;

    // local editor at http://127.0.0.1:13001/ (served from cs2_gc\editor)
    bool m_inventoryEditor{ true };
    // optional extra web page Origin allowed to connect (e.g. https://mikkokko.github.io)
    std::string m_inventoryEditorOrigin;
    // the editor's http + websocket port (another one to try a build while a game holds 13001)
    int m_inventoryEditorPort{ 13001 };

    // CSForge (mod loader): build cs2_gc\mods into a vpk and mount it in front of the game's, see mod_loader.h
    bool m_mods{ false };
    // CSForge's built-in fixes for the game's Panorama scripts (README, CSForge)
    bool m_panoramaFixes{ true };
    // localization files the mods' localization.txt gets merged into (resource/csgo_<lang>.txt)
    std::vector<std::string> m_modLanguages{ "english", "russian" };

    RankId m_competitiveRank{ RankNone };
    int m_competitiveWins{ 0 };
    RankId m_wingmanRank{ RankNone };
    int m_wingmanWins{ 0 };
    // CS Rating shown in premier, e.g. 15000
    uint32_t m_premierRating{ 0 };
    int m_premierWins{ 0 };

    bool m_destroyUsedItems{ true };
    // charm removals a new inventory starts with (later ones come from Remove Keychain Tool Packs)
    uint32_t m_keychainRemoveCharges{ 10 };
    // xp for matches with bots and on community servers, seen through the game's Steam rich presence
    bool m_matchXp{ true };
    // the upgrader's prices: market.csgo.com's list in this currency (RUB, USD, EUR), cached in the
    // data dir and downloaded again when older than the hours; empty or "0": chances by rarity (1:10)
    std::string m_upgraderPrices{ "RUB" };
    int m_upgraderPricesHours{ 12 };

    bool m_vacBanned{ false };
    int m_commendedFriendly{ 0 };
    int m_commendedTeaching{ 0 };
    int m_commendedLeader{ 0 };
    int m_level{ 0 };
    int m_xp{ 0 };

    std::string m_country{ "FI" };
    int m_currency{ 2 };

private:
    // default to valve weights
    std::vector<RarityWeight> m_rarityWeights{
        { Rarity::Common, 10000000 },
        { Rarity::Uncommon, 2000000 },
        { Rarity::Rare, 400000 },
        { Rarity::Mythical, 80000 },
        { Rarity::Legendary, 16000 },
        { Rarity::Ancient, 3200 },
        { Rarity::Unusual, 1280 },
    };
};

void LoadConfig();
const GCConfig &GetConfig();

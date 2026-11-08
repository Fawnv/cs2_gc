#include "stdafx.h"
#include "config.h"
#include "keyvalue.h"
#include <ranges>


static GCConfig s_config;

void LoadConfig()
{
    s_config.Load();
}

const GCConfig &GetConfig()
{
    return s_config;
}

void GCConfig::Load()
{
    std::string configPath = Platform::DataPath("config.txt");
    KeyValueRead configRead{ configPath.c_str() };
    if (!configRead.Good())
    {
        return;
    }

    const KeyValue &config = configRead.Root();

    m_logOutput = config.GetNumber("log_output", m_logOutput);

    m_showCsgoGCServersOnly = config.GetNumber("show_csgo_gc_servers_only", m_showCsgoGCServersOnly);

    m_itemSchemaPath = config.GetString("item_schema_path", m_itemSchemaPath);

    m_inventoryEditor = config.GetNumber("inventory_editor", m_inventoryEditor);
    m_inventoryEditorOrigin = config.GetString("inventory_editor_origin", m_inventoryEditorOrigin);
    m_inventoryEditorPort = config.GetNumber("inventory_editor_port", m_inventoryEditorPort);

    // "custom_content" was the name of the experiment before CSForge (the mod loader)
    m_mods = config.GetNumber("mods", config.GetNumber("custom_content", m_mods ? 1 : 0)) != 0;
    m_panoramaFixes = config.GetNumber("panorama_fixes", m_panoramaFixes ? 1 : 0) != 0;

    std::string_view languages = config.GetString("mod_languages");
    if (!languages.empty())
    {
        m_modLanguages.clear();
        for (auto word : std::views::split(languages, ' '))
        {
            std::string_view language{ word.begin(), word.end() };
            if (!language.empty())
            {
                m_modLanguages.emplace_back(language);
            }
        }
    }

    const KeyValue *ranks = config.GetSubkey("ranks");
    if (ranks)
    {
        m_competitiveRank = ranks->GetNumber("competitive_rank", m_competitiveRank);
        m_competitiveWins = ranks->GetNumber("competitive_wins", m_competitiveWins);

        m_wingmanRank = ranks->GetNumber("wingman_rank", m_wingmanRank);
        m_wingmanWins = ranks->GetNumber("wingman_wins", m_wingmanWins);

        m_premierRating = ranks->GetNumber("premier_rating", m_premierRating);
        m_premierWins = ranks->GetNumber("premier_wins", m_premierWins);
    }

    m_destroyUsedItems = config.GetNumber("destroy_used_items", m_destroyUsedItems);
    m_keychainRemoveCharges = config.GetNumber("keychain_remove_charges", m_keychainRemoveCharges);
    m_matchXp = config.GetNumber("match_xp", m_matchXp ? 1 : 0) != 0;
    // a currency code goes into a file name and the url: letters only
    std::string currency;
    for (char c : config.GetString("upgrader_prices", m_upgraderPrices))
    {
        if (std::isalpha(static_cast<unsigned char>(c)))
        {
            currency += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    m_upgraderPrices = currency == "OFF" ? std::string{} : currency;
    m_upgraderPricesHours = config.GetNumber("upgrader_prices_hours", m_upgraderPricesHours);

    const KeyValue *rarityWeights = config.GetSubkey("rarity_weights");
    if (rarityWeights)
    {
        m_rarityWeights.clear();
        m_rarityWeights.reserve(rarityWeights->Subkeys().size());

        for (const KeyValue &subkey : rarityWeights->Subkeys())
        {
            RarityWeight weight;
            weight.rarity = ToEnum<Rarity>(FromString<uint32_t>(subkey.Name()));
            weight.weight = FromString<float>(subkey.String());
            m_rarityWeights.push_back(weight);
        }
    }

    m_vacBanned = config.GetNumber("vac_banned", m_vacBanned);
    m_commendedFriendly = config.GetNumber("cmd_friendly", m_commendedFriendly);
    m_commendedTeaching = config.GetNumber("cmd_teaching", m_commendedTeaching);
    m_commendedLeader = config.GetNumber("cmd_leader", m_commendedLeader);
    m_level = config.GetNumber("player_level", m_level);
    m_xp = config.GetNumber("player_cur_xp", m_xp);

    m_country = config.GetString("country", m_country);
    m_currency = config.GetNumber("currency", m_currency);
}

float GCConfig::GetRarityWeight(Rarity rarity) const
{
    for (const RarityWeight &weight : m_rarityWeights)
    {
        if (weight.rarity == rarity)
        {
            return weight.weight;
        }
    }

    return 0;
}

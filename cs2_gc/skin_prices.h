#pragma once

// market prices of skins for the upgrader, which compares what goes in with what comes out by
// price. The list is market.csgo.com's (no key needed), cached in the data dir and fetched again
// when it gets old
namespace SkinPrices
{

constexpr int WearCount = 5;

// 0 Factory New, 1 Minimal Wear, 2 Field-Tested, 3 Well-Worn, 4 Battle-Scarred
int WearOf(float wear);
float WearMin(int wear);
float WearMax(int wear);
std::string_view WearName(int wear); // the market's English name

// a finish with this float range comes in this wear
bool WearPossible(int wear, float minFloat, float maxFloat);

// the market's name for a skin: "★ StatTrak™ Karambit | Fade (Factory New)"
std::string MarketHashName(std::string_view weapon, std::string_view finish, int wear, bool star, bool statTrak, bool souvenir);

class PriceList
{
public:
    // the body of https://market.csgo.com/api/v2/prices/<CUR>.json:
    // {"success":true,"time":..,"currency":"RUB","items":[{"market_hash_name":"..","volume":21,"price":"908.68"},..]}
    bool Parse(std::string_view json, std::string &error);

    // in hundredths of the currency (kopecks, cents), 0 if it's not on sale
    uint64_t Find(const std::string &marketHashName) const;

    size_t Size() const { return m_prices.size(); }
    const std::string &Currency() const { return m_currency; }
    int64_t Time() const { return m_time; } // when the market made the list (unix time)

private:
    std::string m_currency;
    int64_t m_time{};
    std::unordered_map<std::string, uint64_t> m_prices;
};

// <data dir>\prices_<CUR>.json, downloaded again when it's older than maxAgeHours or missing
// (a failed download keeps the old one). False when there's no list at all
bool LoadPriceList(std::string_view currency, int maxAgeHours, PriceList &out);

} // namespace SkinPrices

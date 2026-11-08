#include "stdafx.h"
#include "skin_prices.h"
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

namespace SkinPrices
{

// where each wear starts, the last one ends at 1
static constexpr float WearBounds[WearCount + 1] = { 0.0f, 0.07f, 0.15f, 0.38f, 0.45f, 1.0f };
static constexpr std::string_view WearNames[WearCount] = { "Factory New", "Minimal Wear", "Field-Tested", "Well-Worn", "Battle-Scarred" };

int WearOf(float wear)
{
    for (int i = 1; i < WearCount; i++)
    {
        if (wear < WearBounds[i])
        {
            return i - 1;
        }
    }
    return WearCount - 1;
}

float WearMin(int wear)
{
    return WearBounds[std::clamp(wear, 0, WearCount - 1)];
}

float WearMax(int wear)
{
    return WearBounds[std::clamp(wear, 0, WearCount - 1) + 1];
}

std::string_view WearName(int wear)
{
    return WearNames[std::clamp(wear, 0, WearCount - 1)];
}

bool WearPossible(int wear, float minFloat, float maxFloat)
{
    return WearMin(wear) < maxFloat && WearMax(wear) > minFloat;
}

std::string MarketHashName(std::string_view weapon, std::string_view finish, int wear, bool star, bool statTrak, bool souvenir)
{
    // ★ and ™ in UTF-8, the way the market writes them
    return std::format("{}{}{}{} | {} ({})", star ? "\xE2\x98\x85 " : "", statTrak ? "StatTrak\xE2\x84\xA2 " : "",
        souvenir ? "Souvenir " : "", weapon, finish, WearName(wear));
}

static void AppendUtf8(std::string &out, uint32_t code)
{
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

static bool ReadHex4(std::string_view json, size_t &pos, uint32_t &value)
{
    if (pos + 4 > json.size())
    {
        return false;
    }
    auto result = std::from_chars(json.data() + pos, json.data() + pos + 4, value, 16);
    if (result.ptr != json.data() + pos + 4)
    {
        return false;
    }
    pos += 4;
    return true;
}

// a JSON string starting at pos (the opening quote), pos ends up after the closing one
static bool ReadString(std::string_view json, size_t &pos, std::string &out)
{
    out.clear();
    if (pos >= json.size() || json[pos] != '"')
    {
        return false;
    }

    for (pos++; pos < json.size(); pos++)
    {
        char c = json[pos];
        if (c == '"')
        {
            pos++;
            return true;
        }
        if (c != '\\')
        {
            out += c;
            continue;
        }

        if (++pos >= json.size())
        {
            return false;
        }
        switch (json[pos])
        {
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
        {
            pos++;
            uint32_t code = 0;
            if (!ReadHex4(json, pos, code))
            {
                return false;
            }
            // a surrogate pair: 😀
            if (code >= 0xD800 && code < 0xDC00 && json.substr(pos, 2) == "\\u")
            {
                size_t low = pos + 2;
                uint32_t second = 0;
                if (ReadHex4(json, low, second) && second >= 0xDC00 && second < 0xE000)
                {
                    code = 0x10000 + ((code - 0xD800) << 10) + (second - 0xDC00);
                    pos = low;
                }
            }
            AppendUtf8(out, code);
            pos--; // the loop steps over the last digit
            break;
        }
        default: out += json[pos]; break; // \" \\ \/
        }
    }
    return false;
}

// a value after "key": in the object that starts at from: a string or a number, as text
static bool FindValue(std::string_view json, size_t from, size_t to, std::string_view key, std::string &value)
{
    std::string pattern = std::format("\"{}\":", key);
    size_t pos = json.find(pattern, from);
    if (pos == std::string_view::npos || pos >= to)
    {
        return false;
    }
    pos += pattern.size();
    while (pos < to && json[pos] == ' ')
    {
        pos++;
    }
    if (pos < to && json[pos] == '"')
    {
        return ReadString(json, pos, value);
    }
    size_t end = json.find_first_of(",}]", pos);
    if (end == std::string_view::npos || end > to)
    {
        return false;
    }
    value.assign(json.substr(pos, end - pos));
    return true;
}

bool PriceList::Parse(std::string_view json, std::string &error)
{
    m_prices.clear();
    m_currency.clear();
    m_time = 0;

    size_t items = json.find("\"items\":");
    std::string value;
    if (items == std::string_view::npos || !FindValue(json, 0, json.size(), "success", value) || value != "true")
    {
        error = "not a market.csgo.com price list";
        return false;
    }
    if (FindValue(json, 0, items, "currency", value))
    {
        m_currency = value;
    }
    if (FindValue(json, 0, items, "time", value))
    {
        std::from_chars(value.data(), value.data() + value.size(), m_time);
    }

    std::string name;
    size_t pos = items;
    while ((pos = json.find("\"market_hash_name\":", pos)) != std::string_view::npos)
    {
        size_t end = json.find('}', pos);
        if (end == std::string_view::npos)
        {
            break;
        }
        size_t quote = json.find('"', pos + 19);
        if (quote == std::string_view::npos || quote > end || !ReadString(json, quote, name))
        {
            pos = end;
            continue;
        }
        // the name can have a } in it, the object ends after it
        end = json.find('}', quote);
        double price = 0.0;
        if (end != std::string_view::npos && FindValue(json, quote, end, "price", value))
        {
            std::from_chars(value.data(), value.data() + value.size(), price);
        }
        uint64_t hundredths = static_cast<uint64_t>(std::llround(price * 100.0));
        if (hundredths)
        {
            auto [it, inserted] = m_prices.try_emplace(name, hundredths);
            if (!inserted)
            {
                it->second = (std::min)(it->second, hundredths);
            }
        }
        pos = end == std::string_view::npos ? json.size() : end;
    }

    if (m_prices.empty())
    {
        error = "no prices in the list";
        return false;
    }
    return true;
}

uint64_t PriceList::Find(const std::string &marketHashName) const
{
    auto it = m_prices.find(marketHashName);
    return it != m_prices.end() ? it->second : 0;
}

static bool HttpGet(const wchar_t *host, const std::wstring &path, std::string &body, std::string &error)
{
    body.clear();
    HINTERNET session = WinHttpOpen(L"cs2_gc", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        error = std::format("WinHttpOpen failed ({})", GetLastError());
        return false;
    }

    // resolve, connect, send, receive: the game waits for this at startup, so not long
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 20000);
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

    bool ok = false;
    HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &size, WINHTTP_NO_HEADER_INDEX);
        DWORD available = 0;
        ok = true;
        while (WinHttpQueryDataAvailable(request, &available) && available)
        {
            size_t at = body.size();
            body.resize(at + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + at, available, &read))
            {
                ok = false;
                break;
            }
            body.resize(at + read);
        }
        if (status != 200)
        {
            error = std::format("HTTP {}", status);
            ok = false;
        }
        else if (!ok)
        {
            error = std::format("reading failed ({})", GetLastError());
        }
    }
    else
    {
        error = std::format("request failed ({})", GetLastError());
    }

    if (request)
    {
        WinHttpCloseHandle(request);
    }
    if (connection)
    {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return ok;
}

static bool ReadWholeFile(const std::string &path, std::string &text)
{
    FILE *f = Platform::OpenFile(path, "rb");
    if (!f)
    {
        return false;
    }
    char buffer[65536];
    size_t read;
    text.clear();
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        text.append(buffer, read);
    }
    fclose(f);
    return true;
}

bool LoadPriceList(std::string_view currency, int maxAgeHours, PriceList &out)
{
    std::string path = Platform::DataPath(std::format("prices_{}.json", currency));
    std::string text, error;
    bool cached = ReadWholeFile(path, text) && out.Parse(text, error);
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t age = cached ? now - out.Time() : 0;
    if (cached && age >= 0 && age < static_cast<int64_t>(maxAgeHours) * 3600)
    {
        Platform::Print("Upgrader: {} prices of {} items from {} ({} h old)\n", out.Currency(), out.Size(), path, age / 3600);
        return true;
    }

    std::wstring request = L"/api/v2/prices/" + std::wstring(currency.begin(), currency.end()) + L".json";
    std::string body;
    PriceList fresh;
    if (HttpGet(L"market.csgo.com", request, body, error) && fresh.Parse(body, error))
    {
        FILE *f = Platform::OpenFile(path, "wb");
        if (f)
        {
            fwrite(body.data(), 1, body.size(), f);
            fclose(f);
        }
        Platform::Print("Upgrader: downloaded {} prices of {} items from market.csgo.com\n", fresh.Currency(), fresh.Size());
        out = std::move(fresh);
        return true;
    }

    if (cached)
    {
        Platform::Print("Upgrader: market.csgo.com prices not downloaded ({}), using the old ones ({} h old)\n", error, age / 3600);
        return true;
    }

    Platform::Print("Upgrader: no prices ({}), the chances go by rarity\n", error);
    return false;
}

} // namespace SkinPrices

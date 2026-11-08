#include "stdafx.h"
#include "mod_loader.h"
#include "config.h"
#include "keyvalue.h"
#include "inventory.h"
#include "item_schema.h"
#include "platform.h"
#include "skin_prices.h"
#include "vpk.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <funchook.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ModLoader
{

// <data>\mods\_build\csforge_dir.vpk. Not pakNN: filesystem_stdio checks every vpk against
// bin\win64\vpk.signatures by name. A name in the list ("pak01") from a folder the list doesn't
// have is fatal ("unexpected"), an unlisted pakNN is fatal too ("pakxx=off"), any other unlisted
// name gets mounted unsigned.
constexpr const char *VpkName = "csforge";
constexpr const char *BuildFolder = "_build";

static std::string s_builtVpk;

// folder -> enabled, priority as Build saw them, to tell the editor what changed since the game started
static std::map<std::string, std::pair<bool, int>> s_loadedState;

const std::string &BuiltVpkPath()
{
    return s_builtVpk;
}

// ------------------------------------------------------------------ KeyValues text scanning

namespace
{

struct Token
{
    enum Type
    {
        String,
        Open,
        Close,
        End,
    };

    Type type;
    size_t start; // first character of the token
    size_t end; // one past the token
};

class Scanner
{
public:
    explicit Scanner(std::string_view text, size_t pos = 0)
        : m_text{ text }
        , m_pos{ pos }
    {
        // utf-8 bom (the localization files have one)
        if (m_pos == 0 && m_text.starts_with("\xEF\xBB\xBF"))
        {
            m_pos = 3;
        }
    }

    Token Next()
    {
        while (true)
        {
            SkipWhitespace();
            if (m_pos >= m_text.size())
            {
                return { Token::End, m_pos, m_pos };
            }

            char c = m_text[m_pos];
            if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/')
            {
                // comment until the end of the line
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                {
                    m_pos++;
                }
                continue;
            }

            if (c == '[')
            {
                // [$WIN32] style conditional, not a key or value
                while (m_pos < m_text.size() && m_text[m_pos] != ']' && m_text[m_pos] != '\n')
                {
                    m_pos++;
                }
                m_pos++;
                continue;
            }

            size_t start = m_pos;
            if (c == '{')
            {
                m_pos++;
                return { Token::Open, start, m_pos };
            }

            if (c == '}')
            {
                m_pos++;
                return { Token::Close, start, m_pos };
            }

            if (c == '"')
            {
                m_pos++;
                while (m_pos < m_text.size() && m_text[m_pos] != '"')
                {
                    // escaped character (\" \\ \n)
                    m_pos += (m_text[m_pos] == '\\') ? 2 : 1;
                }
                m_pos++;
                return { Token::String, start, (std::min)(m_pos, m_text.size()) };
            }

            // unquoted token
            while (m_pos < m_text.size() && !IsSpace(m_text[m_pos]) && m_text[m_pos] != '{' && m_text[m_pos] != '}' && m_text[m_pos] != '"')
            {
                m_pos++;
            }
            return { Token::String, start, m_pos };
        }
    }

    // string token contents without the quotes
    std::string_view Value(const Token &token) const
    {
        std::string_view value = m_text.substr(token.start, token.end - token.start);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }

    // skips a block whose opening brace was just read, returns the closing brace (End if unterminated)
    Token SkipBlock()
    {
        int depth = 1;
        Token token;
        while (depth > 0)
        {
            token = Next();
            if (token.type == Token::End)
            {
                return token;
            }

            depth += (token.type == Token::Open) ? 1 : (token.type == Token::Close) ? -1 : 0;
        }
        return token;
    }

private:
    static bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    void SkipWhitespace()
    {
        while (m_pos < m_text.size() && IsSpace(m_text[m_pos]))
        {
            m_pos++;
        }
    }

    std::string_view m_text;
    size_t m_pos{};
};

std::string Lower(std::string_view text)
{
    std::string out{ text };
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct Section
{
    std::string name; // lowercase
    size_t bodyStart; // right after the opening brace
    size_t bodyEnd; // at the closing brace
};

struct Child
{
    std::string key; // lowercase
    size_t start; // key token
    size_t valueStart; // value token (string) or opening brace (block)
    size_t end; // one past the value / closing brace
    bool block;
};

// "root" { "a" "b"  "section" { ... } ... } -> the root's sections, false on malformed text
bool ParseRoot(std::string_view text, std::vector<Section> &sections, size_t &rootClose, std::string &error)
{
    Scanner scanner{ text };

    Token rootName = scanner.Next();
    Token rootOpen = scanner.Next();
    if (rootName.type != Token::String || rootOpen.type != Token::Open)
    {
        error = "expected \"root\" {";
        return false;
    }

    while (true)
    {
        Token key = scanner.Next();
        if (key.type == Token::Close)
        {
            rootClose = key.start;
            return true;
        }

        if (key.type != Token::String)
        {
            error = std::format("expected a key at offset {}", key.start);
            return false;
        }

        Token value = scanner.Next();
        if (value.type == Token::String)
        {
            continue;
        }

        if (value.type != Token::Open)
        {
            error = std::format("expected a value after '{}'", scanner.Value(key));
            return false;
        }

        Token close = scanner.SkipBlock();
        if (close.type == Token::End)
        {
            error = std::format("unterminated block '{}'", scanner.Value(key));
            return false;
        }

        sections.push_back({ Lower(scanner.Value(key)), value.end, close.start });
    }
}

// the direct children of a section body
bool ParseChildren(std::string_view text, const Section &section, std::vector<Child> &children)
{
    Scanner scanner{ text, section.bodyStart };

    while (true)
    {
        Token key = scanner.Next();
        if (key.type == Token::Close || key.start >= section.bodyEnd)
        {
            return true;
        }

        if (key.type != Token::String)
        {
            return false;
        }

        Token value = scanner.Next();
        if (value.type == Token::String)
        {
            children.push_back({ Lower(scanner.Value(key)), key.start, value.start, value.end, false });
            continue;
        }

        if (value.type != Token::Open)
        {
            return false;
        }

        Token close = scanner.SkipBlock();
        if (close.type == Token::End)
        {
            return false;
        }

        children.push_back({ Lower(scanner.Value(key)), key.start, value.start, close.end, true });
    }
}

} // namespace

bool MergeSections(std::string &baseText, std::string_view customText, MergeMode mode, MergeReport &report, std::string &error)
{
    std::vector<Section> baseSections, customSections;
    size_t baseClose = 0, customClose = 0;

    if (!ParseRoot(baseText, baseSections, baseClose, error))
    {
        error = "base: " + error;
        return false;
    }

    if (!ParseRoot(customText, customSections, customClose, error))
    {
        error = "mod: " + error;
        return false;
    }

    // (position, length to replace, text), applied back to front so earlier offsets stay valid
    struct Edit
    {
        size_t position;
        size_t length;
        std::string text;
    };
    std::vector<Edit> edits;

    // a section can show up in custom more than once, collect per name
    std::map<std::string, std::string> appendTo; // section name -> children text to add
    std::map<std::string, std::map<std::string, Child>> baseChildren; // section name -> key -> child

    for (const Section &custom : customSections)
    {
        // what the base already has for this section, all occurrences (items_game.txt has
        // e.g. 144 client_loot_lists blocks)
        auto [known, inserted] = baseChildren.try_emplace(custom.name);
        if (inserted)
        {
            for (const Section &base : baseSections)
            {
                if (base.name != custom.name)
                {
                    continue;
                }

                std::vector<Child> children;
                if (!ParseChildren(baseText, base, children))
                {
                    error = std::format("base: can't parse section '{}'", custom.name);
                    return false;
                }

                for (Child &child : children)
                {
                    known->second.try_emplace(child.key, child);
                }
            }
        }

        std::vector<Child> children;
        if (!ParseChildren(customText, custom, children))
        {
            error = std::format("mod: can't parse section '{}'", custom.name);
            return false;
        }

        for (const Child &child : children)
        {
            std::string id = custom.name + "/" + child.key;
            auto existing = known->second.find(child.key);
            if (existing != known->second.end())
            {
                bool replaceable = mode == MergeMode::OverrideExisting && !child.block && !existing->second.block
                    && existing->second.start < baseText.size() && existing->second.end <= baseText.size();
                if (replaceable && existing->second.valueStart != std::string::npos)
                {
                    edits.push_back({ existing->second.valueStart, existing->second.end - existing->second.valueStart,
                        std::string{ customText.substr(child.valueStart, child.end - child.valueStart) } });
                    report.overridden.push_back(id);

                    // a second override of the same key in this merge would overlap, keep the first
                    existing->second.valueStart = std::string::npos;
                }
                else
                {
                    report.skipped.push_back(id);
                }
                continue;
            }

            appendTo[custom.name] += std::format("\n\t\t{}", customText.substr(child.start, child.end - child.start));
            known->second.try_emplace(child.key, Child{ child.key, std::string::npos, std::string::npos, std::string::npos, child.block });
            report.added++;
        }
    }

    for (const auto &[name, text] : appendTo)
    {
        auto it = std::find_if(baseSections.begin(), baseSections.end(), [&](const Section &s) { return s.name == name; });
        if (it != baseSections.end())
        {
            // at the end of the section: the client parses paint_kits (and others) in order and the
            // first entry ("0", the default) is the template for the rest, a custom kit in front of
            // it crashes the client's schema init
            edits.push_back({ it->bodyEnd, 0, std::format("\n\t\t// cs2_gc mods{}\n\t", text) });
        }
        else
        {
            edits.push_back({ baseClose, 0, std::format("\t\"{}\"\n\t{{{}\n\t}}\n", name, text) });
        }
    }

    std::stable_sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.position > b.position; });
    for (const Edit &edit : edits)
    {
        baseText.replace(edit.position, edit.length, edit.text);
    }

    // still balanced? a broken items_game.txt is a fatal error in the client
    std::vector<Section> check;
    size_t checkClose = 0;
    if (!ParseRoot(baseText, check, checkClose, error))
    {
        error = "merged: " + error;
        return false;
    }

    return true;
}

// ------------------------------------------------------------------ files

static std::filesystem::path Utf8Path(std::string_view path)
{
    return std::filesystem::path{ std::u8string_view{ reinterpret_cast<const char8_t *>(path.data()), path.size() } };
}

static std::string PathUtf8(const std::filesystem::path &path)
{
    std::u8string text = path.u8string();
    return std::string{ reinterpret_cast<const char *>(text.data()), text.size() };
}

static bool ReadWholeFile(const std::string &path, std::string &out)
{
    out.clear();
    FILE *f = Platform::OpenFile(path, "rb");
    if (!f)
    {
        return false;
    }

    char buffer[65536];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        out.append(buffer, read);
    }

    fclose(f);
    return true;
}

static bool FileExists(const std::string &path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(Utf8Path(path), ec);
}

std::vector<ModInfo> ListMods()
{
    std::vector<ModInfo> mods;
    std::error_code ec;
    std::filesystem::path root = Utf8Path(Platform::DataPath("mods"));

    for (auto it = std::filesystem::directory_iterator{ root, ec }; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
    {
        if (!it->is_directory(ec))
        {
            continue;
        }

        std::string folder = PathUtf8(it->path().filename());
        if (folder.empty() || folder[0] == '_' || folder[0] == '.')
        {
            continue;
        }

        ModInfo mod;
        mod.folder = folder;
        mod.name = folder;

        std::string infoPath = PathUtf8(it->path() / "mod.txt");
        if (FileExists(infoPath))
        {
            KeyValueRead info{ infoPath.c_str() };
            const KeyValue *key = info.Good() ? info.Root().GetSubkey("mod") : nullptr;
            if (key)
            {
                mod.name = key->GetString("name", folder);
                mod.author = key->GetString("author");
                mod.version = key->GetString("version");
                mod.description = key->GetString("description");
                mod.enabled = key->GetNumber<int>("enabled", 1) != 0;
                mod.priority = key->GetNumber<int>("priority", 0);
            }
            else
            {
                Platform::Print("CSForge: {} doesn't parse (expected \"mod\" {{ ... }}), using defaults\n", infoPath);
            }
        }

        mods.push_back(std::move(mod));
    }

    // load order: lowest priority first, later ones win conflicts
    std::sort(mods.begin(), mods.end(), [](const ModInfo &a, const ModInfo &b) {
        return a.priority != b.priority ? a.priority < b.priority : a.folder < b.folder;
    });

    return mods;
}

// every file under <mod>\files, game relative lowercase paths like vpk trees have
static void CollectModFiles(const std::filesystem::path &filesDir, std::vector<std::pair<std::string, std::filesystem::path>> &out)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(filesDir, ec))
    {
        return;
    }

    for (auto it = std::filesystem::recursive_directory_iterator{ filesDir, ec }; !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec))
    {
        if (it->is_regular_file(ec))
        {
            std::u8string relative = std::filesystem::relative(it->path(), filesDir, ec).generic_u8string();
            out.emplace_back(Lower(std::string_view{ reinterpret_cast<const char *>(relative.data()), relative.size() }), it->path());
        }
    }
}

static std::string ModPath(const ModInfo &mod, std::string_view file)
{
    return Platform::DataPath("mods") + "\\" + mod.folder + "\\" + std::string{ file };
}

// ------------------------------------------------------------------ built-in Panorama fixes

bool PatchResourceText(std::string &file, std::string_view anchor, std::string_view insert, std::string &error)
{
    // a compiled resource: uint32 file size, uint16 header version, uint16 version, uint32 offset of
    // the block table (from its own field), uint32 block count; then { char[4] type, uint32 offset
    // (from its own field), uint32 size } per block. A .vts_c has RED2 (input dependencies), DATA
    // (the JavaScript as plain text) and STAT (no checksum of the text)
    auto read32 = [&](size_t pos) {
        uint32_t value = 0;
        memcpy(&value, file.data() + pos, 4);
        return value;
    };
    auto write32 = [](std::string &out, size_t pos, size_t value) {
        uint32_t v = static_cast<uint32_t>(value);
        memcpy(out.data() + pos, &v, 4);
    };

    if (file.size() < 16 || read32(0) != file.size())
    {
        error = "not a compiled resource";
        return false;
    }

    struct Block
    {
        size_t entry;
        std::string type;
        size_t start;
        size_t size;
    };
    std::vector<Block> blocks;
    size_t table = 8 + read32(8);
    uint32_t count = read32(12);
    for (uint32_t i = 0; i < count; i++)
    {
        size_t entry = table + i * 12;
        if (entry + 12 > file.size())
        {
            error = "broken block table";
            return false;
        }
        Block block{ entry, file.substr(entry, 4), entry + 4 + read32(entry + 4), read32(entry + 8) };
        if (block.start + block.size > file.size())
        {
            error = "broken block " + block.type;
            return false;
        }
        blocks.push_back(std::move(block));
    }

    auto data = std::find_if(blocks.begin(), blocks.end(), [](const Block &block) { return block.type == "DATA"; });
    if (data == blocks.end())
    {
        error = "no DATA block";
        return false;
    }

    std::string_view text{ file.data() + data->start, data->size };
    size_t at = text.find(anchor);
    if (at == std::string_view::npos || text.find(anchor, at + 1) != std::string_view::npos)
    {
        error = "the game's script changed (anchor not found once)";
        return false;
    }

    // everything up to DATA stays, the new text, then the blocks that came after it, 16 byte aligned
    std::string out = file.substr(0, data->start);
    out.append(text.substr(0, at));
    out.append(insert);
    out.append(text.substr(at));
    write32(out, data->entry + 8, data->size + insert.size());

    std::vector<const Block *> after;
    for (const Block &block : blocks)
    {
        if (block.start > data->start)
        {
            after.push_back(&block);
        }
    }
    std::sort(after.begin(), after.end(), [](const Block *a, const Block *b) { return a->start < b->start; });
    for (const Block *block : after)
    {
        out.resize((out.size() + 15) & ~size_t{ 15 }, '\0');
        write32(out, block->entry + 4, out.size() - (block->entry + 4));
        out.append(file, block->start, block->size);
    }

    write32(out, 0, out.size());
    file = std::move(out);
    return true;
}

// JavaScript put into the game's own Panorama scripts at every start (so a game update can't make a
// stale copy override the new script; if the anchor moved, the fix is skipped with a line in the log)
struct PanoramaFix
{
    std::string_view path;
    std::string_view anchor; // the code goes right before it
    std::string_view code;
    std::string_view what;
};

static constexpr PanoramaFix StaticPanoramaFixes[] = {
    {
        "panorama/scripts/popups/popup_capability_decodable.vts_c",
        "_SetUpPanelElements();\n        $.DispatchEvent('CapabilityPopupIsOpen', true);",
        // without a key and no key to buy (during a match the store sells nothing) the popup had no
        // bar at all, not even Close, and stayed on screen. Now it's inspect only, like a rental case
        "if (!InspectShared.GetPopupSetting('tool_id') && !InspectShared.GetPopupSetting('is_keyless') &&\n"
        "            !InspectShared.GetPopupSetting('inspect_only') && !InspectShared.GetPopupSetting('store_item_id')) {\n"
        "            const csgcInMatch = GameStateAPI.IsLocalPlayerPlayingMatch();\n"
        "            const csgcKey = InspectShared.GetPopupSetting('purchase_item_id');\n"
        "            if (csgcInMatch || !csgcKey || !ItemInfo.GetStoreOriginalPrice(csgcKey, 1)) {\n"
        "                InspectShared.SetPopupSetting('show_work_type_warning', false);\n"
        "                InspectShared.SetPopupSetting('inspect_only', true);\n"
        "                InspectShared.SetPopupSetting('only_close_btn', true);\n"
        "                InspectShared.SetPopupSetting('force_hide_async_bar', true);\n"
        "                if (csgcInMatch)\n"
        "                    UiToolkitAPI.ShowGenericPopupOk($.Localize('#inv_context_open_package'), $.Localize('#SFUI_InvError_OpenCrate_PauseMenu'), '', () => { });\n"
        "            }\n"
        "        }\n"
        "        ",
        "a container without a key to buy (during a match) is inspect only, with Close",
    },
};

// ------------------------------------------------------------------ the upgrader

static std::vector<UpgradeTarget> s_upgraderCatalog;
static std::unordered_map<uint64_t, size_t> s_upgraderIndex; // def << 32 | paint kit -> catalog index
static std::string s_upgraderCurrency;
static int64_t s_upgraderPriceTime;

std::span<const UpgradeTarget> UpgraderCatalog()
{
    return s_upgraderCatalog;
}

const std::string &UpgraderCurrency()
{
    return s_upgraderCurrency;
}

int64_t UpgraderPriceTime()
{
    return s_upgraderPriceTime;
}

// the game's English texts (resource/csgo_english.txt, lines of "token" "text"), lowercase tokens:
// the market names skins in English whatever the game's language
static std::unordered_map<std::string, std::string> ReadEnglishTokens(const std::string &gameVpk)
{
    std::unordered_map<std::string, std::string> tokens;
    std::vector<char> data;
    if (!VpkReadFile(gameVpk, "resource/csgo_english.txt", data))
    {
        return tokens;
    }

    auto quoted = [](std::string_view &line, std::string &out) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos || line[start] != '"')
        {
            return false;
        }
        out.clear();
        for (size_t i = start + 1; i < line.size(); i++)
        {
            if (line[i] == '\\' && i + 1 < line.size())
            {
                out += line[++i];
            }
            else if (line[i] == '"')
            {
                line.remove_prefix(i + 1);
                return true;
            }
            else
            {
                out += line[i];
            }
        }
        return false;
    };

    std::string_view text{ data.data(), data.size() };
    std::string key, value;
    while (!text.empty())
    {
        size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (quoted(line, key) && quoted(line, value))
        {
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            tokens.try_emplace(key, value);
        }
    }
    return tokens;
}

size_t BuildUpgraderCatalog()
{
    const GCConfig &config = GetConfig();
    SkinPrices::PriceList prices;
    bool priced = !config.m_upgraderPrices.empty()
        && SkinPrices::LoadPriceList(config.m_upgraderPrices, config.m_upgraderPricesHours, prices);
    return BuildUpgraderCatalog(priced ? &prices : nullptr);
}

size_t BuildUpgraderCatalog(const SkinPrices::PriceList *prices)
{
    // the game's own schema: the same list goes into the window and stays here for the gc
    ItemSchema schema;
    std::unordered_map<std::string, std::string> english;
    if (prices)
    {
        english = ReadEnglishTokens(Platform::GamePath("pak01_dir.vpk"));
    }
    auto englishText = [&](std::string_view token) -> std::string_view {
        std::string key{ token.starts_with('#') ? token.substr(1) : token };
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto it = english.find(key);
        return it != english.end() ? std::string_view{ it->second } : std::string_view{};
    };

    s_upgraderCatalog.clear();
    s_upgraderIndex.clear();
    s_upgraderCurrency = prices ? prices->Currency() : std::string{};
    s_upgraderPriceTime = prices ? prices->Time() : 0;
    size_t wears = 0, found = 0, unnamed = 0;
    for (const ItemSchema::PaintedItem &item : schema.PaintedItems())
    {
        uint32_t tier = Inventory::UpgradeTier(item.rarity, item.quality);
        if (!tier)
        {
            continue;
        }

        UpgradeTarget target{ FromEnum(item.defIndex), item.paintKit, tier };
        bool star = item.quality == Quality::Unusual; // knives and gloves
        bool canStatTrak = target.defIndex < 1000; // not gloves
        std::string_view weapon = englishText(schema.ItemNameToken(item.defIndex));
        std::string_view finish = englishText(schema.PaintKitNameToken(item.paintKit));
        unnamed += prices && (weapon.empty() || finish.empty());
        uint64_t madeUp = 1;
        for (uint32_t i = 0; i < tier; i++)
        {
            madeUp *= 10;
        }

        for (int wear = 0; wear < SkinPrices::WearCount; wear++)
        {
            if (!SkinPrices::WearPossible(wear, item.minFloat, item.maxFloat))
            {
                continue;
            }

            if (!prices)
            {
                target.price[wear] = madeUp;
                target.statTrak[wear] = canStatTrak ? madeUp : 0;
                target.souvenir[wear] = star ? 0 : madeUp;
                continue;
            }

            if (weapon.empty() || finish.empty())
            {
                continue;
            }
            target.price[wear] = prices->Find(SkinPrices::MarketHashName(weapon, finish, wear, star, false, false));
            if (canStatTrak)
            {
                target.statTrak[wear] = prices->Find(SkinPrices::MarketHashName(weapon, finish, wear, star, true, false));
            }
            if (!star)
            {
                target.souvenir[wear] = prices->Find(SkinPrices::MarketHashName(weapon, finish, wear, false, false, true));
            }
            wears++;
            found += target.price[wear] || target.statTrak[wear] || target.souvenir[wear];
        }

        s_upgraderIndex[(static_cast<uint64_t>(target.defIndex) << 32) | target.paintKit] = s_upgraderCatalog.size();
        s_upgraderCatalog.push_back(target);
    }

    if (prices)
    {
        Platform::Print("Upgrader: {} skins, {} of {} wears have a price ({} skins without an English name)\n",
            s_upgraderCatalog.size(), found, wears, unnamed);
    }
    return s_upgraderCatalog.size();
}

uint64_t UpgraderItemPrice(const Item &item)
{
    uint32_t paintKit = static_cast<uint32_t>(item.GetAttributeValue<float>(AttributeDefIndex::TexturePrefab));
    auto it = s_upgraderIndex.find((static_cast<uint64_t>(FromEnum(item.DefIndex())) << 32) | paintKit);
    if (!paintKit || it == s_upgraderIndex.end())
    {
        return 0;
    }

    const UpgradeTarget &target = s_upgraderCatalog[it->second];
    int wear = SkinPrices::WearOf(item.GetAttributeValue<float>(AttributeDefIndex::TextureWear));
    if (item.HasAttribute(AttributeDefIndex::KillEater))
    {
        return target.statTrak[wear];
    }
    if (item.GetDesc().quality == Quality::Tournament)
    {
        return target.souvenir[wear];
    }
    return target.price[wear];
}

// the context menu entry on a skin: puts it in the upgrader tab's deposit and opens the tab, through
// the hook the inventory script leaves on its InventoryMain panel (Data() is how the game itself
// hands things between scripts, see oSettings of the popups)
static constexpr std::string_view UpgraderMenuEntry = R"JS({
            name: 'csgc_upgrade',
            style: (id) => 'TopSeparator',
            AvailableForItem: (id) => {
                if (InventoryAPI.IsFauxItemID(id) || InventoryAPI.IsRental(id))
                    return false;
                const slot = InventoryAPI.GetDefaultSlot(id);
                if (!slot || slot === 'c4' || ItemInfo.IsEquippalbleButNotAWeapon(id))
                    return false;
                // a stock gun with a charm or a name is a real item too, but normal quality. Knives
                // and gloves can go in too: the window looks up the price
                if (Number(InventoryAPI.GetItemQuality(id)) === 0)
                    return false;
                return Number(InventoryAPI.GetItemRarity(id)) >= 1;
            },
            OnSelected: (id) => {
                $.DispatchEvent('ContextMenuEvent', '');
                let root = $.GetContextPanel();
                while (root.GetParent())
                    root = root.GetParent();
                const main = root.FindChildTraverse('InventoryMain');
                if (main && main.Data().csgcUpgrade)
                    main.Data().csgcUpgrade(id);
                else
                    $.Msg('[csgc upgrader] the inventory tab is not there\n');
            }
        },
        )JS";

// the upgrader tab, built from script with plain panels, a sub tab of the inventory next to the
// game's own (mainmenu_inventory makes those from script too). The choice goes to the gc as
// SetItemAttributeValueAsync (k_EMsgGCModifyItemAttribute, any stored_as_integer attribute passes):
// "upgrade threshold" = target index + 1 on every deposit, then "upgrade level" = deposit count.
// The wheel: a ring with the chance as an arc at the bottom (clip: radial, like the Armory's
// progress ring), the percent in a smaller circle, a needle that spins to where the gc's roll says
static constexpr std::string_view UpgraderModule = R"JS(var CsgcUpgrader = (function () {
    // per skin: def, paint kit, tier (the color), then its prices per wear (Factory New .. Battle-Scarred)
    // in hundredths of the currency: 5 plain, 5 StatTrak, 5 souvenir; 0 = not on sale
    const CATALOG = [@CATALOG@];
    const STRIDE = 18;
    const CURRENCY = '@CURRENCY@'; // empty: no price list, the prices are 10^tier (1:10 per rarity step)
    const PRICE_TIME = @PRICE_TIME@;
    const COUNT = CATALOG.length / STRIDE;
    const COLORS = ['#ffffff', '#b0c3d9', '#5e98d9', '#4b69ff', '#8847ff', '#d32ce6', '#eb4b4b', '#e4ae39'];
    const WEARS = ['FN', 'MW', 'FT', 'WW', 'BS'];
    const MULTIPLIERS = [1.5, 2, 3, 5, 10];
    const PAGE_SIZE = 20;
    const MAX_DEPOSITS = 10;
    const SPIN_SECONDS = 6;
    const SPIN_TURNS = 8;
    const REVEAL_DELAY = 0.8; // the game's new item popup, this long after the needle stops
    const REST = 180; // the needle waits at the bottom, in the middle of the win arc
    const byKey = {}; // def * 65536 + paint kit -> catalog index
    for (let i = 0; i < COUNT; i++)
        byKey[CATALOG[i * STRIDE] * 65536 + CATALOG[i * STRIDE + 1]] = i;
    const names = {};
    let prices = {}; // item id -> what it's worth, until the next inventory update
    let categories = null;
    const state = { deposits: [], target: null, tier: 0, kind: 'all', same: false, statTrak: false, multiplier: 0, page: 0, picking: false,
        busy: false, result: null, rotation: REST, spinChance: 0, updates: 0, hold: null, note: '' };
    let ui = null;
    let panel = null;
    function T(token, fallback) {
        const text = $.Localize('#' + token);
        return (!text || text.charAt(0) === '#') ? fallback : text;
    }
    function Money(hundredths) {
        if (!CURRENCY || !hundredths)
            return '';
        const fixed = (hundredths / 100).toFixed(hundredths >= 100000 ? 0 : 2).split('.');
        const text = fixed[0].replace(/\B(?=(\d{3})+(?!\d))/g, ' ') + (fixed.length > 1 ? '.' + fixed[1] : '');
        if (CURRENCY === 'USD')
            return '$' + text;
        if (CURRENCY === 'EUR')
            return text + ' €';
        if (CURRENCY === 'RUB')
            return text + ' ' + T('csgc_upgrade_rub', 'RUB');
        return text + ' ' + CURRENCY;
    }
    function Percent(chance) {
        const p = chance * 100;
        return (p >= 1 ? p.toFixed(2) : p >= 0.01 ? p.toFixed(3) : p.toFixed(5)) + '%';
    }
    // kind: 0 plain, 1 StatTrak, 2 souvenir
    function Price(index, wear, kind) {
        return CATALOG[index * STRIDE + 3 + kind * 5 + wear] || 0;
    }
    function TierOf(id) {
        const slot = InventoryAPI.GetDefaultSlot(id);
        if (slot === 'melee' || slot === 'clothing_hands')
            return 7;
        return Number(InventoryAPI.GetItemRarity(id)) || 0;
    }
    function Kind(def) {
        return def >= 4700 ? 'gloves' : def >= 500 ? 'knives' : 'guns';
    }
    function Faux(index) {
        return InventoryAPI.GetFauxItemIDFromDefAndPaintIndex(CATALOG[index * STRIDE], CATALOG[index * STRIDE + 1]);
    }
    function Name(index) {
        if (!(index in names))
            names[index] = InventoryAPI.GetItemName(Faux(index));
        return names[index];
    }
    // an inventory item's catalog skin: by its paint kit, or by the name when that can't be read
    // (the item's name has the finish's in it, with StatTrak or Souvenir in front)
    function IndexOf(id) {
        const def = Number(InventoryAPI.GetItemDefinitionIndex(id));
        const index = byKey[def * 65536 + Number(InventoryAPI.GetItemAttributeValue(id, 'set item texture prefab'))];
        if (index !== undefined)
            return index;
        const name = InventoryAPI.GetItemName(id) || '';
        for (let i = 0; i < COUNT; i++) {
            if (CATALOG[i * STRIDE] === def && name.indexOf(Name(i)) >= 0)
                return i;
        }
        return -1;
    }
    // what an inventory item is worth, the way the gc counts it: its wear, StatTrak, souvenir
    function ItemPrice(id) {
        if (id in prices)
            return prices[id];
        let price = 0;
        const index = IndexOf(id);
        if (index >= 0) {
            let wear = Number(InventoryAPI.GetWear(id));
            if (!(wear >= 0 && wear <= 4))
                wear = 0;
            const statTrak = InventoryAPI.GetItemAttributeValue(id, 'kill eater') !== undefined;
            const souvenir = Number(InventoryAPI.GetItemQuality(id)) === 12;
            price = Price(index, wear, statTrak ? 1 : souvenir ? 2 : 0);
        }
        prices[id] = price;
        return price;
    }
    // the guns, knives and gloves in the inventory, the way the game's own search lists them: the
    // filter wants a loadout category first, flags alone give nothing. Also how a burnt deposit is
    // noticed (IsValidItemID only looks at the id's form)
    function InventoryItems() {
        if (!categories) {
            categories = ['secondary', 'smg', 'rifle', 'heavy', 'melee', 'clothing_hands'];
            try {
                InventoryAPI.GetCategories().split(',').forEach((category) => {
                    InventoryAPI.GetSubCategories(category).split(',').forEach((sub) => {
                        if (sub && categories.indexOf(sub) < 0 && /melee|knife|glove|hand|clothing/.test(sub))
                            categories.push(sub);
                    });
                });
            }
            catch (e) {
                $.Msg('[csgc upgrader] categories: ' + e + '\n');
            }
            $.Msg('[csgc upgrader] inventory categories: ' + categories.join(', ') + '\n');
        }
        const ids = [];
        const seen = {};
        categories.forEach((category) => {
            InventoryAPI.SetInventorySortAndFilters('inv_sort_rarity', false, category + ',is_rental:false', '', '');
            const count = InventoryAPI.GetInventoryCount();
            for (let i = 0; i < count; i++) {
                const id = InventoryAPI.GetInventoryItemIDByIndex(i);
                if (!seen[id]) {
                    seen[id] = true;
                    ids.push(id);
                }
            }
        });
        return ids;
    }
    // why an item can't go in: '' if it can
    function Reject(id) {
        if (!id || !InventoryAPI.IsValidItemID(id) || InventoryAPI.IsFauxItemID(id) || InventoryAPI.IsRental(id))
            return 'id';
        const slot = InventoryAPI.GetDefaultSlot(id);
        if (!slot || ['c4', 'flair0', 'musickit', 'spray0', 'customplayer', 'pet'].indexOf(slot) >= 0)
            return 'slot';
        // stock guns (with a charm, stickers or a name they're real items too) are normal quality
        if (Number(InventoryAPI.GetItemQuality(id)) === 0)
            return 'stock';
        return ItemPrice(id) > 0 ? '' : 'price';
    }
    function CanDeposit(id) {
        return Reject(id) === '';
    }
    function BetValue() {
        let total = 0;
        for (const id of state.deposits) {
            const price = ItemPrice(id);
            if (!price)
                return 0;
            total += price;
        }
        return total;
    }
    function TargetPrice(target) {
        target = target || state.target;
        return target ? Price(target.index, target.wear, target.st ? 1 : 0) : 0;
    }
    // what the bet is worth over the target's price; 0 when the target isn't dearer
    function Chance() {
        const bet = BetValue();
        const price = TargetPrice();
        return bet > 0 && price > bet ? bet / price : 0;
    }
    function TargetLine(target, bet) {
        const price = TargetPrice(target);
        return WEARS[target.wear] + (target.st ? ' ST' : '') + (CURRENCY ? ' · ' + Money(price) : '') +
            (bet > 0 && price > bet ? ' · ' + Percent(bet / price) : '');
    }
    function RuleText() {
        if (!CURRENCY)
            return T('csgc_upgrade_rule', '1:10 per rarity step, the chances of several skins add up. The skins burn if it fails.');
        const date = new Date(PRICE_TIME * 1000);
        const two = (n) => (n < 10 ? '0' : '') + n;
        return T('csgc_upgrade_rule_prices', 'Chance = what the bet is worth / the target\'s price, the target has to cost more. The skins burn if it fails. Prices:') +
            ' market.csgo.com, ' + two(date.getDate()) + '.' + two(date.getMonth() + 1) + ' ' + two(date.getHours()) + ':' + two(date.getMinutes());
    }
    function Style(target, style) {
        for (const key in style)
            target.style[key] = style[key];
        return target;
    }
    function Box(parent, flow, style) {
        const box = $.CreatePanel('Panel', parent, '');
        box.style.flowChildren = flow;
        return Style(box, style || {});
    }
    function Text(parent, text, style) {
        const label = $.CreatePanel('Label', parent, '');
        label.text = text;
        return Style(label, Object.assign({ color: '#ffffffdd', fontSize: '18px' }, style || {}));
    }
    function Btn(parent, text, onClick, style) {
        const button = $.CreatePanel('Button', parent, '');
        Style(button, Object.assign({ backgroundColor: '#ffffff1a', padding: '6px 12px', margin: '3px' }, style || {}));
        if (text)
            Text(button, text, { fontSize: '16px', horizontalAlign: 'center', verticalAlign: 'center' });
        button.SetPanelEvent('onactivate', onClick);
        return button;
    }
    function Tile(parent, itemId, tier, label, onClick, selected, sub) {
        const tile = $.CreatePanel('Button', parent, '');
        Style(tile, { width: '132px', height: '146px', margin: '4px', flowChildren: 'down', backgroundColor: selected ? '#ffffff38' : '#ffffff0d' });
        const image = $.CreatePanel('ItemImage', tile, '', { itemid: itemId, scaling: 'stretch-to-fit-preserve-aspect' });
        image.itemid = itemId;
        Style(image, { width: '124px', height: '84px', horizontalAlign: 'center' });
        Box(tile, 'none', { width: '100%', height: '3px', backgroundColor: COLORS[tier] || '#ffffff' });
        Text(tile, label, { fontSize: '13px', width: '100%', textAlign: 'center', textOverflow: 'shrink' });
        if (sub)
            Text(tile, sub, { fontSize: '13px', color: '#e8c060', width: '100%', textAlign: 'center', textOverflow: 'shrink' });
        if (onClick)
            tile.SetPanelEvent('onactivate', onClick);
        return tile;
    }
)JS"
    R"JS(    function BuildWheel(parent) {
        const size = 300;
        const wheel = Box(parent, 'none', { width: size + 'px', height: size + 'px', horizontalAlign: 'center', margin: '6px 0px 12px 0px' });
        Box(wheel, 'none', { width: '100%', height: '100%', borderRadius: '50%', backgroundColor: '#3b3f45' });
        Box(wheel, 'none', { width: (size - 14) + 'px', height: (size - 14) + 'px', borderRadius: '50%', backgroundColor: '#17191c', horizontalAlign: 'center', verticalAlign: 'center' });
        ui.arc = Box(wheel, 'none', { width: (size - 14) + 'px', height: (size - 14) + 'px', borderRadius: '50%', backgroundColor: '#e8840c', horizontalAlign: 'center', verticalAlign: 'center' });
        ui.arc.style.clip = 'radial(50% 50%, 180deg, 0deg)';
        const inner = Box(wheel, 'down', { width: (size - 92) + 'px', height: (size - 92) + 'px', borderRadius: '50%', backgroundColor: '#22252a', border: '3px solid #4a4e55', horizontalAlign: 'center', verticalAlign: 'center' });
        ui.percent = Text(inner, '-', { fontSize: '42px', color: '#ffffff', horizontalAlign: 'center', margin: '64px 0px 0px 0px' });
        Text(inner, T('csgc_upgrade_chance', 'Chance'), { fontSize: '18px', color: '#ffffffaa', horizontalAlign: 'center' });
        ui.spinner = Box(wheel, 'none', { width: '100%', height: '100%' });
        Box(ui.spinner, 'none', { width: '10px', height: '50px', backgroundColor: '#ffffff', borderRadius: '5px', border: '2px solid #000000aa', horizontalAlign: 'center', verticalAlign: 'top', margin: '2px 0px 0px 0px' });
        ui.spinner.style.transform = 'rotateZ(' + REST + 'deg)';
    }
    function SetWheel(chance) {
        const sweep = Math.max(0, Math.min(360, chance * 360));
        ui.arc.style.clip = 'radial(50% 50%, ' + (180 - sweep / 2).toFixed(3) + 'deg, ' + sweep.toFixed(3) + 'deg)';
        ui.percent.text = chance > 0 ? Percent(chance) : '-';
    }
    // the game shows its new item popup on every inventory update and the count on the inventory
    // button: both held back while the wheel spins, the way the game's own case and trade up
    // popups do it (CapabilityPopupIsOpen), or the won skin shows before the needle stops
    function HoldReveal() {
        let root = panel;
        while (root.GetParent())
            root = root.GetParent();
        state.hold = { alert: root.FindChildTraverse('MainMenuInvAlert'), count: InventoryAPI.GetUnacknowledgeItemsCount() };
        $.DispatchEvent('CapabilityPopupIsOpen', true);
    }
    function KeepAlert() {
        const alert = state.hold && state.hold.alert;
        if (alert && alert.IsValid()) {
            alert.SetDialogVariable('alert_value', state.hold.count.toString());
            alert.SetHasClass('hidden', state.hold.count < 1);
        }
    }
    function Reveal() {
        if (!state.hold)
            return;
        state.hold.count = InventoryAPI.GetUnacknowledgeItemsCount();
        KeepAlert();
        state.hold = null;
        $.DispatchEvent('CapabilityPopupIsOpen', false); // the inventory looks again: the popup
    }
    function Spin(won, then) {
        // the needle lands inside the win arc (centered at the bottom) or outside it, after 8 turns.
        // Turned frame by frame: a transition goes from one rotateZ to the other the short way,
        // whole turns in between are lost
        const sweep = Math.max(0, Math.min(360, state.spinChance * 360));
        let land;
        if (won)
            land = 180 + (Math.random() - 0.5) * sweep * 0.8;
        else
            land = (180 + sweep / 2 + (360 - sweep) * (0.1 + 0.8 * Math.random())) % 360;
        const start = ((state.rotation % 360) + 360) % 360;
        let delta = land - start;
        if (delta < 0)
            delta += 360;
        const total = 360 * SPIN_TURNS + delta;
        const began = Date.now();
        let notch = 0;
        let ticked = 0;
        ui.spinner.style.transitionDuration = '0s';
        const frame = () => {
            const t = Math.min(1, (Date.now() - began) / (SPIN_SECONDS * 1000));
            // fast from the start, slowing down towards the end (ease out cubic)
            const turned = total * (1 - Math.pow(1 - t, 3));
            ui.spinner.style.transform = 'rotateZ(' + ((start + turned) % 360).toFixed(2) + 'deg)';
            // a tick every 30 degrees, no closer than 70 ms (a rattle while fast, clicks at the end)
            if (Math.floor(turned / 30) !== notch && Date.now() - ticked > 70) {
                notch = Math.floor(turned / 30);
                ticked = Date.now();
                $.DispatchEvent('CSGOPlaySoundEffect', 'UIPanorama.XP.Ticker', 'MOUSE');
            }
            KeepAlert();
            if (t < 1) {
                $.Schedule(0.01, frame);
                return;
            }
            state.rotation = (start + total) % 360;
            then();
        };
        frame();
    }
    function Build(target) {
        panel = target;
        ui = {};
        panel.style.flowChildren = 'down';
        ui.status = Text(panel, RuleText(), { fontSize: '16px', color: '#ffffff99', margin: '10px 16px 4px 16px' });
        const body = Box(panel, 'right', { width: '100%', height: 'fill-parent-flow(1.0)', margin: '4px 16px 12px 16px' });
        ui.left = Box(body, 'down', { width: '300px', height: '100%' });
        const middle = Box(body, 'down', { width: '340px', height: '100%', margin: '0px 16px' });
        ui.right = Box(body, 'down', { width: 'fill-parent-flow(1.0)', height: '100%' });
        BuildWheel(middle);
        ui.middle = Box(middle, 'down', { width: '100%' });
    }
    function Render() {
        if (!ui)
            return;
        ui.status.text = state.note || RuleText();
        // each part on its own, so one failing doesn't take the others with it
        const errors = [];
        [['left', RenderLeft], ['middle', RenderMiddle], ['right', RenderRight]].forEach((part) => {
            try {
                part[1]();
            }
            catch (e) {
                errors.push(part[0] + ': ' + e);
                $.Msg('[csgc upgrader] ' + part[0] + ': ' + e + '\n' + e.stack + '\n');
            }
        });
        if (errors.length)
            ui.status.text = 'Upgrader: ' + errors.join('; ');
    }
    function RenderLeft() {
        ui.left.RemoveAndDeleteChildren();
        const bet = BetValue();
        Text(ui.left, T('csgc_upgrade_deposit', 'Your skins') + ' (' + state.deposits.length + '/' + MAX_DEPOSITS + ')' +
            (CURRENCY && bet ? ': ' + Money(bet) : ''), { fontSize: '20px' });
        const grid = Box(ui.left, 'right-wrap', { width: '100%', height: 'fill-parent-flow(1.0)', overflow: 'squish scroll' });
        state.deposits.forEach((id) => {
            Tile(grid, id, TierOf(id), InventoryAPI.GetItemName(id), () => {
                if (state.busy)
                    return;
                state.deposits = state.deposits.filter((other) => other !== id);
                state.result = null;
                Render();
            }, false, Money(ItemPrice(id)));
        });
        if (state.deposits.length < MAX_DEPOSITS && !state.busy) {
            Btn(ui.left, state.picking ? T('csgc_upgrade_back', 'Back to the targets') : '+ ' + T('csgc_upgrade_add', 'Add a skin'), () => {
                state.picking = !state.picking;
                state.page = 0;
                Render();
            }, { width: '100%' });
        }
    }
)JS"
    R"JS(    function RenderMiddle() {
        ui.middle.RemoveAndDeleteChildren();
        const bet = BetValue();
        const chance = state.busy ? state.spinChance : Chance();
        if (!state.busy)
            SetWheel(state.result ? state.spinChance : chance);
        const row = Box(ui.middle, 'right', { horizontalAlign: 'center' });
        if (state.result && !state.busy) {
            if (state.result.won) {
                Tile(row, state.result.won, TierOf(state.result.won), InventoryAPI.GetItemName(state.result.won), null, true, Money(ItemPrice(state.result.won)));
                Text(ui.middle, T('csgc_upgrade_won', 'You got it!'), { fontSize: '26px', color: '#7fe07f', horizontalAlign: 'center' });
            }
            else {
                Text(ui.middle, T('csgc_upgrade_lost', 'No luck, the skins are gone'), { fontSize: '22px', color: '#e07f7f', horizontalAlign: 'center' });
            }
        }
        else if (state.target) {
            const tier = CATALOG[state.target.index * STRIDE + 2];
            Tile(row, Faux(state.target.index), tier, Name(state.target.index), null, true, TargetLine(state.target, 0));
            if (bet && TargetPrice() <= bet)
                Text(ui.middle, T('csgc_upgrade_too_cheap', 'The target has to cost more than the bet'), { fontSize: '15px', color: '#e07f7f', horizontalAlign: 'center' });
        }
        else {
            Text(ui.middle, T('csgc_upgrade_pick_target', 'Pick the skin you want on the right'), { fontSize: '15px', color: '#ffffff88', horizontalAlign: 'center' });
        }
        if (CURRENCY && !state.result && (bet || state.target))
            Text(ui.middle, (Money(bet) || '0') + '  →  ' + (Money(TargetPrice()) || '?'), { fontSize: '18px', color: '#e8c060', horizontalAlign: 'center', margin: '4px 0px 0px 0px' });
        const go = Btn(ui.middle, state.busy ? T('csgc_upgrade_wait', 'Rolling...') : T('csgc_upgrade_go', 'Upgrade'), Go,
            { width: '100%', height: '56px', margin: '10px 0px', backgroundColor: chance > 0 && !state.busy ? '#4b9e3fcc' : '#ffffff14' });
        go.enabled = chance > 0 && !state.busy;
    }
    function RenderPick(filters, items) {
        Text(filters, T('csgc_upgrade_your_items', 'Your skins'), { fontSize: '18px', margin: '4px' });
        const all = InventoryItems();
        const skipped = { id: 0, slot: 0, stock: 0, price: 0, deposit: 0 };
        all.forEach((id) => {
            const reason = Reject(id) || (state.deposits.indexOf(id) >= 0 ? 'deposit' : '');
            if (reason) {
                skipped[reason]++;
                return;
            }
            items.push({ id: id, tier: TierOf(id), name: InventoryAPI.GetItemName(id), price: ItemPrice(id), pick: () => {
                if (state.busy)
                    return;
                state.deposits.push(id);
                if (state.deposits.length >= MAX_DEPOSITS)
                    state.picking = false;
                state.result = null;
                state.note = '';
                Render();
            } });
        });
        items.forEach((item) => { item.sub = Money(item.price); });
        items.sort((a, b) => (b.price - a.price) || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
        if (!items.length) {
            // say why in words, so a screenshot tells what's wrong. The base guns (faux ids, one
            // per weapon, 'id') are in every category list and don't count
            const words = { stock: T('csgc_upgrade_skip_stock', 'stock'), price: T('csgc_upgrade_skip_price', 'without a market price'),
                deposit: T('csgc_upgrade_skip_deposit', 'in the bet'), slot: T('csgc_upgrade_skip_other', 'other') };
            const why = T('csgc_upgrade_guns_owned', 'skins in the inventory') + ': ' + (all.length - skipped.id) +
                Object.keys(words).filter((k) => skipped[k]).map((k) => ', ' + words[k] + ' ' + skipped[k]).join('');
            Text(filters, T('csgc_upgrade_no_guns', 'No skins with a market price to put in') + ' (' + why + ')', { fontSize: '15px', color: '#ffffff88', margin: '4px' });
            $.Msg('[csgc upgrader] deposit list empty: ' + all.length + ' listed, ' + JSON.stringify(skipped) + '\n');
        }
    }
    function Toggle(active) {
        return { backgroundColor: active ? '#ffffff40' : '#ffffff14' };
    }
    function RenderTargets(filters, items) {
        const bet = BetValue();
        Btn(filters, T('csgc_upgrade_any', 'Any'), () => { state.tier = 0; state.page = 0; Render(); }, Toggle(state.tier === 0));
        for (let t = 1; t <= 7; t++) {
            const tier = t;
            Btn(filters, '', () => { state.tier = state.tier === tier ? 0 : tier; state.page = 0; Render(); },
                { width: '30px', height: '30px', backgroundColor: COLORS[t] + (state.tier === t ? 'ff' : '50') });
        }
        [['all', T('csgc_upgrade_all', 'All')], ['guns', T('csgc_upgrade_guns', 'Guns')], ['knives', T('csgc_upgrade_knives', 'Knives')], ['gloves', T('csgc_upgrade_gloves', 'Gloves')]].forEach((kind) => {
            Btn(filters, kind[1], () => { state.kind = kind[0]; state.page = 0; Render(); }, Toggle(state.kind === kind[0]));
        });
        if (state.deposits.length) {
            Btn(filters, T('csgc_upgrade_same', 'Same weapon'), () => {
                state.same = !state.same;
                state.page = 0;
                if (state.same) {
                    // the deposit's weapon in every kind and rarity (knives + a gun's def is nothing)
                    state.kind = 'all';
                    state.tier = 0;
                }
                Render();
            }, Toggle(state.same));
        }
        Btn(filters, 'StatTrak™', () => { state.statTrak = !state.statTrak; state.page = 0; Render(); }, Toggle(state.statTrak));
        if (bet) {
            // how many times the bet the target costs at least: x2 starts the list at about 50%
            MULTIPLIERS.forEach((m) => {
                Btn(filters, 'x' + m, () => { state.multiplier = state.multiplier === m ? 0 : m; state.page = 0; Render(); }, Toggle(state.multiplier === m));
            });
        }
        const min = bet ? Math.max(bet + 1, state.multiplier ? Math.ceil(bet * state.multiplier) : 0) : 1;
        const firstDef = state.deposits.length ? Number(InventoryAPI.GetItemDefinitionIndex(state.deposits[0])) : -1;
        const kind = state.statTrak ? 1 : 0;
        for (let i = 0; i < COUNT; i++) {
            const base = i * STRIDE;
            const def = CATALOG[base];
            if ((state.tier && CATALOG[base + 2] !== state.tier) || (state.kind !== 'all' && Kind(def) !== state.kind) || (state.same && def !== firstDef))
                continue;
            for (let w = 0; w < 5; w++) {
                const price = CATALOG[base + 3 + kind * 5 + w];
                if (price && price >= min)
                    items.push({ index: i, wear: w, st: state.statTrak, price: price, tier: CATALOG[base + 2] });
            }
        }
        // cheapest first: from the multiplier's chance down
        items.sort((a, b) => (a.price - b.price) || (a.index - b.index) || (a.wear - b.wear));
        if (!items.length) {
            const text = bet ? T('csgc_upgrade_no_dearer', 'Nothing dearer than the bet with these filters: turn a filter or the multiplier off')
                : T('csgc_upgrade_no_targets', 'No targets with these filters: pick another rarity or turn a filter off');
            Text(filters, text + ' (' + state.kind + ', tier ' + (state.tier || 'any') + (state.same ? ', def ' + firstDef : '') +
                (state.statTrak ? ', StatTrak' : '') + (state.multiplier ? ', x' + state.multiplier : '') + ')',
                { fontSize: '15px', color: '#ffffff88', margin: '4px', width: '100%' });
        }
        return bet;
    }
)JS"
    R"JS(    function RenderRight() {
        ui.right.RemoveAndDeleteChildren();
        const filters = Box(ui.right, 'right-wrap', { width: '100%' });
        const items = [];
        const bet = state.picking ? 0 : RenderTargets(filters, items);
        if (state.picking)
            RenderPick(filters, items);
        const pages = Math.max(1, Math.ceil(items.length / PAGE_SIZE));
        if (state.page >= pages)
            state.page = pages - 1;
        const grid = Box(ui.right, 'right-wrap', { width: '100%', height: 'fill-parent-flow(1.0)', overflow: 'squish scroll' });
        items.slice(state.page * PAGE_SIZE, (state.page + 1) * PAGE_SIZE).forEach((item) => {
            if (state.picking) {
                Tile(grid, item.id, item.tier, item.name, item.pick, false, item.sub);
                return;
            }
            // a target: the skin in one wear, what it costs and the chance for the bet
            const target = { index: item.index, wear: item.wear, st: item.st };
            const selected = !!state.target && state.target.index === item.index && state.target.wear === item.wear && state.target.st === item.st;
            Tile(grid, Faux(item.index), item.tier, Name(item.index), () => {
                if (state.busy)
                    return;
                state.target = target;
                state.result = null;
                Render();
            }, selected, TargetLine(target, bet));
        });
        const nav = Box(ui.right, 'right', { horizontalAlign: 'center' });
        Btn(nav, '<', () => { if (state.page > 0) { state.page--; Render(); } });
        Text(nav, (state.page + 1) + ' / ' + pages + '   (' + items.length + ')', { margin: '8px 14px' });
        Btn(nav, '>', () => { if (state.page + 1 < pages) { state.page++; Render(); } });
    }
    function Go() {
        const chance = Chance();
        if (state.busy || chance <= 0)
            return;
        UiToolkitAPI.ShowGenericPopupOkCancel(T('csgc_upgrade_title', 'Upgrader'),
            T('csgc_upgrade_confirm', 'The skins you put in burn if it fails. Go?') + ' ' + T('csgc_upgrade_chance', 'Chance') + ': ' + Percent(chance) +
            (CURRENCY ? ' (' + Money(BetValue()) + ' → ' + Money(TargetPrice()) + ')' : ''), '', Send, () => { });
    }
    function Send() {
        // listed ones first: a burnt deposit is noticed by its absence from the inventory lists
        const listed = {};
        InventoryItems().forEach((id) => { listed[id] = true; });
        const deposits = state.deposits.slice().sort((a, b) => (listed[b] ? 1 : 0) - (listed[a] ? 1 : 0));
        const target = state.target;
        // catalog index * 16 + wear * 2 + StatTrak, plus 1 (0 is no target)
        const code = target.index * 16 + target.wear * 2 + (target.st ? 1 : 0) + 1;
        state.spinChance = Chance();
        const before = {};
        const count = InventoryAPI.GetUnacknowledgeItemsCount();
        for (let i = 0; i < count; i++)
            before[InventoryAPI.GetUnacknowledgeItemByIndex(i)] = true;
        state.busy = true;
        state.picking = false;
        state.result = null;
        state.note = '';
        SetWheel(state.spinChance);
        Render();
        const updatesBefore = state.updates;
        HoldReveal();
        deposits.forEach((id) => InventoryAPI.SetItemAttributeValueAsync(id, 'upgrade threshold', code));
        InventoryAPI.SetItemAttributeValueAsync(deposits[0], 'upgrade level', deposits.length);
        // the gc's answer comes as an inventory update: the deposits are gone (rolled) or still there
        // (refused, the gc sends the first one back unchanged). An update can come before the
        // answer, so a deposit still there counts as refused only after a second of updates
        let tries = 0;
        let sinceUpdate = 0;
        let sinceGone = 0;
        const poll = () => {
            tries++;
            KeepAlert();
            if (state.updates === updatesBefore) {
                if (tries < 100) {
                    $.Schedule(0.1, poll);
                    return;
                }
                state.busy = false;
                Reveal();
                state.note = T('csgc_upgrade_timeout', 'No answer from the GC, see cs2_gc\\gc_log.txt');
                Render();
                return;
            }
            const present = {};
            InventoryItems().forEach((id) => { present[id] = true; });
            if (present[deposits[0]]) {
                if (++sinceUpdate < 10) {
                    $.Schedule(0.1, poll);
                    return;
                }
                state.busy = false;
                Reveal();
                state.deposits = state.deposits.filter((id) => present[id] && CanDeposit(id));
                state.note = T('csgc_upgrade_refused', 'The GC refused, see cs2_gc\\gc_log.txt');
                Render();
                return;
            }
            let won = '';
            const now = InventoryAPI.GetUnacknowledgeItemsCount();
            for (let i = 0; i < now; i++) {
                const id = InventoryAPI.GetUnacknowledgeItemByIndex(i);
                if (!before[id])
                    won = id;
            }
            // the won skin comes in its own message right after the burnt ones: a moment more
            // before it counts as lost
            if (!won && ++sinceGone < 4) {
                $.Schedule(0.1, poll);
                return;
            }
            state.deposits = [];
            Spin(!!won, () => {
                state.busy = false;
                state.result = { won: won };
                Render();
                $.Schedule(REVEAL_DELAY, Reveal);
            });
        };
        $.Schedule(0.1, poll);
    }
    function Show() {
        if (!ui)
            Build(panel);
        Render();
    }
    function Install(contextPanel, inventoryMain, navigate) {
        const tabs = contextPanel.FindChildInLayoutFile('id-navbar-tabs-catagory-btns-container');
        const content = inventoryMain.FindChildInLayoutFile('InventoryMenuContent');
        if (!tabs || !content) {
            $.Msg('[csgc upgrader] no inventory tabs to join\n');
            return;
        }
        if (content.FindChildInLayoutFile('csgc_upgrader'))
            return;
        const button = $.CreatePanel('RadioButton', tabs, 'csgc_upgrader_tab', { class: 'content-navbar__tabs__btn', group: 'inv-top-nav' });
        $.CreatePanel('Label', button, '', { text: '#csgc_upgrade_title' });
        panel = $.CreatePanel('Panel', content, 'csgc_upgrader', { class: 'inv-category' });
        // like the game's own tabs: once faded out it stops being visible, or it would take the clicks
        $.RegisterEventHandler('PropertyTransitionEnd', panel, (panelName, propertyName) => {
            if (propertyName === 'opacity' && panel.visible === true && panel.BIsTransparent()) {
                panel.visible = false;
                return true;
            }
            return false;
        });
        const open = () => {
            navigate('csgc_upgrader');
            try {
                Show();
            }
            catch (e) {
                $.Msg('[csgc upgrader] ' + e + '\n' + e.stack + '\n');
            }
        };
        button.SetPanelEvent('onactivate', open);
        inventoryMain.Data().csgcUpgrade = (id) => {
            if (!state.busy && state.deposits.indexOf(id) < 0 && state.deposits.length < MAX_DEPOSITS) {
                const reason = Reject(id);
                if (!reason) {
                    state.deposits.push(id);
                    state.result = null;
                    state.picking = false;
                    state.note = '';
                }
                else {
                    state.note = reason === 'price' ? T('csgc_upgrade_no_price', 'This skin has no market price in its wear, it can\'t go in')
                        : T('csgc_upgrade_cant', 'This item can\'t go in the upgrader');
                }
            }
            button.checked = true;
            open();
        };
        $.RegisterForUnhandledEvent('PanoramaComponent_MyPersona_InventoryUpdated', () => {
            state.updates++;
            prices = {};
            if (state.hold) {
                // the main menu sets the count on this same update, before or after this
                KeepAlert();
                $.Schedule(0.0, KeepAlert);
            }
            if (!ui || state.busy || !panel.BHasClass('Active'))
                return;
            if (state.deposits.length) {
                const present = {};
                InventoryItems().forEach((id) => { present[id] = true; });
                state.deposits = state.deposits.filter((id) => present[id]);
            }
            Render();
        });
    }
    return { Install: Install };
})();
)JS";

// where it goes into mainmenu_inventory: the module at the top, the tab after the game's own
static constexpr std::string_view UpgraderInstall = R"JS(try {
            CsgcUpgrader.Install($.GetContextPanel(), _m_elInventoryMain, NavigateToTab);
        }
        catch (e) {
            $.Msg('[csgc upgrader] ' + e + '\n' + e.stack + '\n');
        }
        )JS";

// built-in localization: the menu entry and the window (english for every language but russian)
struct BuiltInToken
{
    std::string_view name;
    std::string_view english;
    std::string_view russian;
};

static constexpr BuiltInToken BuiltInTokens[] = {
    { "inv_context_csgc_upgrade", "Upgrade", "Апгрейд" },
    { "csgc_upgrade_title", "Upgrader", "Апгрейдер" },
    { "csgc_upgrade_close", "Close", "Закрыть" },
    { "csgc_upgrade_rule", "1:10 per rarity step, the chances of several skins add up. The skins burn if it fails.",
        "1:10 за каждую ступень редкости, шансы нескольких скинов складываются. При неудаче скины сгорают." },
    { "csgc_upgrade_rule_prices", "Chance = what the bet is worth / the target's price, the target has to cost more. The skins burn if it fails. Prices:",
        "Шанс = стоимость ставки / цена цели, цель должна быть дороже. При неудаче скины сгорают. Цены:" },
    { "csgc_upgrade_rub", "RUB", "руб." },
    { "csgc_upgrade_deposit", "Your skins", "Ставка" },
    { "csgc_upgrade_add", "Add a skin", "Добавить скин" },
    { "csgc_upgrade_back", "Back to the targets", "Назад к целям" },
    { "csgc_upgrade_target", "Target", "Цель" },
    { "csgc_upgrade_pick_target", "Pick the skin you want on the right", "Выбери справа скин, который хочешь получить" },
    { "csgc_upgrade_chance", "Chance", "Шанс" },
    { "csgc_upgrade_go", "Upgrade", "Апгрейд" },
    { "csgc_upgrade_wait", "Rolling...", "Крутим..." },
    { "csgc_upgrade_confirm", "The skins you put in burn if it fails. Go?", "Если не повезёт, поставленные скины сгорят. Крутим?" },
    { "csgc_upgrade_won", "You got it!", "Повезло!" },
    { "csgc_upgrade_lost", "No luck, the skins are gone", "Не повезло, скины сгорели" },
    { "csgc_upgrade_timeout", "No answer from the GC, see cs2_gc\\\\gc_log.txt", "GC не ответил, смотри cs2_gc\\\\gc_log.txt" },
    { "csgc_upgrade_your_items", "Your skins", "Твои скины" },
    { "csgc_upgrade_any", "Any", "Любая" },
    { "csgc_upgrade_all", "All", "Все" },
    { "csgc_upgrade_guns", "Guns", "Оружие" },
    { "csgc_upgrade_knives", "Knives", "Ножи" },
    { "csgc_upgrade_gloves", "Gloves", "Перчатки" },
    { "csgc_upgrade_same", "Same weapon", "То же оружие" },
    { "csgc_upgrade_too_cheap", "The target has to cost more than the bet", "Цель должна быть дороже ставки" },
    { "csgc_upgrade_no_targets", "No targets with these filters: pick another rarity or turn a filter off",
        "Нет целей с такими фильтрами: выбери другую редкость или сними фильтр" },
    { "csgc_upgrade_no_dearer", "Nothing dearer than the bet with these filters: turn a filter or the multiplier off",
        "С такими фильтрами нет ничего дороже ставки: сними фильтр или множитель" },
    { "csgc_upgrade_no_guns", "No skins with a market price to put in", "Нет скинов с ценой на маркете для ставки" },
    { "csgc_upgrade_no_price", "This skin has no market price in its wear, it can't go in",
        "У этого скина нет цены на маркете в таком износе, его нельзя поставить" },
    { "csgc_upgrade_cant", "This item can't go in the upgrader", "Этот предмет нельзя поставить в апгрейдер" },
    { "csgc_upgrade_guns_owned", "skins in the inventory", "скинов в инвентаре" },
    { "csgc_upgrade_skip_stock", "stock", "стандартных" },
    { "csgc_upgrade_skip_price", "without a market price", "без цены на маркете" },
    { "csgc_upgrade_skip_deposit", "in the bet", "уже в ставке" },
    { "csgc_upgrade_skip_other", "other", "прочих" },
    { "csgc_upgrade_refused", "The GC refused, see cs2_gc\\\\gc_log.txt", "GC отказал, смотри cs2_gc\\\\gc_log.txt" },
};

static void ApplyBuiltInTokens(const std::string &gameVpk, std::vector<VpkFile> &out)
{
    for (const std::string &language : GetConfig().m_modLanguages)
    {
        std::string path = std::format("resource/csgo_{}.txt", language);
        std::string text;
        auto it = std::find_if(out.begin(), out.end(), [&](const VpkFile &file) { return file.path == path; });
        if (it != out.end())
        {
            text = std::move(it->data);
            out.erase(it);
        }
        else
        {
            std::vector<char> data;
            if (!VpkReadFile(gameVpk, path, data))
            {
                continue;
            }
            text.assign(data.begin(), data.end());
        }

        std::string custom = "\"lang\"\n{\n\t\"Tokens\"\n\t{\n";
        for (const BuiltInToken &token : BuiltInTokens)
        {
            custom += std::format("\t\t\"{}\"\t\"{}\"\n", token.name, language == "russian" ? token.russian : token.english);
        }
        custom += "\t}\n}\n";

        MergeReport report;
        std::string error;
        std::string merged = text;
        if (MergeSections(merged, custom, MergeMode::OverrideExisting, report, error))
        {
            text = std::move(merged);
        }
        else
        {
            Platform::Print("CSForge: built-in texts not merged into {}: {}\n", path, error);
        }
        out.push_back({ std::move(path), std::move(text) });
    }
}

size_t ApplyPanoramaFixes(const std::string &gameVpk, std::vector<VpkFile> &out)
{
    // a fix is one or more pieces of code for one file, each before its own anchor
    struct Fix
    {
        std::string_view path;
        std::vector<std::pair<std::string_view, std::string>> edits; // anchor, code
        std::string what;
    };
    std::vector<Fix> fixes;
    for (const PanoramaFix &fix : StaticPanoramaFixes)
    {
        fixes.push_back({ fix.path, { { fix.anchor, std::string{ fix.code } } }, std::string{ fix.what } });
    }

    if (!s_upgraderCatalog.empty())
    {
        // def, paint kit, tier, then the prices: 5 plain, 5 StatTrak, 5 souvenir (the window's STRIDE)
        std::string catalog;
        catalog.reserve(s_upgraderCatalog.size() * 80);
        for (const UpgradeTarget &target : s_upgraderCatalog)
        {
            catalog += std::format("{},{},{},", target.defIndex, target.paintKit, target.tier);
            for (const auto *prices : { &target.price, &target.statTrak, &target.souvenir })
            {
                for (uint64_t price : *prices)
                {
                    catalog += std::format("{},", price);
                }
            }
        }
        std::string module{ UpgraderModule };
        module.replace(module.find("@CATALOG@"), 9, catalog);
        module.replace(module.find("@CURRENCY@"), 10, s_upgraderCurrency);
        module.replace(module.find("@PRICE_TIME@"), 12, std::to_string(s_upgraderPriceTime));

        fixes.push_back({ "panorama/scripts/common/item_context_entries.vts_c",
            { { "{\n            name: 'tradeup_add',", std::string{ UpgraderMenuEntry } } },
            "Upgrade in the context menu of a skin" });
        fixes.push_back({ "panorama/scripts/mainmenu_inventory.vts_c",
            {
                { "var InventoryPanel;\n(function (InventoryPanel) {", std::move(module) },
                { "_InitMarketLink();\n        _InitXrayBtn();", std::string{ UpgraderInstall } },
            },
            std::format("the Upgrader tab in the inventory ({} skins, {})", s_upgraderCatalog.size(),
                s_upgraderCurrency.empty() ? std::string{ "chances by rarity" } : s_upgraderCurrency + " prices") });
    }

    size_t applied = 0;
    for (const Fix &fix : fixes)
    {
        if (std::any_of(out.begin(), out.end(), [&](const VpkFile &file) { return file.path == fix.path; }))
        {
            Platform::Print("CSForge: a mod ships {}, fix not applied ({})\n", fix.path, fix.what);
            continue;
        }

        std::vector<char> data;
        if (!VpkReadFile(gameVpk, fix.path, data))
        {
            Platform::Print("CSForge: {} not found in the game files, fix not applied\n", fix.path);
            continue;
        }

        std::string file{ data.begin(), data.end() };
        std::string error;
        bool patched = true;
        for (const auto &[anchor, code] : fix.edits)
        {
            if (!PatchResourceText(file, anchor, code, error))
            {
                patched = false;
                break;
            }
        }
        if (!patched)
        {
            Platform::Print("CSForge: {}: {}, fix not applied ({})\n", fix.path, error, fix.what);
            continue;
        }

        out.push_back({ std::string{ fix.path }, std::move(file) });
        Platform::Print("CSForge: fix: {}\n", fix.what);
        applied++;
    }
    return applied;
}

bool Build()
{
    s_builtVpk.clear();

    std::string buildDir = Platform::DataPath("mods") + "\\" + BuildFolder;
    std::error_code ec;
    std::filesystem::remove_all(Utf8Path(buildDir), ec);

    std::vector<ModInfo> mods = ListMods();
    std::vector<const ModInfo *> enabled;
    s_loadedState.clear();
    for (const ModInfo &mod : mods)
    {
        s_loadedState[mod.folder] = { mod.enabled, mod.priority };
        Platform::Print("CSForge: {} '{}' {} priority {}{}\n", mod.folder, mod.name, mod.version,
            mod.priority, mod.enabled ? "" : " (disabled)");
        if (mod.enabled)
        {
            enabled.push_back(&mod);
        }
    }

    // files: later mods (higher priority) replace earlier ones
    struct Source
    {
        std::filesystem::path path;
        const ModInfo *mod;
    };
    std::map<std::string, Source> files;

    for (const ModInfo *mod : enabled)
    {
        std::vector<std::pair<std::string, std::filesystem::path>> modFiles;
        CollectModFiles(Utf8Path(ModPath(*mod, "files")), modFiles);
        for (auto &[path, source] : modFiles)
        {
            auto [it, inserted] = files.try_emplace(path, Source{ source, mod });
            if (!inserted)
            {
                Platform::Print("CSForge: {} replaces {}'s {}\n", mod->folder, it->second.mod->folder, path);
                it->second = Source{ source, mod };
            }
        }
    }

    std::vector<VpkFile> out;
    for (const auto &[path, source] : files)
    {
        VpkFile file{ path, {} };
        if (ReadWholeFile(PathUtf8(source.path), file.data))
        {
            out.push_back(std::move(file));
        }
    }

    // a mod can ship a whole file in files\ too, merge on top of that then
    auto baseText = [&](std::string_view gameFile, std::string &text) {
        for (VpkFile &file : out)
        {
            if (file.path == gameFile)
            {
                text = std::move(file.data);
                std::erase_if(out, [&](const VpkFile &f) { return f.path == gameFile; });
                return true;
            }
        }

        std::vector<char> data;
        if (!VpkReadFile(Platform::GamePath("pak01_dir.vpk"), gameFile, data))
        {
            return false;
        }
        text.assign(data.begin(), data.end());
        return true;
    };

    auto mergeInto = [&](std::string_view gameFile, MergeMode mode, auto &&modFileFor) {
        std::string text;
        bool loaded = false;
        bool changed = false;

        for (const ModInfo *mod : enabled)
        {
            std::string modFile = modFileFor(*mod);
            std::string custom;
            if (modFile.empty() || !ReadWholeFile(ModPath(*mod, modFile), custom))
            {
                continue;
            }

            if (!loaded)
            {
                if (!baseText(gameFile, text))
                {
                    Platform::Print("CSForge: {} not found in the game files\n", gameFile);
                    return;
                }
                loaded = true;
            }

            MergeReport report;
            std::string error;
            std::string merged = text;
            if (!MergeSections(merged, custom, mode, report, error))
            {
                Platform::Print("CSForge: {}\\{} not merged into {}: {}\n", mod->folder, modFile, gameFile, error);
                continue;
            }

            text = std::move(merged);
            changed = true;
            Platform::Print("CSForge: {}\\{} -> {}: {} added, {} overridden, {} skipped\n", mod->folder, modFile, gameFile,
                report.added, report.overridden.size(), report.skipped.size());
            for (const std::string &skipped : report.skipped)
            {
                Platform::Print("CSForge:   skipped {} (already defined)\n", skipped);
            }
        }

        if (changed)
        {
            out.push_back({ std::string{ gameFile }, std::move(text) });
        }
    };

    mergeInto("scripts/items/items_game.txt", MergeMode::SkipExisting, [](const ModInfo &) { return std::string{ "items_game.txt" }; });

    for (const std::string &language : GetConfig().m_modLanguages)
    {
        // localization.txt for every language, then localization_<lang>.txt on top
        mergeInto(std::format("resource/csgo_{}.txt", language), MergeMode::OverrideExisting,
            [](const ModInfo &) { return std::string{ "localization.txt" }; });
        mergeInto(std::format("resource/csgo_{}.txt", language), MergeMode::OverrideExisting,
            [&](const ModInfo &) { return std::format("localization_{}.txt", language); });
    }

    if (GetConfig().m_panoramaFixes)
    {
        std::string gameVpk = Platform::GamePath("pak01_dir.vpk");
        Platform::Print("CSForge: upgrader: {} targets\n", BuildUpgraderCatalog());
        ApplyBuiltInTokens(gameVpk, out);
        ApplyPanoramaFixes(gameVpk, out);
    }

    if (out.empty())
    {
        Platform::Print("CSForge: nothing to load ({} mods, {} enabled)\n", mods.size(), enabled.size());
        return false;
    }

    std::filesystem::create_directories(Utf8Path(buildDir), ec);
    std::string vpkPath = buildDir + "\\" + VpkName + "_dir.vpk";
    if (!VpkWrite(vpkPath, out))
    {
        Platform::Print("CSForge: can't write {}\n", vpkPath);
        return false;
    }

    size_t total = 0;
    for (const VpkFile &file : out)
    {
        total += file.data.size();
    }

    Platform::Print("CSForge: built {} ({} files, {} bytes from {} mods)\n", vpkPath, out.size(), total, enabled.size());
    s_builtVpk = vpkPath;
    return true;
}

// ------------------------------------------------------------------ inventory editor's Mods tab

bool LoadedThisRun()
{
    return !s_builtVpk.empty();
}

std::vector<ModStatus> ModStatuses()
{
    constexpr size_t MaxListedLost = 20;

    std::vector<ModInfo> mods = ListMods();
    std::vector<ModStatus> statuses;
    std::vector<std::vector<std::string>> modFiles;
    statuses.reserve(mods.size());

    for (ModInfo &mod : mods)
    {
        ModStatus status;
        std::vector<std::pair<std::string, std::filesystem::path>> files;
        CollectModFiles(Utf8Path(ModPath(mod, "files")), files);
        status.files = files.size();
        status.items = FileExists(ModPath(mod, "items_game.txt"));

        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator{ Utf8Path(ModPath(mod, "")), ec }; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            std::string name = Lower(PathUtf8(it->path().filename()));
            if (it->is_regular_file(ec) && name.starts_with("localization") && name.ends_with(".txt"))
            {
                status.localization = true;
            }
        }

        auto loaded = s_loadedState.find(mod.folder);
        status.changed = loaded == s_loadedState.end() || loaded->second.first != mod.enabled
            || (mod.enabled && loaded->second.second != mod.priority);

        std::vector<std::string> paths;
        for (auto &[path, source] : files)
        {
            paths.push_back(path);
        }
        modFiles.push_back(std::move(paths));
        status.info = std::move(mod);
        statuses.push_back(std::move(status));
    }

    // the same file from two enabled mods: the later one in load order (higher priority) wins
    std::map<std::string, size_t> winner;
    for (size_t i = 0; i < statuses.size(); i++)
    {
        if (statuses[i].info.enabled)
        {
            for (const std::string &path : modFiles[i])
            {
                winner[path] = i;
            }
        }
    }

    for (size_t i = 0; i < statuses.size(); i++)
    {
        if (!statuses[i].info.enabled)
        {
            continue;
        }

        for (const std::string &path : modFiles[i])
        {
            size_t won = winner[path];
            if (won != i)
            {
                statuses[i].lostFiles++;
                if (statuses[i].lost.size() < MaxListedLost)
                {
                    statuses[i].lost.emplace_back(path, statuses[won].info.folder);
                }
            }
        }
    }

    return statuses;
}

// "key" "value" in a flat KeyValues text: replaces the first value of key, or adds the pair before
// the last closing brace. Text level, so comments and the other keys stay as they are.
static bool SetKeyValueText(std::string &text, std::string_view key, std::string_view value)
{
    std::string lower = Lower(text);
    std::string quotedKey = "\"" + Lower(key) + "\"";

    for (size_t at = lower.find(quotedKey); at != std::string::npos; at = lower.find(quotedKey, at + 1))
    {
        size_t open = at + quotedKey.size();
        while (open < text.size() && (text[open] == ' ' || text[open] == '\t'))
        {
            open++;
        }

        if (open == at + quotedKey.size() || open >= text.size() || text[open] != '"')
        {
            continue; // not a key followed by a quoted value
        }

        size_t close = text.find('"', open + 1);
        if (close == std::string::npos || text.find('\n', open) < close)
        {
            continue;
        }

        text.replace(open + 1, close - open - 1, value);
        return true;
    }

    size_t brace = text.rfind('}');
    if (brace == std::string::npos)
    {
        return false;
    }

    text.insert(brace, "\t\"" + std::string{ key } + "\"\t\"" + std::string{ value } + "\"\n");
    return true;
}

bool SetModState(std::string_view folder, bool enabled, int priority, std::string &error)
{
    // only folders ListMods knows: the name comes from the editor's websocket
    std::vector<ModInfo> mods = ListMods();
    auto mod = std::find_if(mods.begin(), mods.end(), [&](const ModInfo &m) { return m.folder == folder; });
    if (mod == mods.end())
    {
        error = "no mod folder named " + std::string{ folder };
        return false;
    }

    std::string path = ModPath(*mod, "mod.txt");
    std::string text;
    if (!FileExists(path) || !ReadWholeFile(path, text) || text.find('{') == std::string::npos)
    {
        text = "\"mod\"\n{\n\t\"name\"\t\"" + mod->folder + "\"\n}\n";
    }

    if (!SetKeyValueText(text, "enabled", enabled ? "1" : "0") || !SetKeyValueText(text, "priority", std::to_string(priority)))
    {
        error = "mod.txt has no \"mod\" { } block";
        return false;
    }

    FILE *f = Platform::OpenFile(path, "wb");
    if (!f)
    {
        error = "can't write " + path;
        return false;
    }

    bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!written)
    {
        error = "can't write " + path;
        return false;
    }

    Platform::Print("CSForge: {} set to {} priority {} (from the inventory editor, next game start)\n", mod->folder,
        enabled ? "enabled" : "disabled", priority);
    return true;
}

// ------------------------------------------------------------------ filesystem hook

#ifdef _WIN32

// IFileSystem (VFileSystem017) slot 31, from engine2's gameinfo SearchPaths code:
// AddSearchPath(path, pathID, addType, priority, unknown), addType 0 = head, 1 = tail
constexpr int AddSearchPathSlot = 31;
constexpr int PathAddToHead = 0;

using AddSearchPath_t = void (*)(void *fileSystem, const char *path, const char *pathID, int addType, int priority, int unknown);

static AddSearchPath_t s_addSearchPath;
static std::string s_mountPath; // <data>\mods\_build\csforge.vpk, AddSearchPath opens <name>_dir.vpk
static bool s_fileSystemHooked;
static void *s_fileSystem;
static int s_mounts;

static void MountMods(void *fileSystem, bool game, bool mod, std::string_view reason)
{
    for (const char *pathID : { game ? "GAME" : nullptr, mod ? "MOD" : nullptr })
    {
        if (pathID)
        {
            s_addSearchPath(fileSystem, s_mountPath.c_str(), pathID, PathAddToHead, 0, 0);
            Platform::Print("CSForge: mounted {} ({}, {})\n", s_mountPath, pathID, reason);
        }
    }

    s_mounts++;
}

static bool IsGameFolder(const char *path)
{
    // <cs2>\game\csgo (absolute by the time it gets here), not csgo_lv / csgo_addons
    std::string_view view = path ? path : "";
    while (!view.empty() && (view.back() == '\\' || view.back() == '/'))
    {
        view.remove_suffix(1);
    }

    size_t slash = view.find_last_of("\\/");
    std::string_view last = (slash == std::string_view::npos) ? view : view.substr(slash + 1);
    return last.size() == 4 && _strnicmp(last.data(), "csgo", 4) == 0;
}

static void Hk_AddSearchPath(void *fileSystem, const char *path, const char *pathID, int addType, int priority, int unknown)
{
    s_addSearchPath(fileSystem, path, pathID, addType, priority, unknown);

    if (!pathID || !IsGameFolder(path))
    {
        return;
    }

    // ours go in front of csgo every time the engine (re)adds it
    bool game = !_stricmp(pathID, "GAME");
    bool mod = !_stricmp(pathID, "MOD");
    if (game || mod)
    {
        MountMods(fileSystem, game, mod, "engine added csgo");
    }
}

static bool IsClientDll(LPCWSTR name)
{
    std::wstring_view view = name ? name : L"";
    size_t slash = view.find_last_of(L"\\/");
    std::wstring_view file = (slash == std::wstring_view::npos) ? view : view.substr(slash + 1);
    return _wcsicmp(std::wstring{ file }.c_str(), L"client.dll") == 0;
}

static bool HookFileSystem()
{
    HMODULE module = GetModuleHandleW(L"filesystem_stdio.dll");
    if (!module)
    {
        return false;
    }

    // the interface list gets filled by the dll's static constructors: while filesystem_stdio is
    // still initializing (a load nested in its own) this is null, try again on the next load
    using CreateInterface_t = void *(*)(const char *, int *);
    auto createInterface = reinterpret_cast<CreateInterface_t>(GetProcAddress(module, "CreateInterface"));
    void *fileSystem = createInterface ? createInterface("VFileSystem017", nullptr) : nullptr;
    if (!fileSystem)
    {
        return false;
    }

    void **vtable = *static_cast<void ***>(fileSystem);
    void **slot = &vtable[AddSearchPathSlot];

    s_fileSystem = fileSystem;
    if (*slot == reinterpret_cast<void *>(&Hk_AddSearchPath))
    {
        return true; // already done
    }

    DWORD oldProtect;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect))
    {
        Platform::Print("CSForge: can't patch the filesystem vtable\n");
        return true;
    }

    s_addSearchPath = reinterpret_cast<AddSearchPath_t>(*slot);
    *slot = reinterpret_cast<void *>(&Hk_AddSearchPath);
    VirtualProtect(slot, sizeof(void *), oldProtect, &oldProtect);

    Platform::Print("CSForge: hooked VFileSystem017::AddSearchPath\n");
    return true;
}

using LoadLibraryExW_t = HMODULE(WINAPI *)(LPCWSTR, HANDLE, DWORD);
static LoadLibraryExW_t Og_LoadLibraryExW;

static HMODULE WINAPI Hk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE result = Og_LoadLibraryExW(name, file, flags);
    if (!result || (s_fileSystemHooked && s_mounts))
    {
        return result;
    }

    static std::mutex mutex;
    std::lock_guard lock{ mutex };

    // the engine loads filesystem_stdio through tier0's Plat_LoadModule, hook it as soon as
    // it's there, before the engine reads gameinfo.gi and adds the search paths
    if (!s_fileSystemHooked)
    {
        s_fileSystemHooked = HookFileSystem();
    }

    // hooked too late to see the engine add game\csgo: the search paths are set up by the
    // time client.dll gets loaded and the client reads items_game.txt only later, in its
    // init, so put ours in front now
    if (s_fileSystemHooked && !s_mounts && IsClientDll(name))
    {
        MountMods(s_fileSystem, true, true, "client.dll loaded");
    }

    return result;
}

// kernelbase has the real function, kernel32's export jumps to it
static void HookKernelBase(const char *name, void *hook, void **original)
{
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    void *target = kernelBase ? reinterpret_cast<void *>(GetProcAddress(kernelBase, name)) : nullptr;
    if (!target)
    {
        target = reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name));
    }

    funchook_t *funchook = target ? funchook_create() : nullptr;
    if (!funchook)
    {
        Platform::Print("CSForge: can't hook {}\n", name);
        return;
    }

    void *temp = target;
    if (funchook_prepare(funchook, &temp, hook) != 0)
    {
        Platform::Print("CSForge: can't hook {}: {}\n", name, funchook_error_message(funchook));
        return;
    }

    *original = temp;
    if (funchook_install(funchook, 0) != 0)
    {
        Platform::Print("CSForge: can't hook {}: {}\n", name, funchook_error_message(funchook));
    }
}

#endif // _WIN32

void Install()
{
    if (!GetConfig().m_mods)
    {
        return;
    }

    if (!Build())
    {
        return;
    }

#ifdef _WIN32
    s_mountPath = Platform::DataPath("mods") + "\\" + BuildFolder + "\\" + VpkName + ".vpk";

    // filesystem_stdio isn't loaded yet when launched normally, the LoadLibraryExW hook is also
    // what notices client.dll, so it's needed either way
    s_fileSystemHooked = HookFileSystem();
    HookKernelBase("LoadLibraryExW", reinterpret_cast<void *>(&Hk_LoadLibraryExW), reinterpret_cast<void **>(&Og_LoadLibraryExW));
#endif
}

} // namespace ModLoader

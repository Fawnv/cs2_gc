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
#else
#include <strings.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
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

// mod folder ("" = CSForge itself) -> what Build had to say, for the editor's Mods tab
static std::map<std::string, std::vector<ModProblem>> s_problems;

static void Problem(std::string_view folder, bool error, std::string text)
{
    Platform::Print("CSForge: {}{}{}\n", folder, folder.empty() ? "" : ": ", text);

    // once: a token that can't be replaced comes up again for every language
    std::vector<ModProblem> &problems = s_problems[std::string{ folder }];
    if (std::none_of(problems.begin(), problems.end(), [&](const ModProblem &problem) { return problem.text == text; }))
    {
        problems.push_back({ error, std::move(text) });
    }
}

std::vector<ModProblem> GeneralProblems()
{
    auto it = s_problems.find({});
    return it != s_problems.end() ? it->second : std::vector<ModProblem>{};
}

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

// DeepOverride: the custom block's children into the base blocks of the same path (a block can be
// there more than once, items_game.txt has 144 "client_loot_lists"). Edits are collected as
// replacements and as inserts at a block's end (one text per position, in order)
static bool DeepMergeBlock(std::string_view baseText, const std::vector<Section> &baseBlocks, std::string_view customText,
    const Section &customBlock, const std::string &path, std::vector<std::tuple<size_t, size_t, std::string>> &replace,
    std::map<size_t, std::string> &insert, MergeReport &report, std::string &error)
{
    std::vector<Child> customChildren;
    if (!ParseChildren(customText, customBlock, customChildren))
    {
        error = std::format("mod: can't parse '{}'", path);
        return false;
    }

    std::vector<std::vector<Child>> baseChildren(baseBlocks.size());
    for (size_t i = 0; i < baseBlocks.size(); i++)
    {
        if (!ParseChildren(baseText, baseBlocks[i], baseChildren[i]))
        {
            error = std::format("base: can't parse '{}'", path);
            return false;
        }
    }

    for (const Child &custom : customChildren)
    {
        std::string id = path + "/" + custom.key;
        std::vector<Section> sameBlocks; // the base's blocks of this key, to merge into
        const Child *sameText = nullptr;
        bool mismatch = false;
        for (const std::vector<Child> &children : baseChildren)
        {
            for (const Child &base : children)
            {
                if (base.key != custom.key)
                {
                    continue;
                }

                if (base.block != custom.block)
                {
                    mismatch = true;
                }
                else if (base.block)
                {
                    sameBlocks.push_back({ base.key, base.valueStart + 1, base.end - 1 });
                }
                else if (!sameText)
                {
                    sameText = &base;
                }
            }
        }

        if (custom.block && !sameBlocks.empty())
        {
            Section customBody{ custom.key, custom.valueStart + 1, custom.end - 1 };
            if (!DeepMergeBlock(baseText, sameBlocks, customText, customBody, id, replace, insert, report, error))
            {
                return false;
            }
        }
        else if (!custom.block && sameText)
        {
            replace.emplace_back(sameText->valueStart, sameText->end - sameText->valueStart,
                std::string{ customText.substr(custom.valueStart, custom.end - custom.valueStart) });
            report.overridden.push_back(id);
        }
        else if (mismatch)
        {
            report.skipped.push_back(id);
        }
        else
        {
            insert[baseBlocks.front().bodyEnd] += std::format("\n\t\t{}", customText.substr(custom.start, custom.end - custom.start));
            report.added++;
            report.addedKeys.push_back(id);
        }
    }

    return true;
}

static bool DeepMerge(std::string &baseText, std::string_view customText, MergeReport &report, std::string &error)
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

    std::vector<std::tuple<size_t, size_t, std::string>> replace;
    std::map<size_t, std::string> insert;
    for (const Section &custom : customSections)
    {
        std::vector<Section> same;
        std::copy_if(baseSections.begin(), baseSections.end(), std::back_inserter(same), [&](const Section &s) { return s.name == custom.name; });
        if (same.empty())
        {
            insert[baseClose] += std::format("\t\"{}\"\n\t{{{}}}\n", custom.name, customText.substr(custom.bodyStart, custom.bodyEnd - custom.bodyStart));
            report.added++;
            report.addedKeys.push_back(custom.name);
            continue;
        }

        if (!DeepMergeBlock(baseText, same, customText, custom, custom.name, replace, insert, report, error))
        {
            return false;
        }
    }

    // back to front; at one position the insert goes after a replacement that ends there
    std::vector<std::tuple<size_t, size_t, std::string>> edits = std::move(replace);
    for (auto &[position, text] : insert)
    {
        edits.emplace_back(position, 0, text + "\n\t");
    }
    std::stable_sort(edits.begin(), edits.end(), [](const auto &a, const auto &b) { return std::get<0>(a) > std::get<0>(b); });
    for (const auto &[position, length, text] : edits)
    {
        baseText.replace(position, length, text);
    }

    std::vector<Section> check;
    size_t checkClose = 0;
    if (!ParseRoot(baseText, check, checkClose, error))
    {
        error = "merged: " + error;
        return false;
    }
    return true;
}

bool MergeSections(std::string &baseText, std::string_view customText, MergeMode mode, MergeReport &report, std::string &error)
{
    if (mode == MergeMode::DeepOverride)
    {
        return DeepMerge(baseText, customText, report, error);
    }

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
            report.addedKeys.push_back(id);
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
#ifndef _WIN32
    std::string s{ path };
    for (char &c : s)
    {
        if (c == '\\') c = '/';
    }
    return std::filesystem::path{ std::u8string_view{ reinterpret_cast<const char8_t *>(s.data()), s.size() } };
#else
    return std::filesystem::path{ std::u8string_view{ reinterpret_cast<const char8_t *>(path.data()), path.size() } };
#endif
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
                mod.infoBroken = true;
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
#ifdef _WIN32
    return Platform::DataPath("mods") + "\\" + mod.folder + "\\" + std::string{ file };
#else
    return Platform::DataPath("mods") + "/" + mod.folder + "/" + std::string{ file };
#endif
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
static bool s_upgraderBuilt; // this Build made the catalog already
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


// The upgrader's window is the mod mods\upgrader (it ships with cs2_gc): its JavaScript goes into the
// game's inventory scripts through its panorama.txt, the catalog comes in as @csforge:upgrader_catalog@.
// The window sends the choice as SetItemAttributeValueAsync (k_EMsgGCModifyItemAttribute, any
// stored_as_integer attribute passes): "upgrade threshold" = target index + 1 on every deposit, then
// "upgrade level" = deposit count. That, the catalog's layout (def, paint kit, tier, 5 plain, 5
// StatTrak, 5 souvenir prices) and the index * 16 + wear * 2 + StatTrak target number are version 1
// of what the mod "needs" ("upgrader 1"): change any of it and the number goes up.
constexpr int UpgraderVersion = 1;

// the catalog as the window reads it: def, paint kit, tier, then 15 prices, per skin
static std::string UpgraderCatalogText()
{
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
    return catalog;
}


// where it goes into mainmenu_inventory: the module at the top, the tab after the game's own


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

    size_t applied = 0;
    for (const Fix &fix : fixes)
    {
        if (std::any_of(out.begin(), out.end(), [&](const VpkFile &file) { return file.path == fix.path; }))
        {
            Problem({}, false, std::format("built-in fix not applied, a mod ships its own {}: {}", fix.path, fix.what));
            continue;
        }

        std::vector<char> data;
        if (!VpkReadFile(gameVpk, fix.path, data))
        {
            Problem({}, false, std::format("built-in fix not applied, {} isn't in the game files: {}", fix.path, fix.what));
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
            Problem({}, false, std::format("built-in fix not applied, the game's {} changed ({}): {}", fix.path, error, fix.what));
            continue;
        }

        out.push_back({ std::string{ fix.path }, std::move(file) });
        Platform::Print("CSForge: fix: {}\n", fix.what);
        applied++;
    }
    return applied;
}

// a compiled Source 2 file (*_c) whose header and block table make sense: uint32 size, uint16 header
// version 12, uint16 type version, uint32 block table offset 8, uint32 block count, then per block
// char[4] type, uint32 offset (from where it's stored), uint32 size. The size can be less than the
// file (textures and sounds keep their data after the blocks). True for every *_c of the game
// (checked on 2500 of pak01): a file that fails is no such file or a damaged one, and the client may
// crash on it
static bool ValidResource(std::string_view data)
{
    auto read32 = [&](size_t at) {
        uint32_t value;
        memcpy(&value, data.data() + at, 4);
        return value;
    };

    if (data.size() < 16)
    {
        return false;
    }

    uint16_t headerVersion;
    memcpy(&headerVersion, data.data() + 4, 2);
    uint32_t size = read32(0), blockOffset = read32(8), blockCount = read32(12);
    if (headerVersion != 12 || size < 16 || size > data.size() || blockOffset != 8 || blockCount > 64 || 16 + blockCount * 12ull > data.size())
    {
        return false;
    }

    for (uint32_t i = 0; i < blockCount; i++)
    {
        size_t at = 16 + i * 12;
        if (at + 4 + static_cast<uint64_t>(read32(at + 4)) + read32(at + 8) > data.size())
        {
            return false;
        }
    }
    return true;
}

// a file of a mod named in its panorama.txt: inside the mod's folder only
static bool ReadModTextFile(const ModInfo &mod, std::string_view name, std::string &text)
{
    if (name.empty() || name.find("..") != std::string_view::npos || name.find(':') != std::string_view::npos
        || name.front() == '/' || name.front() == '\\')
    {
        return false;
    }
    std::string path{ name };
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#else
    std::replace(path.begin(), path.end(), '\\', '/');
#endif
    return ReadWholeFile(ModPath(mod, path), text);
}

// the mods' panorama.txt patches, after the built-in fixes: on top of what's going into the vpk
// already (a fixed script, another mod's patch or a whole file of a mod), else the game's file
static void ApplyModPanoramaPatches(const std::vector<const ModInfo *> &enabled, std::vector<VpkFile> &out)
{
    for (const ModInfo *mod : enabled)
    {
        std::string infoPath = ModPath(*mod, "panorama.txt");
        if (!FileExists(infoPath))
        {
            continue;
        }

        KeyValueRead info{ infoPath.c_str() };
        const KeyValue *patches = info.Complete() ? info.Root().GetSubkey("panorama") : nullptr;
        if (!patches)
        {
            Problem(mod->folder, true, "panorama.txt doesn't parse (expected \"panorama\" { \"1\" { ... } }), none of its patches is applied");
            continue;
        }

        // "needs" "upgrader 1": the parts of cs2_gc the mod's code talks to, in the version it was made for
        std::string mismatch;
        std::string_view needs = patches->GetString("needs");
        for (auto word = needs.begin(); word != needs.end();)
        {
            auto end = std::find(word, needs.end(), ' ');
            std::string_view name{ word, end };
            auto numberEnd = end == needs.end() ? end : std::find(end + 1, needs.end(), ' ');
            std::string_view number = end == needs.end() ? std::string_view{} : std::string_view{ end + 1, numberEnd };
            if (!name.empty())
            {
                int have = name == "upgrader" ? UpgraderVersion : 0;
                if (!have)
                {
                    mismatch += std::format("{}'{}', which this cs2_gc doesn't have", mismatch.empty() ? "" : "; ", name);
                }
                else if (number != std::to_string(have))
                {
                    mismatch += std::format("{}{} {}, this cs2_gc has {} {}", mismatch.empty() ? "" : "; ", name,
                        number.empty() ? "?" : number, name, have);
                }
            }
            word = numberEnd == needs.end() ? needs.end() : numberEnd + 1;
        }
        if (!mismatch.empty())
        {
            Problem(mod->folder, true, std::format("made for another version of cs2_gc ({}): none of its panorama.txt patches is "
                                                   "applied. Get the mod's version for this cs2_gc", mismatch));
            continue;
        }

        for (const KeyValue &entry : patches->Subkeys())
        {
            if (entry.Subkeys().empty())
            {
                continue; // "needs" and the like
            }
            std::string what = std::format("panorama.txt patch {}", entry.Name());
            std::string path = Lower(entry.GetString("file"));
            std::replace(path.begin(), path.end(), '\\', '/');
            std::string anchor, code;
            if (path.empty() || !path.ends_with("_c"))
            {
                Problem(mod->folder, true, what + ": \"file\" has to be a compiled game file (panorama/scripts/....vts_c)");
                continue;
            }
            if (!ReadModTextFile(*mod, entry.GetString("anchor"), anchor) || !ReadModTextFile(*mod, entry.GetString("code"), code))
            {
                Problem(mod->folder, true, what + ": its \"anchor\" or \"code\" file isn't in the mod's folder");
                continue;
            }

            // the game's scripts end lines with \n, git on Windows may have checked the files out with \r\n
            for (std::string *text : { &anchor, &code })
            {
                text->erase(std::remove(text->begin(), text->end(), '\r'), text->end());
            }
            // editors end a file with a newline the script may not have after the anchor
            if (anchor.ends_with("\n"))
            {
                anchor.pop_back();
            }

            // values of cs2_gc the code asks for: @csforge:<name>@
            std::string unknown;
            for (size_t at = code.find("@csforge:"); at != std::string::npos; at = code.find("@csforge:", at))
            {
                size_t end = code.find('@', at + 9);
                std::string name = end == std::string::npos ? std::string{} : code.substr(at + 9, end - at - 9);
                if (name.starts_with("upgrader_") && !s_upgraderBuilt)
                {
                    s_upgraderBuilt = true;
                    Platform::Print("CSForge: upgrader: {} targets ({})\n", BuildUpgraderCatalog(),
                        s_upgraderCurrency.empty() ? std::string{ "chances by rarity" } : s_upgraderCurrency + " prices");
                }
                std::string value;
                if (name == "upgrader_catalog")
                {
                    value = UpgraderCatalogText();
                }
                else if (name == "upgrader_currency")
                {
                    value = s_upgraderCurrency;
                }
                else if (name == "upgrader_price_time")
                {
                    value = std::to_string(s_upgraderPriceTime);
                }
                else
                {
                    unknown = name.empty() ? std::string{ "@csforge: without its closing @" } : "@csforge:" + name + "@";
                    break;
                }
                code.replace(at, end + 1 - at, value);
                at += value.size();
            }
            if (!unknown.empty())
            {
                Problem(mod->folder, true, std::format("{}: this cs2_gc has no value {}, not applied", what, unknown));
                continue;
            }

            auto existing = std::find_if(out.begin(), out.end(), [&](const VpkFile &file) { return file.path == path; });
            std::string file;
            if (existing != out.end())
            {
                file = existing->data;
            }
            else
            {
                std::vector<char> data;
                if (!VpkReadFile(Platform::GamePath("pak01_dir.vpk"), path, data))
                {
                    Problem(mod->folder, true, std::format("{}: {} isn't in the game files", what, path));
                    continue;
                }
                file.assign(data.begin(), data.end());
            }

            std::string error;
            if (!PatchResourceText(file, anchor, code, error))
            {
                Problem(mod->folder, true, std::format("{}: not applied to {} ({}); the game's script may have changed", what, path, error));
                continue;
            }

            if (existing != out.end())
            {
                existing->data = std::move(file);
            }
            else
            {
                out.push_back({ path, std::move(file) });
            }
            Platform::Print("CSForge: {}: {} applied to {}\n", mod->folder, what, path);
        }
    }
}

bool Build()
{
    s_builtVpk.clear();
    s_upgraderCatalog.clear(); // made again if an enabled mod asks for it (the upgrader mod)
    s_upgraderIndex.clear();
    s_upgraderCurrency.clear();
    s_upgraderPriceTime = 0;
    s_upgraderBuilt = false;

#ifdef _WIN32
    std::string buildDir = Platform::DataPath("mods") + "\\" + BuildFolder;
#else
    std::string buildDir = Platform::DataPath("mods") + "/" + BuildFolder;
#endif
    std::error_code ec;
    std::filesystem::remove_all(Utf8Path(buildDir), ec);

    std::vector<ModInfo> mods = ListMods();
    std::vector<const ModInfo *> enabled;
    s_loadedState.clear();
    s_problems.clear();
    for (const ModInfo &mod : mods)
    {
        s_loadedState[mod.folder] = { mod.enabled, mod.priority };
        Platform::Print("CSForge: {} '{}' {} priority {}{}\n", mod.folder, mod.name, mod.version,
            mod.priority, mod.enabled ? "" : " (disabled)");
        if (mod.infoBroken)
        {
            Problem(mod.folder, false, "mod.txt doesn't parse (expected \"mod\" { ... }): the name, state and priority are the defaults");
        }
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
            if (path.ends_with("_c") && !ValidResource(file.data))
            {
                Problem(source.mod->folder, true, std::format("files\\{} isn't a compiled game file or it's damaged, "
                                                              "left out (the game could crash on it)", path));
                continue;
            }
            out.push_back(std::move(file));
        }
        else
        {
            Problem(source.mod->folder, true, std::format("files\\{} can't be read, it isn't in the game", path));
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

    // game file -> "paint_kits/20001" -> the mod that added it, to say who a skipped id lost to
    std::map<std::string, std::map<std::string, std::string>> addedBy;

    auto mergeInto = [&](std::string_view gameFile, MergeMode mode, auto &&modFileFor) {
        std::string text;
        bool loaded = false;
        bool changed = false;
        auto &owners = addedBy[std::string{ gameFile }];

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
                    Problem(mod->folder, true, std::format("{} isn't merged: {} not found in the game files", modFile, gameFile));
                    return;
                }
                loaded = true;
            }

            MergeReport report;
            std::string error;
            std::string merged = text;
            if (!MergeSections(merged, custom, mode, report, error))
            {
                Problem(mod->folder, true, std::format("{} isn't merged into {}, nothing of it is in the game: {}", modFile, gameFile, error));
                continue;
            }

            text = std::move(merged);
            changed = true;
            Platform::Print("CSForge: {}\\{} -> {}: {} added, {} overridden, {} skipped\n", mod->folder, modFile, gameFile,
                report.added, report.overridden.size(), report.skipped.size());
            for (const std::string &skipped : report.skipped)
            {
                if (mode == MergeMode::OverrideExisting)
                {
                    Problem(mod->folder, false, std::format("{}: {} isn't replaced (a block where the game has a text, "
                                                            "or the file sets it twice)", modFile, skipped));
                    continue;
                }
                if (mode == MergeMode::DeepOverride)
                {
                    Problem(mod->folder, false, std::format("{}: {} isn't changed: the mod has a block where the game has "
                                                            "a text, or the other way round", modFile, skipped));
                    continue;
                }

                auto owner = owners.find(skipped);
                Problem(mod->folder, false, std::format("{}: {} isn't added, {} already has it", modFile, skipped,
                    owner != owners.end() ? "the mod " + owner->second : std::string{ "the game" }));
            }
            for (const std::string &added : report.addedKeys)
            {
                owners.try_emplace(added, mod->folder);
            }
        }

        if (changed)
        {
            out.push_back({ std::string{ gameFile }, std::move(text) });
        }
    };

    mergeInto("scripts/items/items_game.txt", MergeMode::SkipExisting, [](const ModInfo &) { return std::string{ "items_game.txt" }; });
    mergeInto("scripts/items/items_game.txt", MergeMode::DeepOverride, [](const ModInfo &) { return std::string{ "items_game_override.txt" }; });

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
        ApplyPanoramaFixes(Platform::GamePath("pak01_dir.vpk"), out);
    }

    ApplyModPanoramaPatches(enabled, out);

    if (out.empty())
    {
        Platform::Print("CSForge: nothing to load ({} mods, {} enabled)\n", mods.size(), enabled.size());
        return false;
    }

    std::filesystem::create_directories(Utf8Path(buildDir), ec);
#ifdef _WIN32
    std::string vpkPath = buildDir + "\\" + VpkName + "_dir.vpk";
#else
    std::string vpkPath = buildDir + "/" + VpkName + "_dir.vpk";
#endif
    std::string vpkError;
    if (!VpkWrite(vpkPath, out, &vpkError))
    {
        Problem({}, true, std::format("can't write {} ({}): no mod is loaded", vpkPath, vpkError));
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

        auto problems = s_problems.find(mod.folder);
        if (problems != s_problems.end())
        {
            status.problems = problems->second;
        }

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

// ------------------------------------------------------------------ installing and deleting from the editor

constexpr const char *InstallFolder = "_install";
constexpr const char *TrashFolder = "_trash";
constexpr uint64_t MaxInstallBytes = 8ull << 30;
constexpr size_t MaxInstallFiles = 50000;

static std::string s_installing; // the mod folder an install is writing, empty: none
static uint64_t s_installBytes;
static size_t s_installFiles;

// a mod folder's name as the loader takes it: one path component, not hidden from ListMods
static bool ValidModFolder(std::string_view folder)
{
    if (folder.empty() || folder.size() > 64 || folder[0] == '_' || folder[0] == '.' || folder.back() == '.' || folder.back() == ' ')
    {
        return false;
    }

    return std::all_of(folder.begin(), folder.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ';
    });
}

// a file's path inside a mod, from the editor: relative, no "..", nothing Windows reads as a drive
// or a stream
static bool ValidModFilePath(std::string_view path)
{
    if (path.empty() || path.size() > 400)
    {
        return false;
    }

    size_t start = 0;
    int depth = 0;
    while (start <= path.size())
    {
        size_t end = path.find('/', start);
        std::string_view part = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' || ++depth > 24)
        {
            return false;
        }

        for (char c : part)
        {
            if (static_cast<unsigned char>(c) < 32 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            {
                return false;
            }
        }

        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }

    return true;
}

static std::filesystem::path ModsPath(std::string_view folder, std::string_view inside = {})
{
    std::string path = Platform::DataPath("mods");
    for (std::string_view part : { folder, inside })
    {
        if (!part.empty())
        {
#ifdef _WIN32
            path += "\\";
#else
            path += "/";
#endif
            path += part;
        }
    }
    return Utf8Path(path);
}

// to <data>\mods\_trash\<folder>, replacing what the trash has under that name
static bool MoveToTrash(std::string_view folder, std::string &error)
{
    std::error_code ec;
    std::filesystem::create_directories(ModsPath(TrashFolder), ec);
    std::filesystem::remove_all(ModsPath(TrashFolder, folder), ec);
    std::filesystem::rename(ModsPath(folder), ModsPath(TrashFolder, folder), ec);
    if (ec)
    {
        error = std::format("can't move mods\\{} to mods\\{} ({}): is a file of it open?", folder, TrashFolder, ec.message());
        return false;
    }
    return true;
}

bool InstallBegin(std::string_view folder, std::string &error)
{
    std::error_code ec;
    std::filesystem::remove_all(ModsPath(InstallFolder), ec);
    s_installing.clear();
    s_installBytes = 0;
    s_installFiles = 0;

    if (!ValidModFolder(folder))
    {
        error = std::format("'{}' can't be a mod folder's name (letters, digits, space, _ - . only, not starting with _ or .)", folder);
        return false;
    }

    std::filesystem::create_directories(ModsPath(InstallFolder, folder), ec);
    if (ec)
    {
        error = std::format("can't create mods\\{}\\{} ({})", InstallFolder, folder, ec.message());
        return false;
    }

    s_installing = folder;
    return true;
}

bool InstallWrite(std::string_view path, bool first, const void *data, size_t size, std::string &error)
{
    if (s_installing.empty())
    {
        error = "no install is going on";
        return false;
    }

    if (!ValidModFilePath(path))
    {
        error = std::format("'{}' isn't a path a mod's file can have", path);
        return false;
    }

    s_installBytes += size;
    s_installFiles += first ? 1 : 0;
    if (s_installBytes > MaxInstallBytes || s_installFiles > MaxInstallFiles)
    {
        error = "the archive is too big (over 8 GB or 50000 files)";
        return false;
    }

    std::filesystem::path full = ModsPath(InstallFolder, s_installing) / Utf8Path(path);
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    FILE *f = Platform::OpenFile(PathUtf8(full), first ? "wb" : "ab");
    if (!f)
    {
        error = std::format("can't write {}", path);
        return false;
    }

    bool written = fwrite(data, 1, size, f) == size;
    written = (fclose(f) == 0) && written;
    if (!written)
    {
        error = std::format("can't write {} (disk full?)", path);
    }
    return written;
}

bool InstallEnd(bool commit, std::string &summary, std::string &error)
{
    std::string folder = std::move(s_installing);
    s_installing.clear();

    std::error_code ec;
    bool done = false;
    if (folder.empty())
    {
        error = "no install is going on";
    }
    else if (!commit)
    {
        error = "stopped";
    }
    else if (!s_installFiles)
    {
        error = "the archive has no files";
    }
    else
    {
        bool replaced = std::filesystem::exists(ModsPath(folder), ec);
        if (!replaced || MoveToTrash(folder, error))
        {
            std::filesystem::rename(ModsPath(InstallFolder, folder), ModsPath(folder), ec);
            if (ec)
            {
                error = std::format("can't move the files to mods\\{} ({})", folder, ec.message());
            }
            else
            {
                summary = std::format("{}: {} files, {:.1f} MB{}", folder, s_installFiles, s_installBytes / 1048576.0,
                    replaced ? std::format(", the mod that was there is in mods\\{}", TrashFolder) : std::string{});
                Platform::Print("CSForge: installed {} (from the inventory editor, next game start)\n", summary);
                done = true;
            }
        }
    }

    std::filesystem::remove_all(ModsPath(InstallFolder), ec);
    return done;
}

bool DeleteMod(std::string_view folder, std::string &error)
{
    // only folders ListMods knows: the name comes from the editor's websocket
    std::vector<ModInfo> mods = ListMods();
    if (std::none_of(mods.begin(), mods.end(), [&](const ModInfo &m) { return m.folder == folder; }))
    {
        error = "no mod folder named " + std::string{ folder };
        return false;
    }

    if (!MoveToTrash(folder, error))
    {
        return false;
    }

    Platform::Print("CSForge: {} moved to mods\\{} (from the inventory editor)\n", folder, TrashFolder);
    return true;
}

// ------------------------------------------------------------------ filesystem hook

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

#ifndef _WIN32
using dlopen_t = void *(*)(const char *, int);
static dlopen_t Og_dlopen;
#endif

static bool HookFileSystem()
{
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"filesystem_stdio.dll");
#else
    void *module = Og_dlopen ? Og_dlopen("libfilesystem_stdio.so", RTLD_NOLOAD | RTLD_NOW) : dlopen("libfilesystem_stdio.so", RTLD_NOLOAD | RTLD_NOW);
#endif
    if (!module)
    {
        return false;
    }

    // the interface list gets filled by the dll's static constructors: while filesystem_stdio is
    // still initializing (a load nested in its own) this is null, try again on the next load
    using CreateInterface_t = void *(*)(const char *, int *);
#ifdef _WIN32
    auto createInterface = reinterpret_cast<CreateInterface_t>(GetProcAddress(module, "CreateInterface"));
#else
    auto createInterface = reinterpret_cast<CreateInterface_t>(dlsym(module, "CreateInterface"));
#endif
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

#ifdef _WIN32
    DWORD oldProtect;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect))
    {
        Platform::Print("CSForge: can't patch the filesystem vtable\n");
        return true;
    }

    s_addSearchPath = reinterpret_cast<AddSearchPath_t>(*slot);
    *slot = reinterpret_cast<void *>(&Hk_AddSearchPath);
    VirtualProtect(slot, sizeof(void *), oldProtect, &oldProtect);
#else
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;
    uintptr_t pageStart = reinterpret_cast<uintptr_t>(slot) & ~(pageSize - 1);
    if (mprotect(reinterpret_cast<void *>(pageStart), pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    {
        Platform::Print("CSForge: can't patch the filesystem vtable\n");
        return true;
    }

    s_addSearchPath = reinterpret_cast<AddSearchPath_t>(*slot);
    *slot = reinterpret_cast<void *>(&Hk_AddSearchPath);
    mprotect(reinterpret_cast<void *>(pageStart), pageSize, PROT_READ | PROT_EXEC);
#endif

    Platform::Print("CSForge: hooked VFileSystem017::AddSearchPath\n");
    return true;
}

#ifdef _WIN32

static bool IsClientDll(LPCWSTR name)
{
    std::wstring_view view = name ? name : L"";
    size_t slash = view.find_last_of(L"\\/");
    std::wstring_view file = (slash == std::wstring_view::npos) ? view : view.substr(slash + 1);
    return _wcsicmp(std::wstring{ file }.c_str(), L"client.dll") == 0;
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

#else // Linux

static bool IsClientDll(const char *name)
{
    std::string_view view = name ? name : "";
    size_t slash = view.find_last_of("/\\");
    std::string_view file = (slash == std::string_view::npos) ? view : view.substr(slash + 1);
    return file == "client.dll" || file == "libclient.so" || file == "client.so";
}

static void *Hk_dlopen(const char *filename, int flags)
{
    void *result = Og_dlopen(filename, flags);
    if (!result || (s_fileSystemHooked && s_mounts))
    {
        return result;
    }

    static std::mutex mutex;
    std::lock_guard lock{ mutex };

    if (!s_fileSystemHooked)
    {
        s_fileSystemHooked = HookFileSystem();
    }

    if (s_fileSystemHooked && !s_mounts && IsClientDll(filename))
    {
        MountMods(s_fileSystem, true, true, "client loaded");
    }

    return result;
}

static void HookDlopen()
{
    void *target = dlsym(RTLD_DEFAULT, "dlopen");
    if (!target)
    {
        void *libdl = dlopen("libdl.so.2", RTLD_LAZY | RTLD_NOLOAD);
        if (!libdl) libdl = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (libdl) target = dlsym(libdl, "dlopen");
    }
    if (!target)
    {
        Platform::Print("CSForge: can't find dlopen\n");
        return;
    }

    funchook_t *funchook = funchook_create();
    if (!funchook)
    {
        Platform::Print("CSForge: can't create funchook for dlopen\n");
        return;
    }

    void *temp = target;
    if (funchook_prepare(funchook, &temp, reinterpret_cast<void *>(&Hk_dlopen)) != 0)
    {
        Platform::Print("CSForge: can't hook dlopen: {}\n", funchook_error_message(funchook));
        funchook_destroy(funchook);
        return;
    }

    Og_dlopen = reinterpret_cast<dlopen_t>(temp);
    if (funchook_install(funchook, 0) != 0)
    {
        Platform::Print("CSForge: can't install dlopen hook: {}\n", funchook_error_message(funchook));
        funchook_destroy(funchook);
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
#else
    s_mountPath = Platform::DataPath("mods") + "/" + BuildFolder + "/" + VpkName + ".vpk";

    s_fileSystemHooked = HookFileSystem();
    HookDlopen();
#endif
}

} // namespace ModLoader

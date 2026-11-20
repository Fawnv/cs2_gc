#pragma once

// CSForge, the cs2_gc mod loader (config "mods" "1"). Mods live in <data>\mods\<folder>\:
//
//   mod.txt                   "mod" { "name" "..." "author" "..." "version" "..." "enabled" "1" "priority" "0" }
//   items_game.txt            "items_game" { "paint_kits" {...} "music_definitions" {...} ... }
//                             sections appended to the game's scripts/items/items_game.txt; an id the game
//                             (or a mod loaded earlier) already has is skipped, duplicates crash the client
//   localization.txt          "lang" { "Tokens" { ... } } added to every language in "mod_languages",
//   localization_<lang>.txt   or to one; here existing tokens get overridden (rename items, UI text)
//   items_game_override.txt   changes to what the game (or an earlier mod) already has, merged deep: a text
//                             replaces the text at the same place, a block adds to / changes the block there
//                             ("client_loot_lists" { "crate_community_33_ancient" { "[my_kit]weapon_ak47" "1" } })
//   items_extra.txt           the same sections, but only for the gc's item schema: the client doesn't get
//                             them (a case's list of knives, like the game's own *_unusual lists)
//   panorama.txt              code put into the game's compiled Panorama scripts, each patch a block:
//                             "panorama" { "1" { "file" "panorama/scripts/mainmenu.vts_c" "anchor" "anchor.js"
//                             "code" "code.js" } }: the text of code.js goes in front of the text of anchor.js,
//                             which has to be in the script exactly once (both files in the mod's folder).
//                             "needs" "upgrader 1" in the block: applied only with that version of cs2_gc's
//                             upgrader. @csforge:upgrader_catalog@ (_currency@, _price_time@) in the code
//                             are replaced by the upgrader's catalog of this start (mods\upgrader uses them)
//   files\...                 files with game relative paths (vtex_c, vcompmat_c, vsnd_c...), new or
//                             replacing the game's; for the same path the higher priority mod wins
//
// At startup every enabled mod is built into <data>\mods\_build\csforge_dir.vpk (lowest priority
// first), which is mounted in front of game\csgo from inside the process (IFileSystem::AddSearchPath).
// The CS2 folder isn't touched. See README, "CSForge".
struct VpkFile;
class Item;

namespace SkinPrices
{
class PriceList;
}

namespace ModLoader
{

struct ModInfo
{
    std::string folder; // <data>\mods\<folder>
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    bool enabled{ true };
    int priority{};
    bool infoBroken{}; // mod.txt is there but doesn't parse, the defaults are used
};

// what this run's build had to say about a mod (or about CSForge itself), for the editor's Mods tab
struct ModProblem
{
    bool error{}; // false: a warning, the rest of the mod loaded
    std::string text;
};

// every folder in <data>\mods (except _build and dot folders), sorted by load order
std::vector<ModInfo> ListMods();

// a mod as the inventory editor's Mods tab shows it
struct ModStatus
{
    ModInfo info;
    size_t files{}; // in the files folder
    bool items{}; // items_game.txt
    bool localization{}; // localization*.txt
    bool changed{}; // enabled/priority/new since this game run loaded the mods
    size_t lostFiles{}; // files\ entries a higher priority enabled mod replaces
    std::vector<std::pair<std::string, std::string>> lost; // the first few: path, winning mod folder
    std::vector<ModProblem> problems; // from the build of this game run (also in gc_log.txt)
};
std::vector<ModStatus> ModStatuses();

// the build's problems that belong to no mod: a built-in fix that no longer fits the game's
// script, a vpk that couldn't be written
std::vector<ModProblem> GeneralProblems();

// whether this game run built and mounted the mods
bool LoadedThisRun();

// writes "enabled" and "priority" into <data>\mods\<folder>\mod.txt (the rest of the file stays
// as it is); takes effect at the next game start, when the vpk gets built again
bool SetModState(std::string_view folder, bool enabled, int priority, std::string &error);

// installing a mod from the editor's Mods tab, which unpacks an archive and sends it file by file:
// the files go to <data>\mods\_install\<folder> and take the mod's place when all of it has come
// (a mod of that name there goes to the trash). One install at a time.
bool InstallBegin(std::string_view folder, std::string &error);
bool InstallWrite(std::string_view path, bool first, const void *data, size_t size, std::string &error);
bool InstallEnd(bool commit, std::string &summary, std::string &error);

// moves a mod to <data>\mods\_trash\<folder> (not loaded from there, and easy to put back by
// hand); a mod of that name already in the trash is deleted for good
bool DeleteMod(std::string_view folder, std::string &error);

// builds the vpk and hooks the filesystem. Call before Source2Main.
void Install();

// just the build (self test): false if no enabled mod contributes anything
bool Build();

// built-in fixes for the game's Panorama scripts (config "panorama_fixes"), part of the build:
// puts code into a compiled .vts_c (its DATA block is the JavaScript text) before an anchor that
// has to be there exactly once, and rebuilds the block table
bool PatchResourceText(std::string &file, std::string_view anchor, std::string_view insert, std::string &error);
// adds the fixed scripts to the files going into the vpk, returns how many applied
size_t ApplyPanoramaFixes(const std::string &gameVpk, std::vector<::VpkFile> &out);

// the upgrader (part of the Panorama fixes): the skins the window knows, in its order, with their
// prices; the window sends the target as index * 16 + wear * 2 + StatTrak, plus 1, the gc looks it
// up here. Filled by the build from the game's item schema and the market's prices
struct UpgradeTarget
{
    uint32_t defIndex;
    uint32_t paintKit;
    uint32_t tier; // Inventory::UpgradeTier, the color in the window
    // per wear (0 Factory New .. 4 Battle-Scarred) in hundredths of the currency, 0 = not on sale.
    // Without a price list made up from the rarity: 10^tier, the old 1:10 per rarity step
    std::array<uint64_t, 5> price{};
    std::array<uint64_t, 5> statTrak{};
    std::array<uint64_t, 5> souvenir{};
};
std::span<const UpgradeTarget> UpgraderCatalog();
// with the price list of config "upgrader_prices" (downloaded when it's old)
size_t BuildUpgraderCatalog();
// with this list, or by rarity when it's null (self test)
size_t BuildUpgraderCatalog(const SkinPrices::PriceList *prices);
// "RUB" and when the market made the list, or empty and 0 when the prices come from the rarity
const std::string &UpgraderCurrency();
int64_t UpgraderPriceTime();
// what an item in the inventory is worth, 0 if it's no catalog skin or not on sale in its wear
uint64_t UpgraderItemPrice(const Item &item);

// the vpk Build wrote this run, empty otherwise. The gc's item schema reads items_game.txt from it
// so it sees the same items as the client.
const std::string &BuiltVpkPath();

enum class MergeMode
{
    SkipExisting, // items_game.txt: ids already defined stay as they are
    OverrideExisting, // localization: string values get replaced
    DeepOverride, // items_game_override.txt: blocks merge at every level, texts get replaced, new keys added
};

struct MergeReport
{
    size_t added{};
    std::vector<std::string> addedKeys; // "paint_kits/20001"
    std::vector<std::string> skipped; // "paint_kits/44"
    std::vector<std::string> overridden; // "tokens/paintkit_aq_oiled_tag"
};

// appends the children of every top level section of customText to the first section of the same
// name in baseText's root (keys are compared case insensitively across all sections of that name),
// appends sections baseText doesn't have. Text level: the rest of baseText stays byte for byte.
bool MergeSections(std::string &baseText, std::string_view customText, MergeMode mode, MergeReport &report, std::string &error);

} // namespace ModLoader

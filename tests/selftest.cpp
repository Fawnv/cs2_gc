// Offline self test: exercises the parts of cs2_gc that don't need the game running.
// Reads (never writes) the CS2 install; writes only to the given data dir.
//
//   cs2_gc_selftest.exe "<cs2 install dir>" "<scratch data dir>"
//
// Checks: items_game.txt out of pak01_dir.vpk, item schema parsing, case opening
// on CS2 crates, inventory.txt round trip, SO cache build and the CS2 equip slot
// (CSOEconEquipSlot) messages produced by CMsgAdjustEquipSlots.
#include "stdafx.h"
#include "case_opening.h"
#include "config.h"
#include "mod_loader.h"
#include "skin_prices.h"
#include "gc_const.h"
#include "inventory.h"
#include "inventory_messages.h"
#include "keyvalue.h"
#include "item_schema.h"
#include "match_tracker.h"
#include "steamgameserverproxy.h"
#include "steampipecontext.h"
#include "vpk.h"
#include "base_gcmessages.pb.h"
#include "cstrike15_gcmessages.pb.h"
#include <steam/steamnetworkingtypes.h>
#include <proxy/steamgameserverproxy015.h>
#include <filesystem>
#include <set>
#include <windows.h>

static int s_failures;

#define CHECK(cond, ...) \
    do \
    { \
        if (cond) \
        { \
            printf("  ok   " __VA_ARGS__); \
        } \
        else \
        { \
            printf("  FAIL " __VA_ARGS__); \
            s_failures++; \
        } \
        printf("\n"); \
    } while (0)

static size_t CountType(const std::vector<CMsgSOSingleObject> &objects, int type)
{
    return std::count_if(objects.begin(), objects.end(), [type](const CMsgSOSingleObject &o) { return o.type_id() == type; });
}

static size_t CountUnusualItems(const LootList &lootList)
{
    size_t count = lootList.isUnusual ? lootList.items.size() : 0;
    for (const LootList *subList : lootList.subLists)
    {
        count += CountUnusualItems(*subList);
    }

    return count;
}

template<typename T>
static T AttributeValue(const ItemDesc &desc, AttributeDefIndex defIndex)
{
    for (const ItemAttribute &attribute : desc.attributes)
    {
        if (attribute.DefIndex() == defIndex)
        {
            return attribute.ValueAs<T>();
        }
    }

    return T{};
}

// stand-in for steamclient's ISteamGameServer015 object: a raw vtable with the slot
// numbers read from the SteamAPI_ISteamGameServer_* thunks in CS2's steam_api64.dll
namespace FakeGameServer
{
    uint32 s_initFlags = ~0u;
    const void *s_ticketIdentity;
    void *s_vtable[44];
    void *s_object = s_vtable;

    bool InitGameServer(void *, uint32, uint16, uint16, uint32 flags, AppId_t, const char *)
    {
        s_initFlags = flags;
        return true;
    }

    HAuthTicket GetAuthSessionTicket(void *, void *, int, uint32 *, const SteamNetworkingIdentity *identity)
    {
        s_ticketIdentity = identity;
        return 1234;
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3)
    {
        printf("usage: cs2_gc_selftest <cs2 install dir> <data dir>\n");
        return 2;
    }

    std::wstring root = argv[1];
    std::wstring dataDir = argv[2];
    std::wstring binDir = root + L"\\game\\bin\\win64";
    std::wstring gameDir = root + L"\\game\\csgo";
    CreateDirectoryW(dataDir.c_str(), nullptr);

    Platform::SetPaths(binDir.c_str(), gameDir.c_str(), dataDir.c_str());

    // no price download from the mod build in a test: the upgrader tests give their own list
    if (FILE *f = Platform::OpenFile(Platform::DataPath("config.txt"), "rb"))
    {
        fclose(f);
    }
    else if (FILE *w = Platform::OpenFile(Platform::DataPath("config.txt"), "wb"))
    {
        fprintf(w, "\"upgrader_prices\"\t\"off\"\n");
        fclose(w);
    }
    LoadConfig();

    // --prices <currency>: download the upgrader's price list into <data dir> (the network, so not
    // part of the normal run) and say what came
    if (argc >= 5 && !wcscmp(argv[3], L"--prices"))
    {
        std::wstring wide = argv[4];
        std::string currency(wide.begin(), wide.end());
        SkinPrices::PriceList list;
        bool loaded = SkinPrices::LoadPriceList(currency, 0, list);
        printf("prices: %s, %s, %zu items, made %lld, AK-47 | Redline (Field-Tested) = %llu\n", loaded ? "loaded" : "none",
            list.Currency().c_str(), list.Size(), static_cast<long long>(list.Time()),
            static_cast<unsigned long long>(list.Find("AK-47 | Redline (Field-Tested)")));
        return loaded ? 0 : 1;
    }

    // --serve <seconds>: run only the inventory editor server (http + websocket on
    // 127.0.0.1:13001) on top of <data dir>\inventory.txt, to test the editor page
    if (argc >= 5 && !wcscmp(argv[3], L"--serve"))
    {
        int seconds = _wtoi(argv[4]);
        Inventory inventory{ 76561197960287930ull };
        printf("serving the inventory editor for %d s\n", seconds);
        fflush(stdout);

        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end)
        {
            InventoryChangeMessages changes;
            inventory.Update(changes);
            Sleep(10);
        }

        return 0;
    }

    printf("[vpk]\n");
    {
        std::vector<char> data;
        auto start = std::chrono::steady_clock::now();
        bool ok = VpkReadFile(Platform::GamePath("pak01_dir.vpk"), "scripts/items/items_game.txt", data);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        CHECK(ok && data.size() > 1000000, "items_game.txt from pak01_dir.vpk: %zu bytes in %lld ms", data.size(), static_cast<long long>(ms));
        CHECK(data.size() > 12 && !memcmp(data.data(), "\"items_game\"", 12), "starts with \"items_game\"");
        CHECK(!VpkReadFile(Platform::GamePath("pak01_dir.vpk"), "scripts/items/does_not_exist.txt", data), "missing file is reported as missing");
    }

    printf("[item schema]\n");
    auto start = std::chrono::steady_clock::now();
    ItemSchema schema;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    printf("  parsed in %lld ms\n", static_cast<long long>(ms));

    {
        ItemDesc desc{};
        CHECK(schema.GetItemDesc(ToEnum<ItemDefIndex>(7), ItemOriginBaseItem, UnacknowledgedInvalid, desc), "AK-47 (def 7) exists");
        CHECK(schema.GetItemDesc(ToEnum<ItemDefIndex>(507), ItemOriginBaseItem, UnacknowledgedInvalid, desc), "Karambit (def 507) exists");
        CHECK(schema.GetAttributeType(AttributeDefIndex::StickerOffsetX0) == AttributeType::Float, "attribute 278 'sticker slot 0 offset x' is float");
        CHECK(schema.GetAttributeType(AttributeDefIndex::KeychainId0) == AttributeType::Uint32, "attribute 299 'keychain slot 0 id' is uint32");
    }

    printf("[case opening]\n");
    {
        int crates = 0, opened = 0, failed = 0, keychains = 0, guns = 0, normalGuns = 0;
        std::map<int, int> rarities;
        CaseOpening caseOpening{ schema };

        for (uint32_t def = 1; def < 20000; def++)
        {
            ItemDefIndex defIndex = ToEnum<ItemDefIndex>(def);
            if (!schema.GetCrateLootList(defIndex))
            {
                continue;
            }

            crates++;
            for (int i = 0; i < 20; i++)
            {
                ItemDesc desc{};
                if (caseOpening.SelectItemFromCrate(defIndex, desc))
                {
                    opened++;
                    rarities[static_cast<int>(desc.rarity)]++;

                    for (const ItemAttribute &attribute : desc.attributes)
                    {
                        if (attribute.DefIndex() == AttributeDefIndex::KeychainId0)
                        {
                            keychains++;
                        }
                        if (attribute.DefIndex() == AttributeDefIndex::TexturePrefab && FromEnum(desc.defIndex) < 500)
                        {
                            guns++;
                            normalGuns += desc.quality == Quality::Normal ? 1 : 0;
                        }
                    }
                }
                else
                {
                    failed++;
                }
            }
        }

        CHECK(crates > 50, "%d crates with loot lists", crates);
        CHECK(opened > 0 && failed * 10 < opened, "%d openings ok, %d failed", opened, failed);
        CHECK(keychains > 0, "%d charms dropped from charm capsules", keychains);
        CHECK(guns > 1000 && normalGuns == 0, "%d painted guns from cases, %d of normal quality (trade ups want unique/strange/tournament)", guns, normalGuns);

        // X-Ray P250 Package: loot list comes from items_extra.txt
        ItemDesc xray{};
        bool xrayOk = caseOpening.SelectItemFromCrate(ToEnum<ItemDefIndex>(4668), xray);
        uint32_t xrayPaint = 0;
        for (const ItemAttribute &attribute : xray.attributes)
        {
            if (attribute.DefIndex() == AttributeDefIndex::TexturePrefab)
            {
                xrayPaint = static_cast<uint32_t>(attribute.ValueAs<float>());
            }
        }
        CHECK(xrayOk && FromEnum(xray.defIndex) == 36 && xrayPaint == 125, "X-Ray P250 Package (4668) -> P250 | X-Ray (def %u paint %u)", FromEnum(xray.defIndex), xrayPaint);

        // cases whose knife/glove lists only exist in items_extra.txt
        const std::pair<uint32_t, const char *> newCrates[] = {
            { 4904, "Kilowatt Case" },
            { 7003, "Gallery Case" },
            { 7007, "Fever Case" },
            { 5181, "Sealed Dead Hand Terminal" },
        };

        for (auto [def, name] : newCrates)
        {
            const LootList *lootList = schema.GetCrateLootList(ToEnum<ItemDefIndex>(def));
            size_t unusuals = lootList ? CountUnusualItems(*lootList) : 0;
            ItemDesc desc{};
            bool ok = lootList && caseOpening.SelectItemFromCrate(ToEnum<ItemDefIndex>(def), desc);
            CHECK(ok && unusuals >= 13, "%s (%u) opens, %zu knives/gloves in the rare list", name, def, unusuals);
        }

        // 2025 souvenir packages: souvenir comes with a highlight reel charm (kc_aus2025 = keychain 36)
        const LootList *souvenir = schema.GetCrateLootList(ToEnum<ItemDefIndex>(5120));
        ItemDesc souvenirDesc{};
        bool souvenirOk = souvenir && caseOpening.SelectItemFromCrate(ToEnum<ItemDefIndex>(5120), souvenirDesc);
        uint32_t charm = AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::KeychainId0);
        uint32_t reel = AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::KeychainHighlight0);
        bool reelValid = souvenir && std::find(souvenir->highlightReels.begin(), souvenir->highlightReels.end(), reel) != souvenir->highlightReels.end();
        CHECK(souvenirOk && charm == 36 && reelValid, "Austin 2025 Inferno souvenir package (5120) -> charm %u, highlight %u of %zu inferno reels",
            charm, reel, souvenir ? souvenir->highlightReels.size() : 0);

        // souvenir quality: event 24 (Austin 2025), two different teams, BLAST logo + both teams + an autograph, all gold
        uint32_t team0 = AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::TournamentEventTeam0Id);
        uint32_t team1 = AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::TournamentEventTeam1Id);
        uint32_t logo = AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::StickerId0);
        uint32_t autograph = AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::StickerId3);
        const ItemSchema::TournamentStickers *aus2025 = schema.GetTournamentStickers(24);
        CHECK(souvenirDesc.quality == Quality::Tournament && AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::TournamentEventId) == 24
                && team0 && team1 && team0 != team1 && aus2025 && logo == aus2025->organizer && logo == 8685
                && AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::StickerId1) == aus2025->teams.at(team0)
                && AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::StickerId2) == aus2025->teams.at(team1) && autograph,
            "souvenir: quality 12, Austin 2025 teams %u vs %u, gold stickers %u %u %u %u (%zu teams known)", team0, team1, logo,
            AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::StickerId1), AttributeValue<uint32_t>(souvenirDesc, AttributeDefIndex::StickerId2),
            autograph, aus2025 ? aus2025->teams.size() : 0);
        ItemDesc caseDesc{};
        caseOpening.SelectItemFromCrate(ToEnum<ItemDefIndex>(7007), caseDesc);
        CHECK(caseDesc.quality != Quality::Tournament && !AttributeValue<uint32_t>(caseDesc, AttributeDefIndex::TournamentEventId),
            "a weapon case stays a weapon case (no souvenir attributes)");

        const LootList *souvenirBud = schema.GetCrateLootList(ToEnum<ItemDefIndex>(5219));
        CHECK(souvenirBud && souvenirBud->highlightKeychainId && !souvenirBud->highlightReels.empty(),
            "Budapest 2025 Inferno souvenir package (5219) -> keychain %u, %zu reels",
            souvenirBud ? souvenirBud->highlightKeychainId : 0, souvenirBud ? souvenirBud->highlightReels.size() : 0);
        for (auto [rarity, count] : rarities)
        {
            printf("       rarity %d: %d\n", rarity, count);
        }
    }

    printf("[inventory + cs2 equip slots]\n");
    {
        // 76561197960287930 = gabe, any valid id works
        constexpr uint64_t SteamId = 76561197960287930ull;
        Inventory inventory{ SteamId };

        CMsgSOCacheSubscribed cache;
        inventory.BuildCacheSubscription(cache, 1, false);

        size_t items = 0, equipSlots = 0;
        bool hasType43 = false;
        for (const auto &object : cache.objects())
        {
            if (object.type_id() == SOTypeItem)
            {
                items += object.object_data_size();
                for (const std::string &data : object.object_data())
                {
                    CSOEconItem item;
                    item.ParseFromString(data);
                    printf("       item %llx def %u quality %u rarity %u",
                        static_cast<unsigned long long>(item.id()), item.def_index(), item.quality(), item.rarity());
                    for (const CSOEconItemAttribute &attribute : item.attribute())
                    {
                        float value = 0;
                        if (attribute.value_bytes().size() == 4)
                        {
                            memcpy(&value, attribute.value_bytes().data(), 4);
                        }
                        // 6 paint kit, 7 pattern seed, 8 wear (all floats)
                        printf(" [%u]=%.9g", attribute.def_index(), value);
                    }
                    printf("\n");
                }
            }
            else if (object.type_id() == SOTypeEquipSlot)
            {
                equipSlots += object.object_data_size();
                for (const std::string &data : object.object_data())
                {
                    CSOEconEquipSlot slot;
                    slot.ParseFromString(data);
                    printf("       equip slot: class %u slot %u item %llx def %u\n",
                        slot.class_id(), slot.slot_id(), static_cast<unsigned long long>(slot.item_id()), slot.item_definition());
                }
            }
            else if (object.type_id() == SOTypeDefaultEquippedDefinitionInstanceClient)
            {
                hasType43 = true;
            }
        }

        CHECK(cache.has_owner_soid() && cache.owner_soid().id() == SteamId, "socache owner_soid set");
        printf("       %zu items, %zu equip slots\n", items, equipSlots);
        CHECK(!hasType43, "no SO type 43 (removed in cs2)");

        // equip a default AK-47 into T (class 2) rifle slot 14
        CMsgAdjustEquipSlots adjust;
        CMsgAdjustEquipSlot *slot = adjust.add_slots();
        slot->set_class_id(2);
        slot->set_slot_id(14);
        slot->set_item_id(ItemIdDefaultItemMask | 7);
        adjust.set_change_num(1);

        InventoryChangeMessages equip = inventory.AdjustEquipSlots(adjust);
        size_t created = CountType(equip.client.created, SOTypeEquipSlot);
        CHECK(created == 1, "equip default AK-47 -> %zu CSOEconEquipSlot created", created);
        if (created == 1)
        {
            CSOEconEquipSlot proto;
            proto.ParseFromString(equip.client.created[0].object_data());
            CHECK(proto.class_id() == 2 && proto.slot_id() == 14 && proto.item_definition() == 7, "created slot is class 2 / slot 14 / def 7");
        }

        // same slot, different default item -> update
        slot->set_item_id(ItemIdDefaultItemMask | 60);
        InventoryChangeMessages change = inventory.AdjustEquipSlots(adjust);
        CHECK(change.client.updated.objects_modified_size() == 1, "re-equip same slot -> %d update", change.client.updated.objects_modified_size());

        // clear the slot
        slot->set_item_id(0);
        InventoryChangeMessages clear = inventory.AdjustEquipSlots(adjust);
        size_t destroyed = CountType(clear.client.destroyed, SOTypeEquipSlot);
        CHECK(destroyed == 1, "clear slot -> %zu CSOEconEquipSlot destroyed", destroyed);
    }

    printf("[case rental]\n");
    {
        constexpr uint64_t SteamId = 76561197960287930ull;
        Inventory inventory{ SteamId };

        auto countRentals = [&](int &knives) {
            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            int rentals = 0;
            knives = 0;
            for (const auto &object : cache.objects())
            {
                if (object.type_id() != SOTypeItem)
                {
                    continue;
                }

                for (const std::string &data : object.object_data())
                {
                    CSOEconItem item;
                    item.ParseFromString(data);
                    bool rental = false, expires = false;
                    for (const CSOEconItemAttribute &attribute : item.attribute())
                    {
                        rental |= attribute.def_index() == FromEnum(AttributeDefIndex::IsRental);
                        expires |= attribute.def_index() == FromEnum(AttributeDefIndex::ExpirationDate);
                    }
                    if (rental && expires)
                    {
                        rentals++;
                        knives += item.quality() == FromEnum(Quality::Unusual);
                    }
                }
            }
            return rentals;
        };

        // CS:GO Weapon Case (4001) has "can open for rental"
        std::vector<uint64_t> bought;
        inventory.PurchaseItems({ 4001 }, bought);
        CHECK(bought.size() == 1, "bought a CS:GO Weapon Case to rent");

        if (bought.size() == 1)
        {
            InventoryChangeMessages rent = inventory.RentCrate(bought[0], 0);
            int knives = 0;
            int rentals = countRentals(knives);
            // the CS:GO Weapon Case has 9 regular finishes + the knife pool
            CHECK(rentals == 9 && rent.notification.item_id_size() == rentals,
                "rental gave %d items (notification lists %d)", rentals, rent.notification.item_id_size());
            CHECK(knives == 0, "no knives/gloves in the rental (%d)", knives);

            InventoryChangeMessages expire;
            uint32_t later = static_cast<uint32_t>(time(nullptr)) + 8 * 24 * 60 * 60;
            bool expired = inventory.ExpireRentals(later, expire);
            int left = countRentals(knives);
            CHECK(expired && left == 0, "8 days later the rented items are gone (%d left)", left);
            CHECK(!inventory.ExpireRentals(static_cast<uint32_t>(time(nullptr)), expire), "nothing expires before the date");
        }
    }

    printf("[trade up contracts]\n");
    {
        ItemSchema schema;

        // Kilowatt collection: MP7 | Just Smile (def 33, paint 1163, mythical), AWP | Chrome Cannon (def 9, paint 1170, covert)
        std::vector<TradeUpInput> mythicals(10, TradeUpInput{ ToEnum<ItemDefIndex>(33u), 1163, 0.5f });
        const std::map<uint32_t, float> legendaryMaxWear = { { 60, 0.7f }, { 31, 0.67f }, { 61, 1.0f } };

        bool allLegendary = true, wearOk = true;
        std::set<uint32_t> seen;
        for (int i = 0; i < 200; i++)
        {
            ItemDesc desc{};
            if (!schema.SelectTradeUpOutput(mythicals, Rarity::Mythical, false, false, desc))
            {
                allLegendary = false;
                break;
            }

            uint32_t def = FromEnum(desc.defIndex);
            seen.insert(def);
            auto it = legendaryMaxWear.find(def);
            allLegendary &= (desc.rarity == Rarity::Legendary && it != legendaryMaxWear.end());
            if (it != legendaryMaxWear.end())
            {
                float wear = AttributeValue<float>(desc, AttributeDefIndex::TextureWear);
                wearOk &= std::abs(wear - 0.5f * it->second) < 0.001f;
            }
        }
        CHECK(allLegendary && seen.size() == 3, "10 mythical Kilowatt -> legendary Kilowatt only (%zu of 3 outcomes seen)", seen.size());
        CHECK(wearOk, "output wear = normalized input wear (0.5) on the output's range");

        std::vector<TradeUpInput> coverts(5, TradeUpInput{ ToEnum<ItemDefIndex>(9u), 1170, 0.1f });
        ItemDesc knife{};
        bool knifeOk = schema.SelectTradeUpOutput(coverts, Rarity::Ancient, true, false, knife);
        CHECK(knifeOk && knife.quality == Quality::Unusual && FromEnum(knife.defIndex) == 526,
            "5 covert Kilowatt -> knife (def %u quality %u, kukri is 526)", FromEnum(knife.defIndex), FromEnum(knife.quality));

        ItemDesc stKnife{};
        bool stOk = schema.SelectTradeUpOutput(coverts, Rarity::Ancient, true, true, stKnife);
        CHECK(stOk && stKnife.quality == Quality::Unusual && std::any_of(stKnife.attributes.begin(), stKnife.attributes.end(), [](const ItemAttribute &a) { return a.DefIndex() == AttributeDefIndex::KillEater; }), "5 StatTrak covert -> StatTrak knife");

        // Fever knives have 7 finishes (vanilla, Marble Fade, Tiger Tooth, Doppler, Damascus, Ultraviolet,
        // Rust Coat), the Doppler phases and gems (paint 415-421) must share one finish's chance
        std::vector<TradeUpInput> feverCoverts(5, TradeUpInput{ ToEnum<ItemDefIndex>(10u), 1184, 0.1f }); // FAMAS | Bad Trip
        int feverTotal = 0, dopplers = 0, gems = 0;
        for (int i = 0; i < 20000; i++)
        {
            ItemDesc desc{};
            if (!schema.SelectTradeUpOutput(feverCoverts, Rarity::Ancient, true, false, desc))
            {
                break;
            }

            feverTotal++;
            uint32_t paint = static_cast<uint32_t>(AttributeValue<float>(desc, AttributeDefIndex::TexturePrefab));
            dopplers += (paint >= 415 && paint <= 421);
            gems += (paint >= 415 && paint <= 417);
        }

        float dopplerShare = feverTotal ? float(dopplers) / feverTotal : 0.0f;
        float gemShare = dopplers ? float(gems) / dopplers : 0.0f;
        CHECK(feverTotal == 20000 && dopplerShare > 0.12f && dopplerShare < 0.165f,
            "Fever knives: Doppler is one finish of 7 (%.1f%% of %d, expected ~14.3%%)", dopplerShare * 100, feverTotal);
        CHECK(gemShare > 0.09f && gemShare < 0.17f,
            "Ruby/Sapphire/Black Pearl are rarer than phases (%.1f%% of dopplers, expected ~13%%)", gemShare * 100);

        // end to end through the inventory: 10 MP7 | Just Smile in inventory.txt
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            fprintf(f, "\"items\"\n{\n");
            for (int i = 1; i <= 10; i++)
            {
                fprintf(f, "\t\"%d\"\n\t{\n\t\t\"inventory\"\t\t\"%d\"\n\t\t\"def_index\"\t\t\"33\"\n\t\t\"level\"\t\t\"1\"\n"
                           "\t\t\"quality\"\t\t\"4\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"8\"\n\t\t\"in_use\"\t\t\"0\"\n"
                           "\t\t\"rarity\"\t\t\"4\"\n\t\t\"attributes\"\n\t\t{\n\t\t\t\"6\"\t\t\"1163\"\n\t\t\t\"7\"\t\t\"%d\"\n\t\t\t\"8\"\t\t\"0.2\"\n\t\t}\n\t}\n",
                    i, i, i);
            }
            fprintf(f, "}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        Inventory inventory{ SteamId };
        std::vector<uint64_t> ids;
        for (uint32_t i = 1; i <= 10; i++)
        {
            ids.push_back(ComposeItemId(static_cast<uint32_t>(SteamId), i));
        }

        uint64_t createdId = 0;
        InventoryChangeMessages bad = inventory.TradeUp(4, ids, createdId);
        CHECK(!createdId, "wrong recipe (legendary inputs expected) is rejected");

        InventoryChangeMessages trade = inventory.TradeUp(3, ids, createdId);
        CMsgSOCacheSubscribed cache;
        inventory.BuildCacheSubscription(cache, 1, false);
        int items = 0;
        uint32_t createdOrigin = 0, createdRarity = 0;
        for (const auto &object : cache.objects())
        {
            if (object.type_id() != SOTypeItem)
            {
                continue;
            }

            for (const std::string &data : object.object_data())
            {
                CSOEconItem item;
                item.ParseFromString(data);
                items++;
                if (item.id() == createdId)
                {
                    createdOrigin = item.origin();
                    createdRarity = item.rarity();
                }
            }
        }
        CHECK(createdId && items == 1 && createdOrigin == ItemOriginCrafted && createdRarity == FromEnum(Rarity::Legendary),
            "inventory trade up: 10 inputs consumed, 1 legendary crafted item left (%d items, origin %u, rarity %u)", items, createdOrigin, createdRarity);
    }

    printf("[charm removal + Armory]\n");
    {
        // 1 AK-47 with a patterned charm, 2 a stock AWP that only carries a charm, 3 Remove Keychain Tool Pack,
        // 4 and 5 Armory Passes, 6 an AK-47 without a charm. No "account" block: the config's starting charges
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            auto item = [&](int id, int def, int quality, int rarity, const char *attributes) {
                fprintf(f, "\t\"%d\"\n\t{\n\t\t\"inventory\"\t\t\"%d\"\n\t\t\"def_index\"\t\t\"%d\"\n\t\t\"level\"\t\t\"1\"\n"
                           "\t\t\"quality\"\t\t\"%d\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"8\"\n\t\t\"in_use\"\t\t\"0\"\n"
                           "\t\t\"rarity\"\t\t\"%d\"\n\t\t\"attributes\"\n\t\t{\n%s\t\t}\n\t}\n",
                    id, id, def, quality, rarity, attributes);
            };
            fprintf(f, "\"items\"\n{\n");
            item(1, 7, 4, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"8\"\t\t\"0.2\"\n\t\t\t\"299\"\t\t\"1\"\n\t\t\t\"300\"\t\t\"1.5\"\n\t\t\t\"306\"\t\t\"77\"\n");
            item(2, 9, 0, 0, "\t\t\t\"299\"\t\t\"2\"\n");
            item(3, 4950, 4, 1, "");
            item(4, 1354, 4, 1, "");
            item(5, 1354, 4, 1, "");
            item(6, 7, 4, 3, "\t\t\t\"6\"\t\t\"44\"\n");
            fprintf(f, "}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        auto id = [&](uint32_t low) { return ComposeItemId(static_cast<uint32_t>(SteamId), low); };
        auto items = [](Inventory &inventory) {
            std::map<uint64_t, CSOEconItem> out;
            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            for (const auto &object : cache.objects())
            {
                for (const std::string &data : object.object_data())
                {
                    if (object.type_id() == SOTypeItem)
                    {
                        CSOEconItem item;
                        item.ParseFromString(data);
                        out[item.id()] = item;
                    }
                }
            }
            return out;
        };
        auto attribute = [](const CSOEconItem &item, AttributeDefIndex defIndex) -> const CSOEconItemAttribute * {
            for (const CSOEconItemAttribute &a : item.attribute())
            {
                if (a.def_index() == FromEnum(defIndex))
                {
                    return &a;
                }
            }
            return nullptr;
        };
        auto updatedTypes = [](const InventoryChangeMessages &messages) {
            std::set<uint32_t> types;
            for (const auto &object : messages.client.updated.objects_modified())
            {
                types.insert(object.type_id());
            }
            return types;
        };

        {
            Inventory inventory{ SteamId };

            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            uint32_t charges = 0, balance = 99;
            bool hasCharges = false, hasXpShop = false;
            for (const auto &object : cache.objects())
            {
                if (object.type_id() == SOTypeAccountKeychainRemoveToolCharges && object.object_data_size() == 1)
                {
                    CSOAccountKeychainRemoveToolCharges proto;
                    proto.ParseFromString(object.object_data(0));
                    charges = proto.charges();
                    hasCharges = true;
                }
                if (object.type_id() == SOTypeAccountXpShop && object.object_data_size() == 1)
                {
                    CSOAccountXpShop proto;
                    proto.ParseFromString(object.object_data(0));
                    balance = proto.redeemable_balance();
                    hasXpShop = true;
                }
            }
            CHECK(hasCharges && charges == GetConfig().m_keychainRemoveCharges && hasXpShop && balance == 0,
                "SO cache has KeychainRemoveToolCharges (type 15, %u charges) and XpShop (type 6, %u stars)", charges, balance);

            InventoryChangeMessages removed = inventory.RemoveKeychain(id(1), 0);
            auto after = items(inventory);
            const CSOEconItem *charm = nullptr;
            for (const auto &[itemId, item] : after)
            {
                if (item.def_index() == FromEnum(ItemDefIndex::Keychain))
                {
                    charm = &item;
                }
            }
            CHECK(!attribute(after[id(1)], AttributeDefIndex::KeychainId0) && !attribute(after[id(1)], AttributeDefIndex::KeychainOffsetX0)
                    && attribute(after[id(1)], AttributeDefIndex::TexturePrefab),
                "charm removed from the AK-47, its skin kept");
            CHECK(charm && attribute(*charm, AttributeDefIndex::KeychainId0) && attribute(*charm, AttributeDefIndex::KeychainSeed0),
                "the charm is back in the inventory as a keychain item with its pattern seed");
            CHECK(inventory.KeychainRemoveCharges() == GetConfig().m_keychainRemoveCharges - 1
                    && removed.notification.request() == k_EGCItemCustomizationNotification_RemoveKeychain
                    && updatedTypes(removed).contains(SOTypeAccountKeychainRemoveToolCharges),
                "a removal uses a charge (%u left) and tells the client", inventory.KeychainRemoveCharges());

            inventory.RemoveKeychain(id(2), 0);
            CHECK(!items(inventory).contains(id(2)), "a stock weapon that only carried the charm goes away");
            CHECK(!inventory.RemoveKeychain(id(6), 0).notification.has_request(), "no charm, nothing to remove");

            InventoryChangeMessages pack = inventory.UseItem(id(3));
            CHECK(inventory.KeychainRemoveCharges() == GetConfig().m_keychainRemoveCharges - 2 + 3 && !items(inventory).contains(id(3))
                    && updatedTypes(pack).contains(SOTypeAccountKeychainRemoveToolCharges),
                "Remove Keychain Tool Pack adds 3 charges (%u)", inventory.KeychainRemoveCharges());

            InventoryChangeMessages pass = inventory.UseItem(id(4));
            CHECK(inventory.XpShopBalance() == 40 && inventory.XpShopTrackCount() == 1 && !items(inventory).contains(id(4))
                    && pass.notification.request() == k_EGCItemCustomizationNotification_XpShopUseTicket
                    && updatedTypes(pass).contains(SOTypeAccountXpShop),
                "Armory Pass activated: 40 stars, one full track (%u stars)", inventory.XpShopBalance());

            // redeem_id values the game sent (gc_log, 2026-11-07): CRC-32 of the entry's item_name
            auto entryFor = [&](uint32_t redeemId) { return schema.GetXpShopEntry(11, redeemId); };
            const ItemSchema::XpShopEntry *gameCase = entryFor(1025083006), *drBoom = entryFor(1986856372),
                                          *community = entryFor(4187462448), *arabesque = entryFor(1716898345);
            CHECK(gameCase && gameCase->itemName == "crate_community_35" && gameCase->points == 2
                    && drBoom && drBoom->itemName == "lootlist:keychain_pack_kc_drboom_lootlist" && drBoom->points == 3
                    && community && community->points == 3 && arabesque && arabesque->itemName == "lootlist:set_arabesque"
                    && arabesque->points == 4,
                "Armory: the redeem_ids the game sends find their entries (case 2, charms 3, collection 4 stars)");

            // every shop entry once: 3 collections (4), a case (2), 4 charm packs (3), 3 sticker packs (1) = 29 stars
            std::span<const ItemSchema::XpShopEntry> shop = schema.GetXpShopEntries(11);
            int rewards = 0;
            bool caseOk = false, charmOk = false, originOk = true;
            for (const ItemSchema::XpShopEntry &entry : shop)
            {
                uint64_t createdId = 0;
                inventory.RedeemXpShopReward(11, entry.redeemId, entry.points, createdId);
                auto now = items(inventory);
                if (createdId && now.contains(createdId))
                {
                    rewards++;
                    originOk &= now[createdId].origin() == ItemOriginQuestDrop;
                    caseOk |= &entry == gameCase && now[createdId].def_index() != 0 && !attribute(now[createdId], AttributeDefIndex::TexturePrefab);
                    charmOk |= &entry == drBoom && now[createdId].def_index() == FromEnum(ItemDefIndex::Keychain);
                }
            }
            CHECK(shop.size() == 11 && rewards == 11 && caseOk && charmOk && originOk && inventory.XpShopBalance() == 40 - 29,
                "Armory: all %zu entries give an item (case, charm...), 29 stars spent, %u left", shop.size(), inventory.XpShopBalance());

            uint64_t createdId = 0;
            const uint32_t collection = arabesque ? arabesque->redeemId : 0;
            inventory.RedeemXpShopReward(11, collection, 4, createdId); // 11 -> 7
            inventory.RedeemXpShopReward(11, collection, 4, createdId); // 7 -> 3
            inventory.RedeemXpShopReward(11, collection, 4, createdId);
            CHECK(!createdId && inventory.XpShopBalance() == 3, "Armory: not enough stars, nothing bought (%u left)", inventory.XpShopBalance());
            inventory.RedeemXpShopReward(11, 3, 1, createdId); // a shop index, not a redeem_id
            CHECK(!createdId, "Armory: unknown shop entry rejected");

            InventoryChangeMessages ack = inventory.AckXpShopTracks();
            CHECK(inventory.XpShopTrackCount() == 0 && updatedTypes(ack).contains(SOTypeAccountXpShop), "finished pass acknowledged and dropped");
        }

        {
            Inventory reloaded{ SteamId };
            CHECK(reloaded.KeychainRemoveCharges() == GetConfig().m_keychainRemoveCharges + 1 && reloaded.XpShopBalance() == 3
                    && reloaded.XpShopTrackCount() == 0,
                "charges (%u) and stars (%u) kept in inventory.txt", reloaded.KeychainRemoveCharges(), reloaded.XpShopBalance());
            InventoryChangeMessages pass = reloaded.UseItem(id(5));
            CHECK(reloaded.XpShopBalance() == 43, "the second pass adds 40 more (%u)", reloaded.XpShopBalance());
        }
    }

    printf("[StatTrak swap + terminals + xp + care package]\n");
    {
        // 1 StatTrak AK-47 (100 kills), 2 StatTrak AWP (7 kills), 3 and 6 StatTrak Swap Tools,
        // 4 StatTrak music kit (MVP counter), 5 Sealed Genesis Terminal, 7 Sealed Dead Hand Terminal.
        // Level 2 with 4000 xp, no care package yet
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            auto item = [&](int id, int def, int quality, int rarity, const char *attributes) {
                fprintf(f, "\t\"%d\"\n\t{\n\t\t\"inventory\"\t\t\"%d\"\n\t\t\"def_index\"\t\t\"%d\"\n\t\t\"level\"\t\t\"1\"\n"
                           "\t\t\"quality\"\t\t\"%d\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"8\"\n\t\t\"in_use\"\t\t\"0\"\n"
                           "\t\t\"rarity\"\t\t\"%d\"\n\t\t\"attributes\"\n\t\t{\n%s\t\t}\n\t}\n",
                    id, id, def, quality, rarity, attributes);
            };
            fprintf(f, "\"items\"\n{\n");
            item(1, 7, 9, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"80\"\t\t\"100\"\n\t\t\t\"81\"\t\t\"0\"\n");
            item(2, 9, 9, 3, "\t\t\t\"6\"\t\t\"51\"\n\t\t\t\"80\"\t\t\"7\"\n\t\t\t\"81\"\t\t\"0\"\n");
            item(3, 1324, 4, 1, "");
            item(4, 1314, 9, 3, "\t\t\t\"166\"\t\t\"3\"\n\t\t\t\"80\"\t\t\"5\"\n\t\t\t\"81\"\t\t\"1\"\n");
            item(5, 5176, 4, 1, "");
            item(6, 1324, 4, 1, "");
            item(7, 5181, 4, 1, "");
            fprintf(f, "}\n\"account\"\n{\n\t\"player_level\"\t\t\"2\"\n\t\"player_xp\"\t\t\"4000\"\n}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        auto id = [&](uint32_t low) { return ComposeItemId(static_cast<uint32_t>(SteamId), low); };
        auto cacheObjects = [](Inventory &inventory, uint32_t type) {
            std::vector<std::string> out;
            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            for (const auto &object : cache.objects())
            {
                if (object.type_id() == type)
                {
                    out.insert(out.end(), object.object_data().begin(), object.object_data().end());
                }
            }
            return out;
        };
        auto items = [&](Inventory &inventory) {
            std::map<uint64_t, CSOEconItem> out;
            for (const std::string &data : cacheObjects(inventory, SOTypeItem))
            {
                CSOEconItem item;
                item.ParseFromString(data);
                out[item.id()] = item;
            }
            return out;
        };
        auto attribute = [](const CSOEconItem &item, AttributeDefIndex defIndex) -> const CSOEconItemAttribute * {
            for (const CSOEconItemAttribute &a : item.attribute())
            {
                if (a.def_index() == FromEnum(defIndex))
                {
                    return &a;
                }
            }
            return nullptr;
        };
        auto u32 = [&](const CSOEconItem &item, AttributeDefIndex defIndex) -> uint32_t {
            const CSOEconItemAttribute *a = attribute(item, defIndex);
            uint32_t value = 0;
            if (a && a->value_bytes().size() == 4)
            {
                memcpy(&value, a->value_bytes().data(), 4);
            }
            return value;
        };
        auto stored = [&](std::map<uint64_t, CSOEconItem> &all, uint64_t casketId) {
            std::vector<uint64_t> out;
            for (const auto &[itemId, item] : all)
            {
                uint64_t casket = (static_cast<uint64_t>(u32(item, AttributeDefIndex::CasketIdHigh)) << 32)
                    | u32(item, AttributeDefIndex::CasketIdLow);
                if (casket == casketId)
                {
                    out.push_back(itemId);
                }
            }
            return out;
        };

        CHECK(Inventory::XpWeekStart(1794052800) == 1793750400, "xp week of Sat 2026-11-07 starts Wed 2026-11-04 00:00 UTC");

        {
            Inventory inventory{ SteamId };

            // StatTrak swap
            InventoryChangeMessages swap = inventory.StatTrakSwap(id(3), id(1), id(2));
            auto now = items(inventory);
            CHECK(u32(now[id(1)], AttributeDefIndex::KillEater) == 7 && u32(now[id(2)], AttributeDefIndex::KillEater) == 100
                    && !now.contains(id(3)) && swap.notification.request() == k_EGCItemCustomizationNotification_StatTrakSwap
                    && swap.notification.item_id_size() == 2,
                "StatTrak swap: AK-47 100 <-> AWP 7 kills, tool used up");
            InventoryChangeMessages wrongKind = inventory.StatTrakSwap(id(6), id(1), id(4));
            CHECK(!wrongKind.notification.has_request() && items(inventory).contains(id(6)),
                "StatTrak swap: kills and a music kit's MVPs don't mix, tool kept");

            // terminal: unseal, 5 offers, decline the final one
            CHECK(inventory.IsSealedTerminal(id(5)) && !inventory.IsActiveTerminal(id(5)) && !inventory.IsSealedTerminal(id(1)),
                "Sealed Genesis Terminal recognised by its volatile container attribute");
            uint64_t terminal = 0;
            InventoryChangeMessages unseal = inventory.UnsealTerminal(id(5), terminal);
            now = items(inventory);
            CHECK(terminal && now.contains(terminal) && !now.contains(id(5)) && now[terminal].quality() == 14
                    && u32(now[terminal], AttributeDefIndex::IsRental) == 1
                    && u32(now[terminal], AttributeDefIndex::ExpirationDate) > static_cast<uint32_t>(time(nullptr))
                    && unseal.notification.request() == k_EGCItemCustomizationNotification_UnlockCrate
                    && unseal.notification.item_id(0) == terminal && inventory.IsActiveTerminal(terminal),
                "unsealed: an active terminal (quality 14, rental), crate_unlock with its id");

            std::vector<int32_t> numbers;
            bool oneOfferEach = true, priced = true, notified = true;
            uint64_t lastOffer = 0;
            for (uint32_t i = 0; i < Inventory::TerminalOffers; i++)
            {
                InventoryChangeMessages offer = inventory.NextTerminalOffer(terminal, 0);
                now = items(inventory);
                std::vector<uint64_t> inside = stored(now, terminal);
                oneOfferEach &= inside.size() == 1 && inside[0] != lastOffer;
                if (inside.size() == 1)
                {
                    lastOffer = inside[0];
                    priced &= u32(now[lastOffer], AttributeDefIndex::PurchasePrice) > 0;
                }
                notified &= offer.notification.request() == k_EGCItemCustomizationNotification_CasketContents
                    && offer.notification.item_id_size() == 1 && offer.notification.item_id(0) == terminal;
                numbers.push_back(static_cast<int32_t>(u32(now[terminal], AttributeDefIndex::QuestPointsRemaining)));
            }
            CHECK(oneOfferEach && priced && notified && numbers == std::vector<int32_t>({ 1, 2, 3, 4, -5 }),
                "5 offers, one stored in the terminal at a time with a price, numbered 1 2 3 4 -5 (final)");

            std::vector<std::string> seen = cacheObjects(inventory, SOTypeVolatileItemOffer);
            CSOVolatileItemOffer history;
            if (!seen.empty())
            {
                history.ParseFromString(seen[0]);
            }
            CHECK(seen.size() == 1 && history.defidx() == 5176 && history.faux_itemid_size() >= 1
                    && (history.faux_itemid(0) >> 60) == 0xF,
                "offers seen go to SO type 20 for the collection catalog (%d faux ids)", history.faux_itemid_size());

            InventoryChangeMessages ended = inventory.NextTerminalOffer(terminal, 0);
            now = items(inventory);
            CHECK(!now.contains(terminal) && !now.contains(lastOffer)
                    && ended.notification.request() == k_EGCItemCustomizationNotification_CasketContents,
                "asking after the final offer closes the terminal, its offer goes with it");

            // terminal: buy an offer under a price limit
            uint64_t deadHand = 0;
            inventory.UnsealTerminal(id(7), deadHand);
            inventory.NextTerminalOffer(deadHand, 500);
            now = items(inventory);
            std::vector<uint64_t> inside = stored(now, deadHand);
            uint32_t price = inside.size() == 1 ? u32(now[inside[0]], AttributeDefIndex::PurchasePrice) : 0;
            uint64_t found = 0;
            uint64_t offerId = inventory.TerminalOfferFor(5181, 0, price, &found);
            CHECK(inside.size() == 1 && offerId == inside[0] && found == deadHand && price <= 500
                    && inventory.TerminalOfferFor(5181, deadHand, 0) == offerId && !inventory.TerminalOfferFor(5181, 0, 1)
                    && !inventory.TerminalOfferFor(5176, deadHand, price),
                "Dead Hand offer under the 500 limit (%u), found by terminal id or price; a sealed one from the store isn't", price);
            InventoryChangeMessages reopened = inventory.TerminalContents(deadHand);
            CHECK(reopened.notification.request() == k_EGCItemCustomizationNotification_CasketContents,
                "reopening an active terminal answers casket_contents");
            uint64_t bought = 0;
            inventory.BuyTerminalOffer(deadHand, bought);
            now = items(inventory);
            CHECK(bought == offerId && now.contains(bought) && !now.contains(deadHand) && stored(now, deadHand).empty()
                    && !attribute(now[bought], AttributeDefIndex::CasketIdLow)
                    && u32(now[bought], AttributeDefIndex::PurchaserAccountId) == static_cast<uint32_t>(SteamId),
                "bought: the item keeps its id, leaves the terminal with an original owner certificate, terminal gone");

            // collection xp, levels, the weekly care package
            InventoryChangeMessages claim = inventory.ClaimTerminalReward(5176, 3);
            std::vector<std::string> claimedObjects = cacheObjects(inventory, SOTypeVolatileItemClaimedRewards);
            CSOVolatileItemClaimedRewards claimed;
            if (!claimedObjects.empty())
            {
                claimed.ParseFromString(claimedObjects[0]);
            }
            CHECK(claim.levelChanged && inventory.PlayerLevel() == 2 && inventory.PlayerXp() == 4500
                    && claimed.reward_size() == 1 && claimed.reward(0) == 3,
                "collection tier 3 claimed: +500 xp (%u), recorded in SO type 21", inventory.PlayerXp());
            CHECK(!inventory.ClaimTerminalReward(5176, 3).levelChanged, "a tier can't be claimed twice");

            InventoryChangeMessages levelUp = inventory.AddXp(1000, "test");
            now = items(inventory);
            std::vector<uint64_t> package = inventory.CarePackageItems();
            bool hidden = package.size() == Inventory::CarePackageSize;
            for (uint64_t itemId : package)
            {
                // an acknowledged position (0 or the unacknowledged bit = the client's "new items" popup)
                hidden &= now.contains(itemId) && now[itemId].inventory() != 0 && !(now[itemId].inventory() & (1u << 30))
                    && attribute(now[itemId], AttributeDefIndex::FreeRewardStatus);
            }
            std::vector<std::string> storeObjects = cacheObjects(inventory, SOTypeAccountItemPersonalStore);
            CSOAccountItemPersonalStore store;
            if (!storeObjects.empty())
            {
                store.ParseFromString(storeObjects[0]);
            }
            CHECK(levelUp.levelChanged && inventory.PlayerLevel() == 3 && inventory.PlayerXp() == 500 && hidden
                    && store.redeemable_balance() == 2 && store.items_size() == 4 && inventory.CarePackageEarnedThisWeek(),
                "level 3: the week's care package, 4 items to pick 2 (SO type 4), no \"new items\" popup for them");

            InventoryChangeMessages picked = inventory.RedeemFreeRewards(std::vector<uint64_t>{ package[0], package[2] });
            now = items(inventory);
            CHECK(picked.notification.request() == k_EGCItemCustomizationNotification_ClientRedeemFreeReward
                    && now.contains(package[0]) && now.contains(package[2]) && !now.contains(package[1]) && !now.contains(package[3])
                    && !attribute(now[package[0]], AttributeDefIndex::FreeRewardStatus),
                "picked 2: they join the inventory, the other 2 are gone");

            inventory.AddXp(5000, "test");
            CHECK(inventory.PlayerLevel() == 4 && inventory.CarePackageItems() == package,
                "the next level up in the same week brings no second package");
            inventory.AddXp(500000, "test");
            CHECK(inventory.PlayerLevel() == Inventory::MaxLevel && inventory.PlayerXp() == 500,
                "level 40 is the top, its bar starts over (%u, %u xp)", inventory.PlayerLevel(), inventory.PlayerXp());
        }

        {
            Inventory reloaded{ SteamId };
            std::vector<std::string> seen = cacheObjects(reloaded, SOTypeVolatileItemOffer);
            CHECK(reloaded.PlayerLevel() == Inventory::MaxLevel && reloaded.CarePackageEarnedThisWeek() && seen.size() == 2
                    && !reloaded.ClaimTerminalReward(5176, 3).levelChanged,
                "level, care package week, terminal history and claims kept in inventory.txt");
        }
    }

    printf("[sticker slab + inspect + viewer pass + store]\n");
    {
        // 1 and 3 stickers, 2 a Sticker Slab, 4 AK-47 Redline-ish (paint 44, wear 0.2), 5 Austin 2025 Viewer Pass + 3 tokens
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            auto item = [&](int id, int def, int quality, int rarity, const char *attributes) {
                fprintf(f, "\t\"%d\"\n\t{\n\t\t\"inventory\"\t\t\"%d\"\n\t\t\"def_index\"\t\t\"%d\"\n\t\t\"level\"\t\t\"1\"\n"
                           "\t\t\"quality\"\t\t\"%d\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"8\"\n\t\t\"in_use\"\t\t\"0\"\n"
                           "\t\t\"rarity\"\t\t\"%d\"\n\t\t\"attributes\"\n\t\t{\n%s\t\t}\n\t}\n",
                    id, id, def, quality, rarity, attributes);
            };
            fprintf(f, "\"items\"\n{\n");
            item(1, 1209, 4, 6, "\t\t\t\"113\"\t\t\"8557\"\n");
            item(2, 4000, 4, 1, "");
            item(3, 1209, 4, 3, "\t\t\t\"113\"\t\t\"8554\"\n");
            item(4, 7, 4, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"7\"\t\t\"321\"\n\t\t\t\"8\"\t\t\"0.2\"\n");
            item(5, 5115, 4, 1, "");
            fprintf(f, "}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        auto id = [&](uint32_t low) { return ComposeItemId(static_cast<uint32_t>(SteamId), low); };
        auto items = [](Inventory &inventory) {
            std::map<uint64_t, CSOEconItem> out;
            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            for (const auto &object : cache.objects())
            {
                for (const std::string &data : object.object_data())
                {
                    if (object.type_id() == SOTypeItem)
                    {
                        CSOEconItem item;
                        item.ParseFromString(data);
                        out[item.id()] = item;
                    }
                }
            }
            return out;
        };
        auto u32 = [](const CSOEconItem &item, AttributeDefIndex defIndex) -> uint32_t {
            for (const CSOEconItemAttribute &a : item.attribute())
            {
                if (a.def_index() == FromEnum(defIndex) && a.value_bytes().size() == 4)
                {
                    uint32_t value;
                    memcpy(&value, a.value_bytes().data(), 4);
                    return value;
                }
            }
            return 0;
        };
        auto created = [](const std::map<uint64_t, CSOEconItem> &before, const std::map<uint64_t, CSOEconItem> &after) {
            std::vector<CSOEconItem> out;
            for (const auto &[itemId, item] : after)
            {
                if (!before.contains(itemId))
                {
                    out.push_back(item);
                }
            }
            return out;
        };

        Inventory inventory{ SteamId };

        // sticker slab: sticker + slab, then a sticker on its own (InventoryAPI.RemoveKeychain(sticker, 0))
        auto before = items(inventory);
        CMsgApplySticker wrap;
        wrap.set_sticker_item_id(id(1));
        wrap.set_item_item_id(id(2));
        InventoryChangeMessages slabbed;
        bool wrapped = inventory.TryEncapsulateSticker(wrap, slabbed);
        auto after = items(inventory);
        std::vector<CSOEconItem> fresh = created(before, after);
        CHECK(wrapped && fresh.size() == 1 && fresh[0].def_index() == FromEnum(ItemDefIndex::Keychain)
                && u32(fresh[0], AttributeDefIndex::KeychainId0) == 37 && u32(fresh[0], AttributeDefIndex::KeychainSticker0) == 8557
                && fresh[0].rarity() == 6 && !after.contains(id(1)) && !after.contains(id(2))
                && slabbed.notification.request() == k_EGCItemCustomizationNotification_EncapsulateSticker,
            "sticker slab: sticker 8557 + slab -> charm of keychain 37 showing it, both used up, keychain_seal");

        before = after;
        CMsgApplySticker wrapAlone;
        wrapAlone.set_sticker_item_id(ItemIdDefaultItemMask | FromEnum(ItemDefIndex::KeychainRemoveTool));
        wrapAlone.set_item_item_id(id(3));
        bool wrappedAlone = inventory.TryEncapsulateSticker(wrapAlone, slabbed);
        fresh = created(before, items(inventory));
        CHECK(wrappedAlone && fresh.size() == 1 && u32(fresh[0], AttributeDefIndex::KeychainSticker0) == 8554,
            "sticker slab without a slab item (RemoveKeychain on the sticker) works too");
        CMsgApplySticker notSlab;
        notSlab.set_sticker_item_id(ItemIdDefaultItemMask | FromEnum(ItemDefIndex::KeychainRemoveTool));
        notSlab.set_item_item_id(id(4));
        CHECK(!inventory.TryEncapsulateSticker(notSlab, slabbed), "charm removal on a weapon isn't taken for a slab");

        // inspect links
        CEconItemPreviewDataBlock block;
        bool inspected = inventory.PreviewDataBlock(id(4), block);
        CEconItemPreviewDataBlock missing;
        CHECK(inspected && block.defindex() == 7 && block.paintindex() == 44 && block.paintseed() == 321
                && block.paintwear() == std::bit_cast<uint32_t>(0.2f) && !inventory.PreviewDataBlock(id(99), missing),
            "inspect: AK-47 paint 44 seed 321, exact float 0.2 as bits; unknown items have no data");

        // viewer pass: coin + 3 souvenir tokens, a token is a souvenir package
        before = items(inventory);
        InventoryChangeMessages pass = inventory.UseItem(id(5));
        after = items(inventory);
        fresh = created(before, after);
        std::map<uint32_t, int> defs;
        for (const CSOEconItem &item : fresh)
        {
            defs[item.def_index()]++;
        }
        CHECK(!after.contains(id(5)) && defs[5111] == 1 && defs[5116] == 3
                && pass.notification.request() == k_EGCItemCustomizationNotification_ActivateFanToken,
            "Austin 2025 Viewer Pass + tokens -> coin 5111 and 3 souvenir tokens 5116");
        uint64_t token = 0;
        for (const CSOEconItem &item : fresh)
        {
            token = item.def_index() == 5116 ? item.id() : token;
        }
        before = after;
        inventory.UseItem(token);
        fresh = created(before, items(inventory));
        CHECK(fresh.size() == 1 && fresh[0].def_index() >= 5120 && fresh[0].def_index() <= 5126,
            "a souvenir token -> an Austin 2025 souvenir package (%u)", fresh.empty() ? 0 : fresh[0].def_index());

        // the store has the new majors and the terminals, all of them real items
        KeyValueRead priceSheet{ Platform::DataPath("price_sheet.txt").c_str() };
        const KeyValue *entries = nullptr;
        if (const KeyValue *store = priceSheet.Good() ? priceSheet.Root().GetSubkey("store") : nullptr)
        {
            entries = store->GetSubkey("entries");
        }
        bool known = entries != nullptr;
        int found = 0;
        for (const char *name : { "tournament_pass_aus2025", "crate_sticker_pack_bud2025_legends", "crate_bud2025_promo_de_train",
                 "tournament_pass_cologne2026", "crate_community_36", "crate_community_37", "sticker_display_case" })
        {
            const KeyValue *entry = entries ? entries->GetSubkey(name) : nullptr;
            found += entry ? 1 : 0;
            known &= entry && schema.ItemDefIndexByName(entry->GetString("item_link")).has_value();
        }
        CHECK(known && found == 7, "price sheet sells the 2025/2026 majors, terminals and the slab, every link is a real item (%d/7)", found);
    }

    printf("[storage unit activation]\n");
    {
        // 1 a new Storage Unit straight from the store, no name yet
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            fprintf(f, "\"items\"\n{\n\t\"1\"\n\t{\n\t\t\"inventory\"\t\t\"1\"\n\t\t\"def_index\"\t\t\"1201\"\n\t\t\"level\"\t\t\"1\"\n"
                       "\t\t\"quality\"\t\t\"4\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"2\"\n\t\t\"in_use\"\t\t\"0\"\n"
                       "\t\t\"rarity\"\t\t\"1\"\n\t\t\"attributes\"\n\t\t{\n\t\t}\n\t}\n}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        uint64_t casketId = ComposeItemId(static_cast<uint32_t>(SteamId), 1);
        Inventory inventory{ SteamId };

        // the client names a storage unit with a faux Name Tag (def 1200) as the tool
        constexpr uint64_t FauxNameTag = ItemIdDefaultItemMask | 1200;
        InventoryChangeMessages named = inventory.NameItem(FauxNameTag, casketId, "my skins");

        CMsgSOCacheSubscribed cache;
        inventory.BuildCacheSubscription(cache, 1, false);
        bool hasName = false, hasCount = false, hasDate = false;
        int count = 0;
        for (const auto &object : cache.objects())
        {
            for (const std::string &data : object.object_data())
            {
                CSOEconItem item;
                if (object.type_id() != SOTypeItem || !item.ParseFromString(data))
                {
                    continue;
                }
                count++;
                for (const CSOEconItemAttribute &a : item.attribute())
                {
                    hasName |= a.def_index() == FromEnum(AttributeDefIndex::CustomName) && a.value_bytes().find("my skins") != std::string::npos;
                    hasCount |= a.def_index() == FromEnum(AttributeDefIndex::CasketItemsCount);
                    hasDate |= a.def_index() == FromEnum(AttributeDefIndex::CasketModificationDate);
                }
            }
        }
        CHECK(hasName && hasCount && hasDate && count == 1
                && named.notification.request() == k_EGCItemCustomizationNotification_NameItem
                && named.notification.item_id_size() == 1 && named.notification.item_id(0) == casketId,
            "a new storage unit named with the faux Name Tag gets its name, item count and date, the client is notified");
    }

    printf("[old normal quality skins]\n");
    {
        // 1 AK-47 with a paint and normal quality (older builds, csgo_gc), 2 a stock AK-47, 3 a knife
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            auto item = [&](int id, int def, int quality, int rarity, const char *attributes) {
                fprintf(f, "\t\"%d\"\n\t{\n\t\t\"inventory\"\t\t\"%d\"\n\t\t\"def_index\"\t\t\"%d\"\n\t\t\"level\"\t\t\"1\"\n"
                           "\t\t\"quality\"\t\t\"%d\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"8\"\n\t\t\"in_use\"\t\t\"0\"\n"
                           "\t\t\"rarity\"\t\t\"%d\"\n\t\t\"attributes\"\n\t\t{\n%s\t\t}\n\t}\n",
                    id, id, def, quality, rarity, attributes);
            };
            fprintf(f, "\"items\"\n{\n");
            item(1, 7, 0, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"8\"\t\t\"0.2\"\n");
            item(2, 7, 0, 1, "");
            item(3, 507, 0, 6, "\t\t\t\"6\"\t\t\"38\"\n");
            fprintf(f, "}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        Inventory inventory{ SteamId };
        CMsgSOCacheSubscribed cache;
        inventory.BuildCacheSubscription(cache, 1, false);
        std::map<uint32_t, uint32_t> quality; // low id -> quality
        for (const auto &object : cache.objects())
        {
            for (const std::string &data : object.object_data())
            {
                CSOEconItem item;
                if (object.type_id() == SOTypeItem && item.ParseFromString(data))
                {
                    quality[static_cast<uint32_t>(item.id())] = item.quality();
                }
            }
        }
        CHECK(quality[1] == FromEnum(Quality::Unique) && quality[2] == FromEnum(Quality::Normal) && quality[3] == FromEnum(Quality::Normal),
            "on load a painted gun of normal quality becomes unique (trade up), a stock gun and a knife stay (%u %u %u)",
            quality[1], quality[2], quality[3]);
    }

    printf("[match xp]\n");
    {
        // one rich presence batch, in the order client.dll sends it
        auto batch = [](MatchTracker &tracker, uint32_t now, const char *state, const char *mode, const char *map,
                         const char *score, const char *server) {
            std::optional<MatchSummary> ended;
            const std::pair<const char *, const char *> keys[] = {
                { "game:state", state }, { "game:mode", mode }, { "game:map", map }, { "game:score", score }, { "game:server", server }
            };
            for (const auto &[key, value] : keys)
            {
                std::optional<MatchSummary> match = tracker.Update(key, value, now);
                if (match)
                {
                    ended = match;
                }
            }
            return ended;
        };

        {
            // competitive with bots, won 13:7, then back to the main menu
            MatchTracker tracker;
            bool quiet = !batch(tracker, 1000, "game", "competitive", "de_dust2", "", "offline")
                && !batch(tracker, 1600, "game", "competitive", "de_dust2", "[ 5 : 3 ]", "offline")
                && !batch(tracker, 2800, "game", "competitive", "de_dust2", "[ 13 : 7 ]", "offline");
            std::optional<MatchSummary> match = batch(tracker, 2900, "lobby", "", "", "", "");
            CHECK(quiet && match && match->mode == "competitive" && match->map == "de_dust2" && match->server == "offline"
                    && match->roundsWon == 13 && match->roundsLost == 7 && match->seconds == 1900 && MatchBaseXp(*match) == 390
                    && MatchFinished(*match) && !tracker.InMatch(),
                "bot competitive 13:7: the match ends in the lobby, 30 xp per round won = 390");
        }

        {
            // left a bot match at 2:0 after 145 s: not over, no xp (as in CS2)
            MatchTracker tracker;
            batch(tracker, 0, "game", "competitive", "de_nuke", "", "offline");
            batch(tracker, 140, "game", "competitive", "de_nuke", "[ 2 : 0 ]", "offline");
            std::optional<MatchSummary> left = batch(tracker, 145, "lobby", "", "", "", "");
            MatchSummary draw{ "competitive", "de_dust2", "offline", 12, 12, 2400 };
            MatchSummary casualWon{ "casual", "de_dust2", "offline", 8, 3, 1200 };
            MatchSummary casualLeft{ "casual", "de_dust2", "offline", 5, 3, 900 };
            MatchSummary dmFull{ "deathmatch", "de_dust2", "offline", 0, 0, 610 };
            MatchSummary dmLeft{ "deathmatch", "de_dust2", "offline", 0, 0, 300 };
            MatchSummary armsRace{ "gungameprogressive", "ar_shoots", "offline", 0, 0, 400 };
            CHECK(left && left->roundsWon == 2 && !MatchFinished(*left) && MatchFinished(draw) && MatchFinished(casualWon)
                    && !MatchFinished(casualLeft) && MatchFinished(dmFull) && !MatchFinished(dmLeft) && !MatchFinished(armsRace),
                "only finished matches pay: 2:0 left early no, 12:12 draw yes, casual 8:3 yes 5:3 no, deathmatch 10 min yes 5 no");
        }

        {
            // a community casual server changes the map: that match is over, the next one starts
            MatchTracker tracker;
            batch(tracker, 0, "game", "casual", "de_mirage", "", "community");
            batch(tracker, 600, "game", "casual", "de_mirage", "[ 4 : 2 ]", "community");
            std::optional<MatchSummary> mirage = batch(tracker, 900, "game", "casual", "de_inferno", "", "community");
            std::optional<MatchSummary> inferno = tracker.End(930);
            CHECK(mirage && mirage->map == "de_mirage" && mirage->roundsWon == 4 && mirage->seconds == 900
                    && MatchBaseXp(*mirage) == 150 && MatchFinished(*mirage) && !inferno,
                "community casual: the map change ends the match (15 min = 150 xp), 30 s on the next map count for nothing");
        }

        {
            // the score starts over on the same map (a new match), a demo, deathmatch and wingman rates
            MatchTracker tracker;
            batch(tracker, 0, "game", "scrimcomp2v2", "de_vertigo", "[ 9 : 4 ]", "offline");
            std::optional<MatchSummary> restarted = batch(tracker, 1200, "game", "scrimcomp2v2", "de_vertigo", "", "offline");
            MatchTracker demo;
            batch(demo, 0, "game", "competitive", "de_nuke", "[ 13 : 0 ]", "watch");
            MatchSummary deathmatch{ "deathmatch", "de_dust2", "offline", 0, 0, 600 };
            MatchSummary longCasual{ "casual", "de_dust2", "offline", 8, 7, 3600 };
            CHECK(restarted && restarted->roundsWon == 9 && MatchBaseXp(*restarted) == 135 && MatchFinished(*restarted) && tracker.InMatch()
                    && !demo.End(3000) && MatchBaseXp(deathmatch) == 150 && MatchBaseXp(longCasual) == 350,
                "wingman 9:4 = 135 xp when the score starts over; demos give nothing; deathmatch 10 min = 150, casual tops at 350");
        }

        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            fprintf(f, "\"items\"\n{\n}\n\"account\"\n{\n\t\"player_level\"\t\t\"3\"\n\t\"player_xp\"\t\t\"0\"\n}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        {
            Inventory inventory{ SteamId };
            InventoryChangeMessages first = inventory.AddMatchXp(390, "test");
            bool firstOk = inventory.WeeklyXpEarned() == 1560 && first.levelChanged && inventory.PlayerXp() == 1560;
            inventory.AddMatchXp(1000, "test"); // 735 at x4 fills 4500, the other 265 at x2
            bool secondOk = inventory.WeeklyXpEarned() == 5030 && inventory.PlayerLevel() == 4;
            inventory.AddMatchXp(100000, "test"); // x2 to 7500, x1 to 11167, the rest at x0.175
            CHECK(firstOk && secondOk && inventory.WeeklyXpEarned() == 27809,
                "weekly bonus: 390 -> 1560 (x4), 1000 -> 3470 (x4 then x2), past 11167 only x0.175 (%u this week)",
                inventory.WeeklyXpEarned());
        }
        {
            Inventory reloaded{ SteamId };
            CHECK(reloaded.WeeklyXpEarned() == 27809, "the week's match xp is kept in inventory.txt");
        }
    }

    printf("[CSForge: merge]\n");
    {
        auto parseFile = [](const std::string &name, std::string_view text) {
            std::string path = Platform::DataPath(name);
            FILE *f = Platform::OpenFile(path, "wb");
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            return std::make_unique<KeyValueRead>(path.c_str());
        };

        using ModLoader::MergeMode;
        ModLoader::MergeReport report;
        std::string error;

        std::string base = "\"r\"\n{\n\t\"a\"\n\t{\n\t\t\"1\" \"x\" // comment {\n\t}\n\t\"b\" \"v\"\n\t\"a\"\n\t{\n\t\t\"9\" \"second block\"\n\t}\n}\n";
        bool merged = ModLoader::MergeSections(base, "\"r\" { \"A\" { \"2\" \"y\" \"9\" \"dup\" \"1\" \"dup\" } \"c\" { \"3\" \"z\" } }", MergeMode::SkipExisting, report, error);
        auto smallKv = parseFile("merge_small.txt", base);
        const KeyValue *root = smallKv->Good() ? smallKv->Root().GetSubkey("r") : nullptr;
        const KeyValue *a = root ? root->GetSubkey("a") : nullptr;
        const KeyValue *c = root ? root->GetSubkey("c") : nullptr;
        CHECK(merged && a && a->GetString("1") == "x" && a->GetString("2") == "y" && c && c->GetString("3") == "z"
                && report.added == 2 && report.skipped.size() == 2 && base.find("dup") == std::string::npos,
            "merge: new keys appended (case insensitive), keys of any same-named block skipped (%zu added, %zu skipped) %s",
            report.added, report.skipped.size(), error.c_str());

        std::string tokens = "\"lang\" { \"Tokens\" { \"Old\" \"old text\" \"Keep\" \"keep\" } }";
        report = {};
        merged = ModLoader::MergeSections(tokens, "\"lang\" { \"Tokens\" { \"old\" \"new text\" \"New\" \"added\" } }", MergeMode::OverrideExisting, report, error);
        CHECK(merged && tokens.find("\"Old\" \"new text\"") != std::string::npos && tokens.find("old text") == std::string::npos
                && tokens.find("\"New\" \"added\"") != std::string::npos && tokens.find("\"Keep\" \"keep\"") != std::string::npos,
            "merge: localization overrides existing tokens in place, adds new ones (%zu overridden) %s", report.overridden.size(), error.c_str());

        std::vector<char> data;
        bool read = VpkReadFile(Platform::GamePath("pak01_dir.vpk"), "scripts/items/items_game.txt", data);
        std::string itemsGame{ data.begin(), data.end() };
        size_t before = itemsGame.size();
        report = {};
        bool itemsMerged = read && ModLoader::MergeSections(itemsGame,
            "// comment with \"quotes\" and { braces\n\"items_game\"\n{\n\t\"paint_kits\"\n\t{\n"
            "\t\t\"20001\" { \"name\" \"cs2gc_test\" }\n\t\t\"44\" { \"name\" \"overwrite_attempt\" }\n\t}\n}\n",
            MergeMode::SkipExisting, report, error);
        auto parsed = parseFile("merge_items_game.txt", itemsGame);
        const KeyValue *game = parsed->Good() ? parsed->Root().GetSubkey("items_game") : nullptr;
        const KeyValue *paintKits = game ? game->GetSubkey("paint_kits") : nullptr;
        const KeyValue *kit = paintKits ? paintKits->GetSubkey("20001") : nullptr;
        const KeyValue *oiled = paintKits ? paintKits->GetSubkey("44") : nullptr;
        CHECK(itemsMerged && kit && kit->GetString("name") == "cs2gc_test" && oiled && oiled->GetString("name") == "aq_oiled"
                && report.skipped.size() == 1 && itemsGame.find("overwrite_attempt") == std::string::npos,
            "items_game.txt: new paint kit added, existing kit 44 not touched (+%zu bytes) %s", itemsGame.size() - before, error.c_str());

        // the client takes the first paint kit ("0", name "default") as the template for the rest
        // and crashes if a custom one comes before it
        size_t kitsAt = itemsGame.find("\"paint_kits\"");
        size_t defaultAt = itemsGame.find("\"default\"", kitsAt);
        size_t customAt = itemsGame.find("\"cs2gc_test\"", kitsAt);
        CHECK(kitsAt != std::string::npos && defaultAt < customAt && customAt != std::string::npos,
            "custom paint kit goes after paint kit 0 (default at %zu, custom at %zu)", defaultAt, customAt);
    }

    printf("[CSForge: build]\n");
    {
        // three mods: low (priority 0), high (priority 10), off (disabled)
        auto write = [](const std::string &name, std::string_view text) {
            std::string path = Platform::DataPath(name);
            std::filesystem::create_directories(std::filesystem::path{ path }.parent_path());
            FILE *f = Platform::OpenFile(path, "wb");
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        };

        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{ Platform::DataPath("mods") }, ec);

        write("mods\\low\\mod.txt", "\"mod\" { \"name\" \"Low\" \"priority\" \"0\" }");
        write("mods\\low\\files\\sounds\\cs2gc_test\\Shared.vsnd_c", "low");
        write("mods\\low\\files\\sounds\\cs2gc_test\\only_low.vsnd_c", std::string(70000, 'L'));
        write("mods\\low\\items_game.txt", "\"items_game\" { \"paint_kits\" { \"20001\" { \"name\" \"low_kit\" } } \"paint_kits_rarity\" { \"low_kit\" \"ancient\" } }");
        write("mods\\low\\localization.txt", "\"lang\" { \"Tokens\" { \"PaintKit_aq_oiled_Tag\" \"Low override\" \"cs2gc_mod_token\" \"cs2gc low marker\" } }");

        write("mods\\high\\mod.txt", "\"mod\" { \"name\" \"High\" \"priority\" \"10\" \"version\" \"1.2\" }");
        write("mods\\high\\files\\sounds\\cs2gc_test\\shared.vsnd_c", "high");
        write("mods\\high\\items_game.txt", "\"items_game\" { \"paint_kits\" { \"20001\" { \"name\" \"high_dup\" } \"20002\" { \"name\" \"high_kit\" } } }");
        write("mods\\high\\localization_russian.txt", "\"lang\" { \"Tokens\" { \"cs2gc_mod_token\" \"cs2gc high marker\" } }");

        write("mods\\off\\mod.txt", "\"mod\" { \"name\" \"Off\" \"enabled\" \"0\" \"priority\" \"99\" }");
        write("mods\\off\\files\\sounds\\cs2gc_test\\shared.vsnd_c", "off");

        std::vector<ModLoader::ModInfo> mods = ModLoader::ListMods();
        CHECK(mods.size() == 3 && mods[0].folder == "low" && mods[1].folder == "high" && mods[2].folder == "off" && !mods[2].enabled
                && mods[1].version == "1.2",
            "3 mods listed in load order low, high, off (disabled)");

        bool built = ModLoader::Build();
        const std::string &vpk = ModLoader::BuiltVpkPath();

        std::vector<char> shared, onlyLow, items, russian, english;
        VpkReadFile(vpk, "sounds/cs2gc_test/shared.vsnd_c", shared);
        VpkReadFile(vpk, "sounds/cs2gc_test/only_low.vsnd_c", onlyLow);
        VpkReadFile(vpk, "scripts/items/items_game.txt", items);
        VpkReadFile(vpk, "resource/csgo_russian.txt", russian);
        VpkReadFile(vpk, "resource/csgo_english.txt", english);

        std::string_view itemsText{ items.data(), items.size() };
        std::string_view russianText{ russian.data(), russian.size() };
        std::string_view englishText{ english.data(), english.size() };

        CHECK(built && std::string_view(shared.data(), shared.size()) == "high" && onlyLow.size() == 70000,
            "files: the higher priority mod wins a shared path (case insensitive), the disabled one is ignored");
        CHECK(itemsText.find("\"low_kit\"") != std::string_view::npos && itemsText.find("\"high_kit\"") != std::string_view::npos
                && itemsText.find("high_dup") == std::string_view::npos,
            "items_game.txt: both mods' kits merged, the second 20001 skipped");
        CHECK(russianText.find("\"cs2gc high marker\"") != std::string_view::npos && russianText.find("Low override") != std::string_view::npos
                && englishText.find("\"cs2gc low marker\"") != std::string_view::npos && englishText.find("cs2gc high marker") == std::string_view::npos,
            "localization: low's tokens in every language, high's localization_russian.txt on top of russian only (%d%d%d%d, %zu/%zu bytes)",
            russianText.find("\"cs2gc high marker\"") != std::string_view::npos, russianText.find("Low override") != std::string_view::npos,
            englishText.find("\"cs2gc low marker\"") != std::string_view::npos, englishText.find("cs2gc high marker") == std::string_view::npos,
            russianText.size(), englishText.size());

        // the gc's item schema reads the same items_game.txt as the client
        ItemSchema modSchema;
        ItemDesc desc{};
        CHECK(modSchema.GetItemDesc(ToEnum<ItemDefIndex>(7u), ItemOriginBaseItem, UnacknowledgedInvalid, desc),
            "gc item schema loads from the mods vpk");

        // the inventory editor's Mods tab
        auto status = [](const std::vector<ModLoader::ModStatus> &statuses, std::string_view folder) {
            auto it = std::find_if(statuses.begin(), statuses.end(), [&](const ModLoader::ModStatus &s) { return s.info.folder == folder; });
            return it != statuses.end() ? *it : ModLoader::ModStatus{};
        };
        std::vector<ModLoader::ModStatus> statuses = ModLoader::ModStatuses();
        ModLoader::ModStatus low = status(statuses, "low");
        CHECK(statuses.size() == 3 && low.files == 2 && low.items && low.localization && low.lostFiles == 1
                && low.lost.size() == 1 && low.lost[0].second == "high" && !low.changed && !status(statuses, "high").changed,
            "Mods tab: files, items/texts, low's shared.vsnd_c lost to high, nothing changed since the build");

        write("mods\\low\\mod.txt", "// keep me\n\"mod\"\n{\n\t\"name\"\t\"Low\"\n\t\"priority\"\t\"0\"\n}\n");
        std::string error;
        bool saved = ModLoader::SetModState("low", false, 7, error);
        std::string lowText;
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("mods\\low\\mod.txt"), "rb");
            char buffer[512]{};
            lowText.assign(buffer, f ? fread(buffer, 1, sizeof(buffer), f) : 0);
            if (f)
            {
                fclose(f);
            }
        }
        statuses = ModLoader::ModStatuses();
        CHECK(saved && lowText.starts_with("// keep me") && lowText.find("\"priority\"\t\"7\"") != std::string::npos
                && lowText.find("\"enabled\"\t\"0\"") != std::string::npos && status(statuses, "low").changed
                && !status(statuses, "low").info.enabled && status(statuses, "high").lostFiles == 0,
            "Mods tab: low switched off with priority 7 in mod.txt (comment kept), marked changed");
        CHECK(!ModLoader::SetModState("..\\..\\evil", true, 0, error) && !ModLoader::SetModState("_build", true, 0, error),
            "Mods tab: only real mod folders can be written (%s)", error.c_str());

        std::filesystem::remove_all(std::filesystem::path{ Platform::DataPath("mods") }, ec);
    }

    printf("[CSForge: Panorama fixes]\n");
    {
        constexpr std::string_view Script = "panorama/scripts/popups/popup_capability_decodable.vts_c";
        std::string gameVpk = Platform::GamePath("pak01_dir.vpk");
        std::vector<char> original;
        bool read = VpkReadFile(gameVpk, Script, original);
        size_t targets = ModLoader::BuildUpgraderCatalog(nullptr); // by rarity: no download in a test
        std::vector<VpkFile> out;
        size_t applied = ModLoader::ApplyPanoramaFixes(gameVpk, out);
        auto fixed = [&](std::string_view path) -> const VpkFile * {
            for (const VpkFile &file : out)
            {
                if (file.path == path)
                {
                    return &file;
                }
            }
            return nullptr;
        };

        // the rebuilt resource: size field, the block table, DATA = the old text with the code in, STAT as it was
        auto blocks = [](std::string_view file) {
            std::map<std::string, std::string_view> result;
            uint32_t size = 0, table = 0, count = 0;
            memcpy(&size, file.data(), 4);
            memcpy(&table, file.data() + 8, 4);
            memcpy(&count, file.data() + 12, 4);
            if (size != file.size())
            {
                return result;
            }
            for (uint32_t i = 0; i < count; i++)
            {
                size_t entry = 8 + table + i * 12;
                uint32_t offset = 0, length = 0;
                memcpy(&offset, file.data() + entry + 4, 4);
                memcpy(&length, file.data() + entry + 8, 4);
                if (entry + 4 + offset + length > file.size())
                {
                    return std::map<std::string, std::string_view>{};
                }
                result[std::string{ file.substr(entry, 4) }] = file.substr(entry + 4 + offset, length);
            }
            return result;
        };
        std::string_view originalFile{ original.data(), original.size() };
        auto before = blocks(originalFile);
        std::map<std::string, std::string_view> after;
        if (fixed(Script))
        {
            after = blocks(fixed(Script)->data);
        }
        std::string_view newText = after.contains("DATA") ? after["DATA"] : std::string_view{};
        std::string_view oldText = before.contains("DATA") ? before["DATA"] : std::string_view{};
        // the texts match up to the inserted code and from its end on
        size_t insertedAt = 0;
        while (insertedAt < oldText.size() && insertedAt < newText.size() && oldText[insertedAt] == newText[insertedAt])
        {
            insertedAt++;
        }
        bool sameAround = newText.size() > oldText.size() && newText.ends_with(oldText.substr(insertedAt))
            && newText.substr(insertedAt, newText.size() - oldText.size()).find("csgcInMatch") != std::string_view::npos;
        CHECK(read && applied == 3 && out.size() == 3 && fixed(Script) && after.size() == before.size()
                && after.contains("STAT") && after["STAT"] == before["STAT"] && after["RED2"] == before["RED2"] && sameAround
                && newText.find("#SFUI_InvError_OpenCrate_PauseMenu") != std::string_view::npos,
            "case popup: Close and the in-match message go into the game's current script, the rest of the resource intact");

        std::string copy{ originalFile };
        std::string error;
        CHECK(!ModLoader::PatchResourceText(copy, "no such anchor in the script", "x", error) && copy == originalFile,
            "an anchor the game's script doesn't have leaves it alone (%s)", error.c_str());

        // the upgrader: the menu entry and the window, with the catalog in; the scripts go to
        // <data>\panorama_fix_*.js so node --check can look at them
        auto dataText = [&](std::string_view path) {
            const VpkFile *file = fixed(path);
            if (!file)
            {
                return std::string{};
            }
            auto b = blocks(file->data);
            return b.contains("DATA") ? std::string{ b["DATA"] } : std::string{};
        };
        std::string menu = dataText("panorama/scripts/common/item_context_entries.vts_c");
        std::string inventoryScript = dataText("panorama/scripts/mainmenu_inventory.vts_c");
        // the module alone too, for a run with fake Panorama APIs
        size_t moduleStart = inventoryScript.find("var CsgcUpgrader");
        size_t moduleEnd = inventoryScript.find("var InventoryPanel;");
        std::string window = (moduleStart != std::string::npos && moduleEnd != std::string::npos && moduleEnd > moduleStart)
            ? inventoryScript.substr(moduleStart, moduleEnd - moduleStart) : std::string{};
        for (const auto &[name, text] : { std::pair{ "menu", &menu }, std::pair{ "window", &window }, std::pair{ "inventory", &inventoryScript } })
        {
            FILE *f = Platform::OpenFile(Platform::DataPath(std::format("panorama_fix_{}.js", name)), "wb");
            if (f)
            {
                fwrite(text->data(), 1, text->size(), f);
                fclose(f);
            }
        }
        std::span<const ModLoader::UpgradeTarget> catalog = ModLoader::UpgraderCatalog();
        size_t knives = 0, bad = 0;
        for (const ModLoader::UpgradeTarget &target : catalog)
        {
            knives += target.tier == 7 && target.defIndex >= 500 && target.defIndex < 1000;
            // made up from the rarity: 10^tier in every wear the finish comes in
            bool anyWear = false;
            for (uint64_t price : target.price)
            {
                bad += price && price != static_cast<uint64_t>(std::pow(10.0, target.tier));
                anyWear |= price != 0;
            }
            bad += target.tier < 1 || target.tier > 7 || !anyWear;
        }
        CHECK(targets > 1500 && targets == catalog.size() && knives > 300 && !bad
                && menu.find("name: 'csgc_upgrade'") != std::string::npos && menu.find("name: 'tradeup_add'") != std::string::npos
                && window.find("var CsgcUpgrader") != std::string::npos && window.find("@CATALOG@") == std::string::npos
                && window.find(std::format("{},{},{},", catalog[0].defIndex, catalog[0].paintKit, catalog[0].tier)) != std::string::npos
                && window.find("@CURRENCY@") == std::string::npos && window.find("@PRICE_TIME@") == std::string::npos
                && inventoryScript.find("CsgcUpgrader.Install($.GetContextPanel(), _m_elInventoryMain, NavigateToTab);") != std::string::npos
                && inventoryScript.find("_InitMarketLink();") != std::string::npos,
            "upgrader: %zu targets (%zu knives), Upgrade in the skin menu, the Upgrader tab in the inventory script",
            targets, knives);
    }

    printf("[upgrader prices]\n");
    {
        // market.csgo.com's list: ★ written as ★, ™ as UTF-8, a price with 3 decimals, one not on sale
        std::string json = "{\"success\":true,\"time\":1794140264,\"currency\":\"RUB\",\"items\":["
            "{\"market_hash_name\":\"AK-47 | Case Hardened (Field-Tested)\",\"volume\":10,\"price\":\"50.00\"},"
            "{\"market_hash_name\":\"AK-47 | Case Hardened (Factory New)\",\"volume\":3,\"price\":\"60.00\"},"
            "{\"market_hash_name\":\"StatTrak\xE2\x84\xA2 AK-47 | Case Hardened (Factory New)\",\"volume\":1,\"price\":\"80.004\"},"
            "{\"market_hash_name\":\"AK-47 | Fire Serpent (Factory New)\",\"volume\":1,\"price\":\"2000.00\"},"
            "{\"market_hash_name\":\"AK-47 | Redline (Field-Tested)\",\"volume\":400,\"price\":\"100.00\"},"
            "{\"market_hash_name\":\"AK-47 | Redline (Factory New)\",\"volume\":0,\"price\":\"999.00\"},"
            "{\"market_hash_name\":\"StatTrak\xE2\x84\xA2 AK-47 | Redline (Field-Tested)\",\"volume\":90,\"price\":\"300.00\"},"
            "{\"market_hash_name\":\"\\u2605 Karambit | Fade (Factory New)\",\"volume\":5,\"price\":\"80000.00\"},"
            "{\"market_hash_name\":\"\\u2605 StatTrak\\u2122 Karambit | Fade (Factory New)\",\"volume\":1,\"price\":\"160000\"},"
            "{\"market_hash_name\":\"Sticker | Nothing } here\",\"volume\":1,\"price\":\"0\"}]}";
        SkinPrices::PriceList list;
        std::string error;
        bool parsed = list.Parse(json, error);
        CHECK(parsed && list.Currency() == "RUB" && list.Time() == 1794140264 && list.Size() == 9
                && list.Find("AK-47 | Redline (Field-Tested)") == 10000
                && list.Find(SkinPrices::MarketHashName("AK-47", "Case Hardened", 0, false, true, false)) == 8000
                && list.Find(SkinPrices::MarketHashName("Karambit", "Fade", 0, true, true, false)) == 16000000,
            "the price list: currency, time, kopecks, \\u escapes and UTF-8 names (%zu prices, %s)", list.Size(), error.c_str());
        // a list made an hour ago is fresh for 12 hours: read from the data dir, nothing downloaded
        {
            std::string fresh = json;
            fresh.replace(fresh.find("1794140264"), 10, std::to_string(time(nullptr) - 3600));
            fresh.replace(fresh.find("\"RUB\""), 5, "\"TST\"");
            FILE *f = Platform::OpenFile(Platform::DataPath("prices_TST.json"), "wb");
            fwrite(fresh.data(), 1, fresh.size(), f);
            fclose(f);
            SkinPrices::PriceList cached;
            CHECK(SkinPrices::LoadPriceList("TST", 12, cached) && cached.Currency() == "TST" && cached.Size() == 9,
                "a cached list an hour old is used as it is (%zu prices)", cached.Size());
        }
        CHECK(SkinPrices::WearOf(0.0f) == 0 && SkinPrices::WearOf(0.07f) == 1 && SkinPrices::WearOf(0.2f) == 2
                && SkinPrices::WearOf(0.449f) == 3 && SkinPrices::WearOf(1.0f) == 4
                && !SkinPrices::WearPossible(0, 0.1f, 0.7f) && SkinPrices::WearPossible(4, 0.1f, 0.7f),
            "wears by float: 0.07 is Minimal Wear, a 0.1-0.7 finish has no Factory New");

        size_t skins = ModLoader::BuildUpgraderCatalog(&list);
        std::span<const ModLoader::UpgradeTarget> catalog = ModLoader::UpgraderCatalog();
        auto find = [&](uint32_t def, uint32_t paint) -> const ModLoader::UpgradeTarget * {
            for (const ModLoader::UpgradeTarget &target : catalog)
            {
                if (target.defIndex == def && target.paintKit == paint)
                {
                    return &target;
                }
            }
            return nullptr;
        };
        const ModLoader::UpgradeTarget *redline = find(7, 282);
        const ModLoader::UpgradeTarget *fade = find(507, 38);
        CHECK(skins > 1500 && ModLoader::UpgraderCurrency() == "RUB" && redline && fade
                && redline->price[2] == 10000 && redline->statTrak[2] == 30000 && redline->price[0] == 0 && redline->price[1] == 0
                && fade->price[0] == 8000000 && fade->statTrak[0] == 16000000 && fade->souvenir[0] == 0,
            "the catalog: English names from the game, Redline FT 100 (ST 300), no Factory New for it (0.1-0.7); Karambit Fade FN 80000");

        // 1-4 AK-47 Case Hardened FT (50), 5 one in Battle-Scarred (not on sale), 11 StatTrak FN (80),
        // 12 Fire Serpent FN (2000), 13 a rental, 14 Case Hardened FN with the star from the editor (60)
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            auto item = [&](int id, int def, int quality, int rarity, const char *attributes) {
                fprintf(f, "\t\"%d\"\n\t{\n\t\t\"inventory\"\t\t\"%d\"\n\t\t\"def_index\"\t\t\"%d\"\n\t\t\"level\"\t\t\"1\"\n"
                           "\t\t\"quality\"\t\t\"%d\"\n\t\t\"flags\"\t\t\"0\"\n\t\t\"origin\"\t\t\"8\"\n\t\t\"in_use\"\t\t\"0\"\n"
                           "\t\t\"rarity\"\t\t\"%d\"\n\t\t\"attributes\"\n\t\t{\n%s\t\t}\n\t}\n",
                    id, id, def, quality, rarity, attributes);
            };
            fprintf(f, "\"items\"\n{\n");
            for (int i = 1; i <= 4; i++)
            {
                item(i, 7, 4, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"8\"\t\t\"0.2\"\n");
            }
            item(5, 7, 4, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"8\"\t\t\"0.9\"\n");
            item(11, 7, 9, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"80\"\t\t\"3\"\n\t\t\t\"81\"\t\t\"0\"\n");
            item(12, 7, 4, 6, "\t\t\t\"6\"\t\t\"180\"\n");
            item(13, 7, 4, 3, "\t\t\t\"6\"\t\t\"44\"\n\t\t\t\"8\"\t\t\"0.2\"\n\t\t\t\"297\"\t\t\"1\"\n");
            item(14, 7, 3, 3, "\t\t\t\"6\"\t\t\"44\"\n");
            fprintf(f, "}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        auto id = [&](uint32_t low) { return ComposeItemId(static_cast<uint32_t>(SteamId), low); };
        Inventory inventory{ SteamId };
        auto items = [&]() {
            std::map<uint64_t, CSOEconItem> result;
            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            for (const auto &object : cache.objects())
            {
                for (const std::string &data : object.object_data())
                {
                    CSOEconItem item;
                    if (object.type_id() == SOTypeItem && item.ParseFromString(data))
                    {
                        result[item.id()] = item;
                    }
                }
            }
            return result;
        };
        auto floatOf = [](const CSOEconItem &item, AttributeDefIndex attribute) {
            for (const CSOEconItemAttribute &a : item.attribute())
            {
                if (a.def_index() == FromEnum(attribute) && a.value_bytes().size() == 4)
                {
                    float value;
                    memcpy(&value, a.value_bytes().data(), 4);
                    return value;
                }
            }
            return -1.0f;
        };

        if (redline && fade)
        {
            auto offer = [](const ModLoader::UpgradeTarget &target, int wear, bool statTrak) {
                return Inventory::UpgradeOffer{ ToEnum<ItemDefIndex>(target.defIndex), target.paintKit, wear, statTrak,
                    statTrak ? target.statTrak[wear] : target.price[wear] };
            };
            const auto price = &ModLoader::UpgraderItemPrice;

            Inventory::UpgradeResult won;
            inventory.Upgrade(std::vector<uint64_t>{ id(1) }, offer(*redline, 2, false), price, won, 0.4f);
            auto now = items();
            float wear = now.contains(won.itemId) ? floatOf(now[won.itemId], AttributeDefIndex::TextureWear) : -1.0f;
            CHECK(won.done && won.won && std::abs(won.chance - 0.5) < 1e-9 && !now.contains(id(1)) && now.contains(won.itemId)
                    && now[won.itemId].def_index() == 7 && static_cast<uint32_t>(floatOf(now[won.itemId], AttributeDefIndex::TexturePrefab)) == 282
                    && wear >= 0.15f && wear < 0.38f && now[won.itemId].quality() == FromEnum(Quality::Unique),
                "50 for a Redline FT at 100: 50%%, won with a roll of 40%%, the new one Field-Tested (%.3f) (%s)", wear, won.error.c_str());

            size_t before = items().size();
            Inventory::UpgradeResult even, dearer, unpriced, rental, wrongWear;
            inventory.Upgrade(std::vector<uint64_t>{ id(2), id(3) }, offer(*redline, 2, false), price, even, 0.0f);
            inventory.Upgrade(std::vector<uint64_t>{ id(12) }, offer(*redline, 2, false), price, dearer, 0.0f);
            inventory.Upgrade(std::vector<uint64_t>{ id(5) }, offer(*redline, 2, false), price, unpriced, 0.0f);
            inventory.Upgrade(std::vector<uint64_t>{ id(13) }, offer(*redline, 2, false), price, rental, 0.0f);
            inventory.Upgrade(std::vector<uint64_t>{ id(2) }, Inventory::UpgradeOffer{ ToEnum<ItemDefIndex>(7), 282, 0, false, 99900 },
                price, wrongWear, 0.0f);
            CHECK(!even.done && !dearer.done && !unpriced.done && !rental.done && !wrongWear.done && items().size() == before,
                "refused, nothing burns: 50+50 for 100 (%s); a Fire Serpent at 2000 for 100 (%s); no price in Battle-Scarred (%s); "
                "a rental (%s); a Factory New Redline that can't be (%s)",
                even.error.c_str(), dearer.error.c_str(), unpriced.error.c_str(), rental.error.c_str(), wrongWear.error.c_str());

            Inventory::UpgradeResult lost;
            inventory.Upgrade(std::vector<uint64_t>{ id(2), id(3) }, offer(*redline, 2, true), price, lost, 0.5f);
            now = items();
            CHECK(lost.done && !lost.won && std::abs(lost.chance - 1.0 / 3.0) < 1e-9 && !now.contains(id(2)) && !now.contains(id(3)) && !lost.itemId,
                "50+50 for a StatTrak Redline FT at 300: 33.3%%, a roll of 50%% burns both");

            Inventory::UpgradeResult starred;
            inventory.Upgrade(std::vector<uint64_t>{ id(14) }, offer(*redline, 2, false), price, starred, 0.99f);
            CHECK(starred.done && std::abs(starred.chance - 0.6) < 1e-9,
                "a Case Hardened with the star from the editor counts at its price (FN, 60): 60%% for 100 (%s)", starred.error.c_str());

            Inventory::UpgradeResult gold;
            inventory.Upgrade(std::vector<uint64_t>{ id(11) }, offer(*fade, 0, true), price, gold, 0.0001f);
            now = items();
            bool hasKills = false;
            for (const CSOEconItemAttribute &a : now[gold.itemId].attribute())
            {
                hasKills |= a.def_index() == FromEnum(AttributeDefIndex::KillEater);
            }
            wear = now.contains(gold.itemId) ? floatOf(now[gold.itemId], AttributeDefIndex::TextureWear) : -1.0f;
            CHECK(gold.won && std::abs(gold.chance - 8000.0 / 16000000.0) < 1e-12 && now[gold.itemId].quality() == FromEnum(Quality::Unusual)
                    && hasKills && wear >= 0.0f && wear < 0.07f,
                "a StatTrak Case Hardened FN (80) for a StatTrak Karambit Fade FN (160000): 0.05%%, the knife comes StatTrak, unusual, Factory New (%s)",
                gold.error.c_str());
        }
        else
        {
            CHECK(false, "upgrader: no Redline or Karambit Fade in the catalog");
        }

        // the real list, when one is put in the data dir: how many skins the names find
        std::string realJson;
        if (FILE *f = Platform::OpenFile(Platform::DataPath("prices_real.json"), "rb"))
        {
            char buffer[65536];
            size_t read;
            while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
            {
                realJson.append(buffer, read);
            }
            fclose(f);
            SkinPrices::PriceList real;
            if (real.Parse(realJson, error))
            {
                ModLoader::BuildUpgraderCatalog(&real);
                size_t priced = 0, knivesPriced = 0, glovesPriced = 0, knivesAll = 0, glovesAll = 0;
                for (const ModLoader::UpgradeTarget &target : ModLoader::UpgraderCatalog())
                {
                    bool any = false;
                    for (uint64_t p : target.price)
                    {
                        any |= p != 0;
                    }
                    priced += any;
                    bool knife = target.defIndex >= 500 && target.defIndex < 1000;
                    bool gloves = target.defIndex >= 4700;
                    knivesAll += knife;
                    glovesAll += gloves;
                    knivesPriced += knife && any;
                    glovesPriced += gloves && any;
                    if (!any)
                    {
                        printf("    no price: def %u paint %u\n", target.defIndex, target.paintKit);
                    }
                }
                printf("  info real list (%zu prices): %zu of %zu skins priced, knives %zu/%zu, gloves %zu/%zu\n", real.Size(), priced,
                    ModLoader::UpgraderCatalog().size(), knivesPriced, knivesAll, glovesPriced, glovesAll);
            }
        }
    }

    printf("[service medal]\n");
    {
        // level 39, no medal yet; this week's care package already in (no drop on the level ups)
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            fprintf(f, "\"items\"\n{\n}\n\"account\"\n{\n\t\"player_level\"\t\t\"39\"\n\t\"player_xp\"\t\t\"100\"\n\t\"xp_week\"\t\t\"%u\"\n}\n",
                Inventory::XpWeekStart(static_cast<uint32_t>(time(nullptr))));
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        Inventory inventory{ SteamId };
        ItemSchema schema;
        auto items = [&]() {
            std::map<uint64_t, CSOEconItem> result;
            CMsgSOCacheSubscribed cache;
            inventory.BuildCacheSubscription(cache, 1, false);
            for (const auto &object : cache.objects())
            {
                for (const std::string &data : object.object_data())
                {
                    CSOEconItem item;
                    if (object.type_id() == SOTypeItem && item.ParseFromString(data))
                    {
                        result[item.id()] = item;
                    }
                }
            }
            return result;
        };
        auto nameOf = [&](ItemDefIndex def) { return std::string{ schema.ItemDefName(def) }; };

        std::optional<Inventory::PrestigeCoin> first = inventory.NextPrestigeCoin();
        uint64_t medalId = 0;
        std::string error;
        inventory.RedeemPrestigeCoin(first ? first->defIndex : ItemDefIndex{}, medalId, error);
        std::string firstName = first ? nameOf(first->defIndex) : "";
        CHECK(first && !first->upgradeId && firstName.starts_with("prestige coin ") && firstName.find("level") == std::string::npos
                && !medalId && !error.empty() && items().empty() && inventory.PlayerLevel() == 39,
            "level 39: the check names %s (%u) to get new, getting it is refused (%s)", firstName.c_str(),
            first ? FromEnum(first->defIndex) : 0u, error.c_str());

        inventory.AddXp(5000, "test");
        InventoryChangeMessages got = inventory.RedeemPrestigeCoin(first ? first->defIndex : ItemDefIndex{}, medalId, error = {});
        auto now = items();
        bool gotOk = error.empty() && medalId && now.contains(medalId) && first && now[medalId].def_index() == FromEnum(first->defIndex)
            && (now[medalId].inventory() & (1u << 30)) && now[medalId].origin() == ItemOriginEarned && got.levelChanged
            && inventory.PlayerLevel() == 1 && inventory.PlayerXp() == 0;
        CHECK(gotOk, "level 40: the medal comes new and unacknowledged (earned), the level starts over at 1 with 0 xp (%s)", error.c_str());

        // back to 40: the next one upgrades the same item to level 2
        inventory.AddXp(39 * 5000, "test");
        std::optional<Inventory::PrestigeCoin> second = inventory.NextPrestigeCoin();
        std::string secondName = second ? nameOf(second->defIndex) : "";
        uint64_t upgradedId = 0;
        inventory.RedeemPrestigeCoin(second ? second->defIndex : ItemDefIndex{}, upgradedId, error = {});
        now = items();
        CHECK(second && second->upgradeId == medalId && secondName == firstName + " level 2" && upgradedId == medalId
                && now.size() == 1 && now[medalId].def_index() == FromEnum(second->defIndex) && inventory.PlayerLevel() == 1,
            "level 40 again: the check names %s to upgrade the medal owned, the same item becomes it (%s)", secondName.c_str(), error.c_str());

        inventory.AddXp(39 * 5000, "test");
        uint64_t wrongId = 0;
        inventory.RedeemPrestigeCoin(first ? first->defIndex : ItemDefIndex{}, wrongId, error = {});
        CHECK(!wrongId && !error.empty() && inventory.PlayerLevel() == 40, "asking for another medal than the next one is refused (%s)", error.c_str());
    }

    printf("[StatTrak storage unit]\n");
    {
        // the editor could put a StatTrak counter on a storage unit, which crashes the client
        {
            FILE *f = Platform::OpenFile(Platform::DataPath("inventory.txt").c_str(), "wb");
            fprintf(f, "\"items\"\n{\n"
                       "\t\"1\"\n\t{\n\t\t\"inventory\"\t\t\"1\"\n\t\t\"def_index\"\t\t\"1201\"\n\t\t\"quality\"\t\t\"4\"\n\t\t\"rarity\"\t\t\"1\"\n"
                       "\t\t\"attributes\"\n\t\t{\n\t\t\t\"270\"\t\t\"0\"\n\t\t\t\"80\"\t\t\"80085\"\n\t\t\t\"81\"\t\t\"0\"\n\t\t}\n\t}\n"
                       "\t\"2\"\n\t{\n\t\t\"inventory\"\t\t\"2\"\n\t\t\"def_index\"\t\t\"5270\"\n\t\t\"quality\"\t\t\"4\"\n\t\t\"rarity\"\t\t\"1\"\n"
                       "\t\t\"attributes\"\n\t\t{\n\t\t\t\"80\"\t\t\"1337\"\n\t\t\t\"81\"\t\t\"0\"\n\t\t}\n\t}\n}\n");
            fclose(f);
        }

        constexpr uint64_t SteamId = 76561197960287930ull;
        Inventory inventory{ SteamId };
        CMsgSOCacheSubscribed cache;
        inventory.BuildCacheSubscription(cache, 1, false);
        int casketKills = -1, medalKills = -1;
        for (const auto &object : cache.objects())
        {
            for (const std::string &data : object.object_data())
            {
                CSOEconItem item;
                if (object.type_id() != SOTypeItem || !item.ParseFromString(data))
                {
                    continue;
                }
                int &kills = item.def_index() == 1201 ? casketKills : item.def_index() == 5270 ? medalKills : casketKills;
                kills = 0;
                for (const CSOEconItemAttribute &a : item.attribute())
                {
                    kills += a.def_index() == FromEnum(AttributeDefIndex::KillEater) ? 1 : 0;
                }
            }
        }
        CHECK(casketKills == 0 && medalKills == 1, "on load the storage unit loses its StatTrak counter, a medal keeps its own (%d, %d)", casketKills, medalKills);
    }

    printf("[SteamGameServer015 proxy]\n");
    {
        FakeGameServer::s_vtable[0] = reinterpret_cast<void *>(&FakeGameServer::InitGameServer);
        FakeGameServer::s_vtable[25] = reinterpret_cast<void *>(&FakeGameServer::GetAuthSessionTicket);

        // neither is touched on this path
        alignas(16) char dummy[64]{};
        SteamPipeContext context{ 1, true, *reinterpret_cast<ISteamClient *>(dummy), *reinterpret_cast<CCallbackMgr *>(dummy) };
        context.SetUser(1);

        void *original = &FakeGameServer::s_object;
        auto *server = static_cast<ISteamGameServer015 *>(context.GetInterface("SteamGameServer015", original));
        CHECK(server && server != original, "SteamGameServer015 gets proxied");

        if (server && server != original)
        {
            constexpr uint32 k_unServerFlagSecure = 2;
            server->InitGameServer(0, 27015, 27016, k_unServerFlagSecure | 1, 730, "1.0");
            CHECK(FakeGameServer::s_initFlags == 1, "InitGameServer (slot 0) reaches steam with the secure flag removed (flags %u)", FakeGameServer::s_initFlags);

            SteamNetworkingIdentity identity{};
            uint32 size = 0;
            char ticket[64];
            HAuthTicket handle = server->GetAuthSessionTicket(ticket, sizeof(ticket), &size, &identity);
            CHECK(handle == 1234 && FakeGameServer::s_ticketIdentity == &identity, "GetAuthSessionTicket (slot 25) forwards all 4 parameters");
        }
    }

    printf("\n%s (%d failures)\n", s_failures ? "FAILED" : "PASSED", s_failures);
    return s_failures ? 1 : 0;
}

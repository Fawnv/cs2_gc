#include "stdafx.h"
#include "gc_client.h"
#include "config.h"
#include "gc_message.h"
#include "graffiti.h"
#include "inventory_messages.h"
#include "keyvalue.h"
#include "match_tracker.h"
#include "mod_loader.h"
#include "networking_shared.h"
#include "random.h"
#include "skin_prices.h"

#include "base_gcmessages.pb.h"
#include "cstrike15_gcmessages.pb.h"
#include "econ_gcmessages.pb.h"
#include "gcsdk_gcmessages.pb.h"
#include "gcsystemmsgs.pb.h"

// FIXME: add to protos?
constexpr int k_EMsgGCRequestInventoryRefresh = 1050;

ClientGC::ClientGC(uint64_t steamId)
    : m_steamId{ steamId }
    , m_inventory{ steamId }
{
    // also called from ServerGC's constructor
    Graffiti::Initialize();

    StartThread();

    Platform::Print("ClientGC spawned for user {}\n", steamId);
}

ClientGC::~ClientGC()
{
    StopThread();

    // closing the game from the end of match scoreboard still counts
    std::optional<MatchSummary> match = m_matchTracker.End(static_cast<uint32_t>(time(nullptr)));
    if (match)
    {
        AwardMatchXp(*match, false);
    }

    Platform::Print("ClientGC destroyed\n");
}

void ClientGC::HandleEvent(GCEvent type, uint64_t id, std::vector<uint8_t> &&buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(id, buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::SetAuthTicket:
        SetAuthTicket(static_cast<uint32_t>(id), std::move(buffer));
        break;

    case GCEvent::ClearAuthTicket:
        ClearAuthTicket(static_cast<uint32_t>(id));
        break;

    case GCEvent::RichPresence:
        RichPresence(id, buffer);
        break;

    case GCEvent::Tick:
        InventoryUpdate();
        break;

    default:
        assert(false);
        break;
    }
}

void ClientGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
{
    GCMessageRead messageRead{ type, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCClientHello:
        case k_EMsgGCClientHelloPW:
        case k_EMsgGCClientHelloR2:
        case k_EMsgGCClientHelloR3:
        case k_EMsgGCClientHelloR4:
        case k_EMsgGCClientHelloPartner:
            OnClientHello(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingClient2GCHello:
            OnMatchmakingHello();
            break;

        case k_EMsgGCRequestInventoryRefresh:
            OnRequestInventoryRefresh();
            break;

        case k_EMsgGCAdjustEquipSlotsManual:
        case k_EMsgGCAdjustEquipSlotsShuffle:
            AdjustEquipSlots(messageRead);
            break;

        case k_EMsgGCOpenCrate:
            OpenCrate(messageRead);
            break;

        case k_EMsgGCCraft:
            Craft(messageRead);
            break;

        case k_EMsgGCDelete:
            DeleteItemProto(messageRead);
            break;

        case k_EMsgGCAcknowledgeRentalExpiration:
            // the client saw its rented items go away, nothing to do (expiry is ours)
            break;

        case k_EMsgGCCStrike15_v2_ClientPlayerDecalSign:
            ClientPlayerDecalSign(messageRead);
            break;

        case k_EMsgGCUseItemRequest:
            UseItemRequest(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestJoinServerData:
            ClientRequestJoinServerData(messageRead);
            break;

        case k_EMsgGCSetItemPositions:
            SetItemPositions(messageRead);
            break;

        case k_EMsgGCApplySticker:
            ApplySticker(messageRead);
            break;

        case k_EMsgGCStatTrakSwap:
            StatTrakSwap(messageRead);
            break;

        case k_EMsgGCNameItem:
        case k_EMsgGCNameBaseItem:
        case k_EMsgGCRemoveItemName:
            ModifyItemStringAttr(messageRead);
            break;

        case k_EMsgGCModifyItemAttribute:
            ModifyItemAttribute(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockRequest:
            EconPreviewDataBlockRequest(messageRead);
            break;

        case k_EMsgGCVolatileItemLoadContents:
            VolatileItemLoadContents(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_VolatileItemClaimReward:
            VolatileItemClaimReward(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRedeemFreeReward:
            RedeemFreeReward(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingStart:
        case k_EMsgGCCStrike15_v2_MatchmakingStop:
            // there's no matchmaking to start or stop offline
            break;

        case k_EMsgGCCStrike15_v2_RequestRecurringMissionSchedule:
            // weekly missions need Valve's mission templates (KV3 objects the client shows as
            // is) and the game server tracking them; without them the mission tile stays hidden
            break;

        case k_EMsgGCCStrike15_v2_ClientRedeemMissionReward:
            RedeemMissionReward(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin:
            RequestPrestigeCoin(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Client2GcAckXPShopTracks:
            AckXpShopTracks(messageRead);
            break;

        case k_EMsgGCStoreGetUserData:
            StoreGetUserData(messageRead);
            break;

        case k_EMsgGCStorePurchaseInit:
            StorePurchaseInit(messageRead);
            break;

        case k_EMsgGCStorePurchaseFinalize:
            StorePurchaseFinalize(messageRead);
            break;

        case k_EMsgGCCasketItemLoadContents:
            ProcessCasketItemLoadContents(messageRead);
            break;

        case k_EMsgGCCasketItemAdd:
            ProcessCasketItemAdd(messageRead);
            break;

        case k_EMsgGCCasketItemExtract:
            ProcessCasketItemExtract(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_GetEventFavorites_Request:
            OnGetEventFavorites(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_Party_Register:
            OnPartyRegister(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled protobuf message {}\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCDelete:
            DeleteItem(messageRead);
            break;

        case k_EMsgGCUnlockCrate_DEPRECATED:
            UnlockCrate(messageRead);
            break;

        case k_EMsgGCNameItem:
            NameItem(messageRead);
            break;

        case k_EMsgGCNameBaseItem:
            NameBaseItem(messageRead);
            break;

        case k_EMsgGCRemoveItemName:
            RemoveItemName(messageRead);
            break;

        case k_EMsgGCStatTrakSwap:
            StatTrakSwap(messageRead);
            break;

        case k_EMsgGCStorePurchaseFinalize:
            StorePurchaseFinalizeStruct(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled struct message {}\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
}

void ClientGC::HandleNetMessage(uint64_t steamId, const void *data, uint32_t size)
{
    // pass 0 as type so it gets parsed from the message
    GCMessageRead messageRead{ 0, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (!messageRead.IsProtobuf() && messageRead.TypeUnmasked() == k_EMsgNetworkConnect)
    {
        HandleConnect(steamId, messageRead);
        return;
    }

    if (!m_serverSteamId || steamId != m_serverSteamId)
    {
        Platform::Print("ClientGC: ignored message from {} (not our gs {})\n", steamId, m_serverSteamId);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            return;

        case k_EMsgGCCStrike15_v2_MatchEndRunRewardDrops:
            MatchEndRunRewardDrops(messageRead);
            return;
        }
    }

    Platform::Print("ClientGC::HandleNetMessage: unhandled protobuf message {}\n",
        MessageName(messageRead.TypeUnmasked()));
}

void ClientGC::SetAuthTicket(uint32_t handle, std::vector<uint8_t> &&buffer)
{
    AuthTicket &ticket = m_tickets[handle];
    ticket.steamId = 0;
    ticket.buffer = std::move(buffer);
}

void ClientGC::ClearAuthTicket(uint32_t handle)
{
    auto it = m_tickets.find(handle);
    if (it == m_tickets.end())
    {
        assert(false);
        Platform::Print("ClientGC: tried to clear a nonexistent auth ticket???\n");
        return;
    }

    if (it->second.steamId)
    {
        Platform::Print("ClientGC: closing p2p session with {}\n", it->second.steamId);
        PostToHost(HostEvent::CloseChannel, it->second.steamId, {});

        if (it->second.steamId == m_serverSteamId)
        {
            Platform::Print("ClientGC: clearing gs identity\n");
            m_serverSteamId = 0;

            // kind of cringe to store this in inventory...
            m_inventory.ResetGameServerCache();
        }
    }

    m_tickets.erase(it);
}

void ClientGC::RichPresence(uint64_t id, const std::vector<uint8_t> &buffer)
{
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    std::optional<MatchSummary> match;
    if (id == 1)
    {
        // ClearRichPresence
        match = m_matchTracker.End(now);
    }
    else
    {
        std::string_view text{ reinterpret_cast<const char *>(buffer.data()), buffer.size() };
        size_t separator = text.find('\0');
        if (separator == std::string_view::npos)
        {
            return;
        }
        match = m_matchTracker.Update(text.substr(0, separator), text.substr(separator + 1), now);
    }

    if (match)
    {
        AwardMatchXp(*match, true);
    }
}

void ClientGC::AwardMatchXp(const MatchSummary &match, bool send)
{
    if (m_matchReported)
    {
        // the server's end of match report already paid for this one
        m_matchReported = false;
        return;
    }

    if (!MatchFinished(match))
    {
        Platform::Print("Match: left before the end ({} {}:{}), no xp\n", match.mode, match.roundsWon, match.roundsLost);
        return;
    }

    uint32_t baseXp = MatchBaseXp(match);
    if (!baseXp)
    {
        return;
    }

    std::string reason = std::format("{} on {}, {}:{} in {} min", match.mode, match.map, match.roundsWon,
        match.roundsLost, match.seconds / 60);
    InventoryChangeMessages messages = m_inventory.AddMatchXp(baseXp, reason);
    if (send)
    {
        SendInventoryChangeMessages(messages);
    }
}

void ClientGC::HandleConnect(uint64_t steamId, GCMessageRead &message)
{
    uint32_t ticketSize = message.ReadUint32();
    const void *ticket = message.ReadData(ticketSize);
    if (!message.IsValid())
    {
        Platform::Print("ClientGC: ignored connection from {} (malformed message)\n", steamId);
        return;
    }

    for (auto &[handle, authTicket] : m_tickets)
    {
        if (authTicket.buffer.size() == ticketSize && !memcmp(authTicket.buffer.data(), ticket, ticketSize))
        {
            authTicket.steamId = steamId;
            m_serverSteamId = steamId;
            Platform::Print("ClientGC: sending socache to {}\n", steamId);
            HandleSOCacheRequest();
            return;
        }
    }

    Platform::Print("ClientGC: ignored connection from {} (ticket mismatch)\n", steamId);
}

void ClientGC::HandleSOCacheRequest()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().m_level, true);

    GCMessageWrite messageWrite{ k_ESOMsg_CacheSubscribed, message };
    PostToHost(HostEvent::NetMessage, m_serverSteamId, std::move(messageWrite).TakeBuffer());
}

void ClientGC::InventoryUpdate()
{
    InventoryChangeMessages changeMessages;
    if (m_inventory.Update(changeMessages))
    {
        SendInventoryChangeMessages(changeMessages);
    }
}

void ClientGC::SendMessageToGame(bool server, uint32_t type, const google::protobuf::MessageLite &message, uint64_t jobId)
{
    GCMessageWrite messageWrite{ type, message, jobId };

    if (server)
    {
        if (m_serverSteamId)
        {
            PostToHost(HostEvent::NetMessage, m_serverSteamId, std::move(messageWrite).TakeBuffer());
        }
    }
    else
    {
        uint32_t typeMasked = messageWrite.TypeMasked();
        PostToHost(HostEvent::Message, typeMasked, std::move(messageWrite).TakeBuffer());
    }
}

void ClientGC::SendSOCacheMessages(const SOCacheMessages &messages, bool server)
{
    for (const CMsgSOSingleObject &destroyed : messages.destroyed)
    {
        SendMessageToGame(server, k_ESOMsg_Destroy, destroyed);
    }

    for (const CMsgSOSingleObject &created : messages.created)
    {
        SendMessageToGame(server, k_ESOMsg_Create, created);
    }

    if (messages.updated.objects_modified_size())
    {
        SendMessageToGame(server, k_ESOMsg_UpdateMultiple, messages.updated);
    }
}

void ClientGC::SendInventoryChangeMessages(const InventoryChangeMessages &result)
{
    SendSOCacheMessages(result.client, false);
    SendSOCacheMessages(result.gameServer, true);

    if (result.notification.has_request())
    {
        // never sent to the gameserver
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, result.notification);
    }

    if (result.levelChanged)
    {
        // the level and xp bar come from the matchmaking hello
        OnMatchmakingHello();
    }
}

constexpr uint32_t MakeAddress(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
    return v4 | (v3 << 8) | (v2 << 16) | (v1 << 24);
}

static void BuildCSWelcome(CMsgCStrike15Welcome &message)
{
    // mikkotodo cleanup dox
    message.set_store_item_hash(136617352);
    message.set_timeplayedconsecutively(0);
    message.set_time_first_played(1329845773);
    message.set_last_time_played(1680260376);
    message.set_last_ip_address(MakeAddress(127, 0, 0, 1));
}

void ClientGC::BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message)
{
    message.set_account_id(AccountId());

    // this is the state of csgo matchmaking in 2024
    message.mutable_global_stats()->set_players_online(0);
    message.mutable_global_stats()->set_servers_online(0);
    message.mutable_global_stats()->set_players_searching(0);
    message.mutable_global_stats()->set_servers_available(0);
    message.mutable_global_stats()->set_ongoing_matches(0);
    message.mutable_global_stats()->set_search_time_avg(0);

    // don't write search_statistics

    message.mutable_global_stats()->set_main_post_url("");

    // bullshit
    message.mutable_global_stats()->set_required_appid_version(13857);
    message.mutable_global_stats()->set_pricesheet_version(1680057676); // mikkotodo revisit
    message.mutable_global_stats()->set_twitch_streams_version(2);

    // NOTE: on 2013 this is actually game_monthly_uniques!!! probably not read there so keep the event id
    message.mutable_global_stats()->set_active_tournament_eventid(20);

    message.mutable_global_stats()->set_active_survey_id(0);
    message.mutable_global_stats()->set_required_appid_version2(13862); // csgo s2

    message.set_vac_banned(GetConfig().m_vacBanned);

    // set competitive rank and wins here for old builds
    message.mutable_ranking()->set_account_id(AccountId());
    message.mutable_ranking()->set_rank_id(GetConfig().m_competitiveRank);
    message.mutable_ranking()->set_wins(GetConfig().m_competitiveWins);
    message.mutable_ranking()->set_rank_if_win(GetConfig().m_competitiveRank);
    message.mutable_ranking()->set_rank_if_lose(GetConfig().m_competitiveRank);
    message.mutable_ranking()->set_rank_if_tie(GetConfig().m_competitiveRank);

    message.mutable_commendation()->set_cmd_friendly(GetConfig().m_commendedFriendly);
    message.mutable_commendation()->set_cmd_teaching(GetConfig().m_commendedTeaching);
    message.mutable_commendation()->set_cmd_leader(GetConfig().m_commendedLeader);
    message.set_player_level(m_inventory.PlayerLevel());
    message.set_player_cur_xp(m_inventory.PlayerXp());
}

void ClientGC::BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
    const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello)
{
    message.set_version(0); // this is accurate
    message.set_game_data(csWelcome.SerializeAsString());
    m_inventory.BuildCacheSubscription(*message.add_outofdate_subscribed_caches(), GetConfig().m_level, false);
    message.mutable_location()->set_latitude(65.0133006f);
    message.mutable_location()->set_longitude(25.4646212f);
    message.mutable_location()->set_country(GetConfig().m_country);
    message.set_game_data2(matchmakingHello.SerializeAsString());
    message.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));
    message.set_currency(GetConfig().m_currency);
    message.set_txn_country_code(GetConfig().m_country);
}

void ClientGC::SendRankUpdate()
{
    CMsgGCCStrike15_v2_ClientGCRankUpdate message;

    PlayerRankingInfo *rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().m_competitiveRank);
    rank->set_wins(GetConfig().m_competitiveWins);
    rank->set_rank_type_id(RankTypeCompetitive);

    rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().m_wingmanRank);
    rank->set_wins(GetConfig().m_wingmanWins);
    rank->set_rank_type_id(RankTypeWingman);

    // premier: rank_id is the CS Rating number
    rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().m_premierRating);
    rank->set_wins(GetConfig().m_premierWins);
    rank->set_rank_type_id(RankTypePremier);

    // "what the next match does to you" (match stakes panel, predictedRankingIfWin/IfLoss):
    // ranks stay put offline, the rating moves by a typical premier match
    constexpr uint32_t PremierStep = 125;
    for (PlayerRankingInfo &info : *message.mutable_rankings())
    {
        uint32_t current = info.rank_id();
        bool premier = info.rank_type_id() == RankTypePremier;
        info.set_rank_if_win(premier && current ? current + PremierStep : current);
        info.set_rank_if_lose(premier && current ? (current > PremierStep ? current - PremierStep : 0) : current);
        info.set_rank_if_tie(current);
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientGCRankUpdate, message);
}

void ClientGC::OnClientHello(GCMessageRead &messageRead)
{
    CMsgClientHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::Print("Parsing CMsgClientHello failed, ignoring\n");
        return;
    }

    // we don't care about anything in this message, just reply
    CMsgCStrike15Welcome csWelcome;
    BuildCSWelcome(csWelcome);

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);

    CMsgClientWelcome clientWelcome;
    BuildClientWelcome(clientWelcome, csWelcome, mmHello);

    SendMessageToGame(false, k_EMsgGCClientWelcome, clientWelcome);

    // the real gc sends this a bit later when it has more info to put on it
    // however we have everything at our fingertips so send it right away
    // mikkotodo is this even needed? k_EMsgGCClientWelcome should have it all already
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);

    // send all ranks here as well, it's a bit back and forth with real gc
    SendRankUpdate();
}

void ClientGC::OnMatchmakingHello()
{
    // only old builds explicitly request for this
    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);
}

void ClientGC::OnRequestInventoryRefresh()
{
    // only pre dota gcsdk merge builds send this
    m_inventory.SetOldProtocol(true);

    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().m_level, false);
    SendMessageToGame(false, k_ESOMsg_CacheSubscribed, message);
}

// CS2 loadout changes: a batch of { class, slot, item id } (item id 0 = clear the slot)
void ClientGC::AdjustEquipSlots(GCMessageRead &messageRead)
{
    CMsgAdjustEquipSlots message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgAdjustEquipSlots failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.AdjustEquipSlots(message);
    SendInventoryChangeMessages(messages);

    // FIXME(runtime): unverified. Echo the request back so the client sees its
    // change_num acknowledged, the real GC's reply format is not known yet.
    SendMessageToGame(false, messageRead.TypeUnmasked(), message, messageRead.JobId());
}

void ClientGC::ClientPlayerDecalSign(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPlayerDecalSign message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientPlayerDecalSign failed, ignoring\n");
        return;
    }

    if (!Graffiti::SignMessage(*message.mutable_data()))
    {
        Platform::Print("Could not sign graffiti! it won't appear\n");
        return;
    }

    Platform::Print("Graffiti: signed spray {} tint {} (equip slot {})\n", message.data().tx_defidx(), message.data().tint_id(),
        message.data().equipslot());
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientPlayerDecalSign, message);
}

void ClientGC::UseItemRequest(GCMessageRead &messageRead)
{
    CMsgUseItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgUseItem failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.UseItem(message.item_id());
    SendInventoryChangeMessages(messages);
}

static void AddressString(uint32_t ip, uint32_t port, char *buffer, size_t bufferSize)
{
    snprintf(buffer, bufferSize,
        "%u.%u.%u.%u:%u\n",
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8) & 0xff,
        ip & 0xff,
        port);
}

void ClientGC::ClientRequestJoinServerData(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestJoinServerData request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientRequestJoinServerData failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_ClientRequestJoinServerData response = request;
    response.mutable_res()->set_serverid(request.version());
    response.mutable_res()->set_direct_udp_ip(request.server_ip());
    response.mutable_res()->set_direct_udp_port(request.server_port());
    response.mutable_res()->set_reservationid(GameServerCookieId);

    char addressString[32];
    AddressString(request.server_ip(), request.server_port(), addressString, sizeof(addressString));
    response.mutable_res()->set_server_address(addressString);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientRequestJoinServerData, response);
}

void ClientGC::SetItemPositions(GCMessageRead &messageRead)
{
    CMsgSetItemPositions message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgSetItemPositions failed, ignoring\n");
        return;
    }

    std::vector<CMsgItemAcknowledged> acknowledgements;
    acknowledgements.reserve(message.item_positions_size());

    InventoryChangeMessages messages = m_inventory.SetItemPositions(message, acknowledgements);

    // send these to the server only
    if (m_serverSteamId)
    {
        for (const CMsgItemAcknowledged &acknowledgement : acknowledgements)
        {
            {
                GCMessageWrite messageWrite{ k_EMsgGCItemAcknowledged, acknowledgement };
                PostToHost(HostEvent::NetMessage, m_serverSteamId, std::move(messageWrite).TakeBuffer());
            }

            // support for older builds...
            {
                CMsgItemAcknowledged__DEPRECATED deprecated;
                deprecated.set_account_id(acknowledgement.iteminfo().accountid());
                deprecated.set_inventory(acknowledgement.iteminfo().inventory());
                deprecated.set_def_index(acknowledgement.iteminfo().defindex());
                deprecated.set_quality(acknowledgement.iteminfo().quality());
                deprecated.set_rarity(acknowledgement.iteminfo().rarity());
                deprecated.set_origin(acknowledgement.iteminfo().origin());
                deprecated.set_item_id(acknowledgement.iteminfo().itemid());

                GCMessageWrite messageWrite{ k_EMsgGCItemAcknowledged__DEPRECATED, deprecated };
                PostToHost(HostEvent::NetMessage, m_serverSteamId, std::move(messageWrite).TakeBuffer());
            }
        }
    }

    SendInventoryChangeMessages(messages);
}

void ClientGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    assert(message.event_type() == 0);

    InventoryChangeMessages messages = m_inventory.IncrementKillCountAttribute(message.item_id(), message.amount());
    SendInventoryChangeMessages(messages);
}

void ClientGC::ApplySticker(GCMessageRead &messageRead)
{
    CMsgApplySticker message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgApplySticker failed, ignoring\n");
        return;
    }

    assert(!message.item_item_id() != !message.baseitem_defidx());

    // charm removal (InventoryAPI.RemoveKeychain(item, 0)) comes in here too. What exactly the client
    // sends isn't known yet: the "Remove Keychain Tool" base item (def 65) as the tool, or no tool on
    // a weapon whose sticker slot is empty. Logged in full so gc_log.txt shows the real thing.
    uint64_t toolId = message.sticker_item_id();
    bool toolIsBaseItem = (toolId & ItemIdDefaultItemMask) == ItemIdDefaultItemMask;
    uint32_t toolDefIndex = toolIsBaseItem ? static_cast<uint32_t>(toolId & 0xffffffff) : 0;
    bool keychainOnItem = message.item_item_id() && m_inventory.ItemHasKeychain(message.item_item_id());
    if (!toolId || toolIsBaseItem)
    {
        Platform::Print("ApplySticker without a sticker: sticker_item_id {:#x} item_item_id {} sticker_slot {} "
                        "baseitem_defidx {} wear {} wear_target {} (charm on item: {}, removals left: {})\n",
            toolId, message.item_item_id(), message.sticker_slot(), message.baseitem_defidx(), message.sticker_wear(),
            message.sticker_wear_target(), keychainOnItem, m_inventory.KeychainRemoveCharges());
    }

    bool removeKeychain = keychainOnItem
        && ((toolIsBaseItem && toolDefIndex == FromEnum(ItemDefIndex::KeychainRemoveTool))
            || (!toolId && !m_inventory.ItemHasSticker(message.item_item_id(), message.sticker_slot())));

    InventoryChangeMessages messages;
    if (!removeKeychain && m_inventory.TryEncapsulateSticker(message, messages))
    {
        // sticker slab
    }
    else if (removeKeychain)
    {
        messages = m_inventory.RemoveKeychain(message.item_item_id(), message.sticker_slot());
    }
    else if (!toolId || toolIsBaseItem)
    {
        // scrape
        messages = m_inventory.ScrapeSticker(message);
    }
    else
    {
        messages = m_inventory.ApplySticker(message);
    }

    SendInventoryChangeMessages(messages);
}

void ClientGC::RedeemMissionReward(GCMessageRead &messageRead)
{
    CMsgGCCstrike15_v2_ClientRedeemMissionReward message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCstrike15_v2_ClientRedeemMissionReward failed, ignoring\n");
        return;
    }

    Platform::Print("Armory redeem: campaign {} redeem_id {} balance {} expected_cost {} bid_control {}\n",
        message.campaign_id(), message.redeem_id(), message.redeemable_balance(), message.expected_cost(), message.bid_control());

    uint64_t createdId = 0;
    InventoryChangeMessages messages = m_inventory.RedeemXpShopReward(message.campaign_id(), message.redeem_id(),
        message.expected_cost(), createdId);
    SendInventoryChangeMessages(messages);
}

// the service medal at level 40, the same message both ways. InventoryAPI.RequestPrestigeCoinCheck
// sends it empty: the answer names the medal (defindex) and the one it upgrades (upgradeid, 0 for a
// new one), and the client shows it with a button to get it. That sends RequestPrestigeCoin with
// the defindex: the answer's upgradeid is then the medal got, which the client acknowledges and
// shows; an upgradeid 0 just closes the popup (refused). Without an answer the check spins forever
void ClientGC::RequestPrestigeCoin(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin reply;
    reply.set_prestigetime(static_cast<uint32_t>(time(nullptr)));
    if (!message.defindex())
    {
        std::optional<Inventory::PrestigeCoin> coin = m_inventory.NextPrestigeCoin();
        if (!coin)
        {
            Platform::Print("Service medal check: no \"prestige coin <year>\" items in the schema\n");
            return;
        }

        Platform::Print("Service medal check: level {}, next medal {} (upgrades {})\n", m_inventory.PlayerLevel(),
            FromEnum(coin->defIndex), coin->upgradeId);
        reply.set_defindex(FromEnum(coin->defIndex));
        reply.set_upgradeid(coin->upgradeId);
    }
    else
    {
        uint64_t itemId = 0;
        std::string error;
        InventoryChangeMessages messages = m_inventory.RedeemPrestigeCoin(ToEnum<ItemDefIndex>(message.defindex()), itemId, error);
        if (!error.empty())
        {
            Platform::Print("Service medal refused: {}\n", error);
        }

        // the item and the new level first, then the answer that shows the medal
        SendInventoryChangeMessages(messages);
        reply.set_defindex(message.defindex());
        reply.set_upgradeid(itemId);
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Client2GCRequestPrestigeCoin, reply);
}

void ClientGC::AckXpShopTracks(GCMessageRead &messageRead)
{
    InventoryChangeMessages messages = m_inventory.AckXpShopTracks();
    SendInventoryChangeMessages(messages);
}

void ClientGC::StoreGetUserData(GCMessageRead &messageRead)
{
    CMsgStoreGetUserData message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgStoreGetUserData failed, ignoring\n");
        return;
    }

    std::string priceSheetPath = Platform::DataPath("price_sheet.txt");
    KeyValueRead priceSheetRead{ priceSheetPath.c_str() };
    if (!priceSheetRead.Good())
    {
        return;
    }

    std::string binaryString;
    binaryString.reserve(1 << 17);
    priceSheetRead.Root().BinaryWriteToString(binaryString);

    // fuck you idiot
    CMsgStoreGetUserDataResponse response;
    response.set_result(1);
    response.set_currency_deprecated(GetConfig().m_currency); // for old builds
    response.set_country_deprecated(GetConfig().m_country); // for old builds
    response.set_price_sheet_version(1729); // what
    *response.mutable_price_sheet() = std::move(binaryString);

    SendMessageToGame(false, k_EMsgGCStoreGetUserDataResponse, response, messageRead.JobId());
}

void ClientGC::StorePurchaseInit(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseInit message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseInit failed, ignoring\n");
        return;
    }

    // value doesn't matter
    assert(!m_transactionId);
    m_transactionId = Random::Uint32(1, UINT32_MAX);

    std::vector<uint32_t> purchasedDefIndexes;
    std::vector<uint64_t> terminalOffers;
    m_transactionTerminal = 0;

    for (const auto &item : message.line_items())
    {
        Platform::Print("Store: def {} x{} cost {} type {} supplemental {}\n", item.item_def_id(), item.quantity(),
            item.cost_in_local_currency(), item.purchase_type(), item.supplemental_data());

        // StoreAPI.StoreItemPurchase("<terminal def>(<terminal id>)"): the terminal's current offer
        uint64_t terminal = 0;
        if (uint64_t offerId = m_inventory.TerminalOfferFor(item.item_def_id(), item.supplemental_data(), item.cost_in_local_currency(), &terminal))
        {
            m_transactionTerminal = terminal;
            terminalOffers.push_back(offerId);
            continue;
        }

        for (uint32_t i = 0; i < item.quantity(); i++)
        {
            purchasedDefIndexes.push_back(item.item_def_id());
        }
    }

    InventoryChangeMessages messages = m_inventory.PurchaseItems(purchasedDefIndexes, m_transactionItemIds);
    m_transactionItemIds.insert(m_transactionItemIds.end(), terminalOffers.begin(), terminalOffers.end());

    char url[128]; // url doesn't matter, but it needs to be set
    snprintf(url, sizeof(url), "https://checkout.steampowered.com/checkout/approvetxn/%llu/?returnurl=steam", m_transactionId);

    CMsgGCStorePurchaseInitResponse response;
    response.set_result(1); // success
    response.set_txn_id(m_transactionId);
    response.set_url(url);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());

    SendMessageToGame(false, k_EMsgGCStorePurchaseInitResponse, response, messageRead.JobId());

    SendInventoryChangeMessages(messages);

    // this will run the steam callback
    PostToHost(HostEvent::MicroTransactionResponse, 0, {});
}

void ClientGC::StorePurchaseFinalize(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseFinalize message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseFinalize failed, ignoring\n");
        return;
    }

    assert(m_transactionId);

    // a terminal offer changes hands only now, so the terminal stays open until the purchase is through
    if (m_transactionTerminal)
    {
        uint64_t offerId = 0;
        SendInventoryChangeMessages(m_inventory.BuyTerminalOffer(m_transactionTerminal, offerId));
        m_transactionTerminal = 0;
    }

    CMsgGCStorePurchaseFinalizeResponse response;
    response.set_result(1); // success
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());
    SendMessageToGame(false, k_EMsgGCStorePurchaseFinalizeResponse, response, messageRead.JobId());

    // done with this one
    m_transactionId = 0;
}

void ClientGC::StorePurchaseFinalizeStruct(GCMessageRead &messageRead)
{
    messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCStorePurchaseFinalize (struct) failed, ignoring\n");
        return;
    }

    assert(m_transactionId);

    {
        GCMessageWrite response{ k_EMsgGCStorePurchaseFinalizeResponse, messageRead.JobId() };

        response.WriteUint16(1); // success
        response.WriteUint32(static_cast<uint32_t>(m_transactionItemIds.size()));

        for (uint64_t id : m_transactionItemIds)
        {
            response.WriteUint64(id);
        }

        uint32_t typeMasked = response.TypeMasked();
        PostToHost(HostEvent::Message, typeMasked, std::move(response).TakeBuffer());
    }

    // done with this one
    m_transactionId = 0;
}

void ClientGC::OnGetEventFavorites(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_GetEventFavorites_Request request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing GetEventFavorites failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_GetEventFavorites_Response response;
    response.set_all_events(request.all_events());
    response.set_json_favorites("[]");
    response.set_json_featured("[]");

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_GetEventFavorites_Response, response);
}

void ClientGC::OnPartyRegister(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_Party_Register message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Failed to parse Party_Register\n");
        return;
    }

    uint32_t lobbyId = message.id();
    if (!lobbyId)
    {
        lobbyId = Random::Uint32(1, UINT32_MAX);
    }

    CMsgGCCStrike15_v2_Party_Register response;
    response.set_id(lobbyId);
    response.set_ver(message.ver());
    response.set_apr(1);
    response.set_ark(static_cast<uint32_t>(GetConfig().m_competitiveRank) * 10);
    response.set_grp(3);
    response.set_slots(5);
    response.set_launcher(message.launcher());
    response.set_game_type(message.game_type());

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Party_Register, response);
}

void ClientGC::DeleteItem(GCMessageRead &messageRead)
{
    // there is data after this, but i don't know what it is
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCDelete failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.RemoveItem(itemId);
    SendInventoryChangeMessages(messages);
}

void ClientGC::OpenCrate(GCMessageRead &messageRead)
{
    CMsgOpenCrate message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgOpenCrate failed, ignoring\n");
        return;
    }

    // terminals: UseToolWithIntArg(terminal, terminal, offer number) asks for the next offer
    if (m_inventory.IsActiveTerminal(message.subject_item_id()))
    {
        Platform::Print("Terminal {}: next offer (client is at {}, limit {})\n", message.subject_item_id(),
            static_cast<int32_t>(message.points_remaining()), message.volatile_limit());
        SendInventoryChangeMessages(m_inventory.NextTerminalOffer(message.subject_item_id(), message.volatile_limit()));
        return;
    }

    Platform::Print("CASE OPENING {} with {}{}\n", message.subject_item_id(), message.tool_item_id(),
        message.for_rental() ? " (7 day rental)" : "");

    if (m_inventory.IsSealedTerminal(message.subject_item_id()))
    {
        uint64_t containerId = 0;
        SendInventoryChangeMessages(m_inventory.UnsealTerminal(message.subject_item_id(), containerId));

        CMsgRequestInventoryRefresh response;
        SendMessageToGame(false, k_EMsgGCUnlockCrateResponse, response);
        return;
    }

    InventoryChangeMessages messages = message.for_rental()
        ? m_inventory.RentCrate(message.subject_item_id(), message.tool_item_id())
        : m_inventory.UnlockCrate(message.subject_item_id(), message.tool_item_id());
    SendInventoryChangeMessages(messages);

    CMsgRequestInventoryRefresh response;
    SendMessageToGame(false, k_EMsgGCUnlockCrateResponse, response);
}

void ClientGC::EconPreviewDataBlockRequest(GCMessageRead &messageRead)
{
    // +csgo_econ_action_preview S<owner>A<item>D<key> (or M<market listing>A<item>D<key>)
    CMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockRequest message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockRequest failed, ignoring\n");
        return;
    }

    // only our own items exist here; links of other people's or market items have nothing to show
    CMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockResponse response;
    if (!m_inventory.PreviewDataBlock(message.param_a(), *response.mutable_iteminfo()))
    {
        Platform::Print("Inspect: item {} (owner {}, market {}) is not in this inventory\n", message.param_a(),
            message.param_s(), message.param_m());
        return;
    }

    Platform::Print("Inspect: item {} def {} paint {}\n", message.param_a(), response.iteminfo().defindex(),
        response.iteminfo().paintindex());
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_Client2GCEconPreviewDataBlockResponse, response, messageRead.JobId());
}

void ClientGC::VolatileItemLoadContents(GCMessageRead &messageRead)
{
    // client.dll sends CMsgOpenCrate here when an active terminal is opened again
    CMsgOpenCrate message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing VolatileItemLoadContents failed, ignoring\n");
        return;
    }

    Platform::Print("Terminal {}: reopened (client is at {})\n", message.subject_item_id(),
        static_cast<int32_t>(message.points_remaining()));
    SendInventoryChangeMessages(m_inventory.TerminalContents(message.subject_item_id()));
}

void ClientGC::VolatileItemClaimReward(GCMessageRead &messageRead)
{
    // InventoryAPI.ClaimVolatileReward(def, reward) sends a CSOVolatileItemClaimedRewards
    CSOVolatileItemClaimedRewards message;
    if (!messageRead.ReadProtobuf(message) || !message.reward_size())
    {
        Platform::Print("Parsing VolatileItemClaimReward failed, ignoring\n");
        return;
    }

    SendInventoryChangeMessages(m_inventory.ClaimTerminalReward(message.defidx(), message.reward(0)));
}

void ClientGC::RedeemFreeReward(GCMessageRead &messageRead)
{
    CMsgGCCstrike15_v2_ClientRedeemFreeReward message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCstrike15_v2_ClientRedeemFreeReward failed, ignoring\n");
        return;
    }

    Platform::Print("Care package: picking {} items (generation {}, balance {})\n", message.items_size(),
        message.generation_time(), message.redeemable_balance());
    std::vector<uint64_t> items{ message.items().begin(), message.items().end() };
    SendInventoryChangeMessages(m_inventory.RedeemFreeRewards(items));
}

void ClientGC::MatchEndRunRewardDrops(GCMessageRead &messageRead)
{
    // the game server's end of match report, forwarded by our ServerGC
    CMsgGCCStrike15_v2_MatchEndRunRewardDrops message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_MatchEndRunRewardDrops failed, ignoring\n");
        return;
    }

    const CMsgGC_ServerQuestUpdateData &data = message.match_end_quest_data();
    for (const PlayerQuestData &player : data.player_quest_data())
    {
        uint32_t xp = 0;
        for (const XpProgressData &progress : player.xp_progress_data())
        {
            Platform::Print("Match end: account {} xp {} category {}\n", player.quester_account_id(), progress.xp_points(),
                progress.xp_category());
            xp += progress.xp_points();
        }

        if (player.quester_account_id() == AccountId())
        {
            Platform::Print("Match end: {} xp, played {} s, game mode {}\n", xp, player.time_played(), data.mm_game_mode());
            SendInventoryChangeMessages(m_inventory.AddMatchXp(xp, "the match"));
            m_matchReported = true;
        }
    }
}

void ClientGC::Craft(GCMessageRead &messageRead)
{
    CMsgCraftItemsRequest message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCraftItemsRequest failed, ignoring\n");
        return;
    }

    Platform::Print("TRADE UP recipe {} with {} items\n", message.recipe_def(), message.craft_items_size());

    std::vector<uint64_t> itemIds{ message.craft_items().begin(), message.craft_items().end() };
    uint64_t createdId = 0;
    InventoryChangeMessages messages = m_inventory.TradeUp(message.recipe_def(), itemIds, createdId);

    // the new item has to exist on the client before the response: the trade up panel
    // shows the newest item of the inventory, the ids in the response aren't read
    SendInventoryChangeMessages(messages);

    // the request is protobuf but the response is still the old struct message:
    // MsgGCStandardResponse_t (int16 recipe, uint32 EGCMsgResponse), uint16 count, uint64 ids
    GCMessageWrite response{ k_EMsgGCCraftResponse };
    response.WriteUint16(static_cast<uint16_t>(message.recipe_def()));
    response.WriteUint32(createdId ? k_EGCMsgResponseOK : k_EGCMsgResponseDenied);
    response.WriteUint16(createdId ? 1 : 0);
    if (createdId)
    {
        response.WriteUint64(createdId);
    }

    uint32_t typeMasked = response.TypeMasked();
    PostToHost(HostEvent::Message, typeMasked, std::move(response).TakeBuffer());
}

void ClientGC::DeleteItemProto(GCMessageRead &messageRead)
{
    CMsgDeleteItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgDeleteItem failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.RemoveItem(message.item_id());
    SendInventoryChangeMessages(messages);
}

void ClientGC::UnlockCrate(GCMessageRead &messageRead)
{
    uint64_t keyId = messageRead.ReadUint64();
    uint64_t crateId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCUnlockCrate failed, ignoring\n");
        return;
    }

    Platform::Print("CASE OPENING {} with {}\n", crateId, keyId);

    uint64_t containerId = 0;
    InventoryChangeMessages messages = m_inventory.IsSealedTerminal(crateId)
        ? m_inventory.UnsealTerminal(crateId, containerId)
        : m_inventory.UnlockCrate(crateId, keyId);
    SendInventoryChangeMessages(messages);

    // send this for old builds
    // FIXME: payload contents? don't think game reads it so we
    // can just pass an empty protobuf though... should probably
    // have a more idiomatic way of sending empty messages
    CMsgRequestInventoryRefresh response;
    SendMessageToGame(false, k_EMsgGCUnlockCrateResponse, response);
}

void ClientGC::NameItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint64_t itemId = messageRead.ReadUint64();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameItem failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.NameItem(nameTagId, itemId, name);
    SendInventoryChangeMessages(messages);
}

void ClientGC::ModifyItemStringAttr(GCMessageRead &messageRead)
{
    // CS2 sends name tags, storage unit names and name removal as CMsgModifyItemStringAttr
    // under the old message ids. Items without an id (base weapons) come as NameBaseItem with
    // a faux id, a storage unit comes with a faux Name Tag (1200) as the tool, and attr_def is
    // only set for the description ("custom desc attr"), which the UI never fills in
    CMsgModifyItemStringAttr message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgModifyItemStringAttr failed, ignoring\n");
        return;
    }

    uint32_t type = messageRead.TypeUnmasked();
    uint64_t toolId = message.tool_item_id();
    uint64_t itemId = message.subject_item_id();
    Platform::Print("{}: tool {} item {} attr {} \"{}\"\n", MessageName(type), toolId, itemId,
        message.attr_def(), message.attr_value());

    if (message.has_attr_def() && message.attr_def() != static_cast<uint32_t>(AttributeDefIndex::CustomName))
    {
        Platform::Print("Item descriptions are not supported, ignoring\n");
        return;
    }

    InventoryChangeMessages messages;
    if (type == k_EMsgGCRemoveItemName)
    {
        messages = m_inventory.RemoveItemName(itemId);
    }
    else if (type == k_EMsgGCNameBaseItem)
    {
        uint32_t defIndex = static_cast<uint32_t>(itemId & 0xffff);
        messages = m_inventory.NameBaseItem(toolId, defIndex, message.attr_value());
    }
    else
    {
        messages = m_inventory.NameItem(toolId, itemId, message.attr_value());
    }

    SendInventoryChangeMessages(messages);
}

void ClientGC::ModifyItemAttribute(GCMessageRead &messageRead)
{
    // InventoryAPI.SetItemAttributeValueAsync: the game uses it for a tournament coin's graffiti,
    // the upgrader window (CSForge) for its request, see ModLoader's UpgraderWindow
    CMsgModifyItemAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgModifyItemAttribute failed, ignoring\n");
        return;
    }

    uint64_t itemId = message.item_id();
    uint32_t attribute = message.attr_defidx();
    uint32_t value = message.attr_value();
    if (value >= 0x3f800000u && value < 0x4b800000u)
    {
        // a float's bits (1.0 and up): the window's numbers are small integers
        float asFloat;
        memcpy(&asFloat, &value, sizeof(asFloat));
        value = static_cast<uint32_t>(asFloat);
    }

    constexpr uint32_t UpgradeThreshold = 73; // "upgrade threshold": a deposit, value = target index + 1
    constexpr uint32_t UpgradeLevel = 268; // "upgrade level": go, value = how many deposits
    uint32_t now = static_cast<uint32_t>(time(nullptr));

    if (attribute == UpgradeThreshold)
    {
        if (value != m_upgradeTarget || now - m_upgradeTime > 30 || m_upgradeDeposits.size() >= Inventory::UpgradeMaxDeposits)
        {
            m_upgradeDeposits.clear();
        }
        m_upgradeTarget = value;
        m_upgradeTime = now;
        m_upgradeDeposits.push_back(itemId);
        return;
    }

    if (attribute == UpgradeLevel)
    {
        std::vector<uint64_t> deposits = std::move(m_upgradeDeposits);
        uint32_t target = m_upgradeTarget;
        m_upgradeDeposits.clear();
        m_upgradeTarget = 0;

        // the window's target: catalog index * 16 + wear * 2 + StatTrak, plus 1
        std::span<const ModLoader::UpgradeTarget> catalog = ModLoader::UpgraderCatalog();
        size_t index = target ? (target - 1) / 16 : 0;
        int wear = target ? static_cast<int>(((target - 1) % 16) / 2) : 0;
        bool statTrak = target && (target - 1) % 2;
        if (!target || index >= catalog.size() || wear >= SkinPrices::WearCount || deposits.size() != value || deposits.empty()
            || deposits[0] != itemId)
        {
            Platform::Print("Upgrader: bad request ({} deposits of {}, target {} of {})\n", deposits.size(), value, target, catalog.size());
            SendInventoryChangeMessages(m_inventory.TouchItem(itemId)); // the window sees it as refused
            return;
        }

        const ModLoader::UpgradeTarget &chosen = catalog[index];
        Inventory::UpgradeOffer offer{ ToEnum<ItemDefIndex>(chosen.defIndex), chosen.paintKit, wear, statTrak,
            statTrak ? chosen.statTrak[wear] : chosen.price[wear] };
        Inventory::UpgradeResult result;
        InventoryChangeMessages messages = m_inventory.Upgrade(deposits, offer, &ModLoader::UpgraderItemPrice, result);
        if (!result.done)
        {
            Platform::Print("Upgrader: refused: {}\n", result.error);
            SendInventoryChangeMessages(m_inventory.TouchItem(itemId)); // the window sees it as refused
            return;
        }

        SendInventoryChangeMessages(messages);
        return;
    }

    Platform::Print("ModifyItemAttribute: item {} attribute {} = {} not handled\n", itemId, attribute, value);
}

void ClientGC::StatTrakSwap(GCMessageRead &messageRead)
{
    // CMsgApplyStatTrakSwap, or the same three ids as a struct message
    uint64_t toolId = 0, itemId1 = 0, itemId2 = 0;
    if (messageRead.IsProtobuf())
    {
        CMsgApplyStatTrakSwap message;
        if (!messageRead.ReadProtobuf(message))
        {
            Platform::Print("Parsing CMsgApplyStatTrakSwap failed, ignoring\n");
            return;
        }
        toolId = message.tool_item_id();
        itemId1 = message.item_1_item_id();
        itemId2 = message.item_2_item_id();
    }
    else
    {
        toolId = messageRead.ReadUint64();
        itemId1 = messageRead.ReadUint64();
        itemId2 = messageRead.ReadUint64();
        if (!messageRead.IsValid())
        {
            Platform::Print("Parsing the StatTrak swap struct failed, ignoring\n");
            return;
        }
    }

    InventoryChangeMessages messages = m_inventory.StatTrakSwap(toolId, itemId1, itemId2);
    SendInventoryChangeMessages(messages);
}

void ClientGC::NameBaseItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint32_t defIndex = messageRead.ReadUint32();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameBaseItem failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.NameBaseItem(nameTagId, defIndex, name);
    SendInventoryChangeMessages(messages);
}

void ClientGC::RemoveItemName(GCMessageRead &messageRead)
{
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCRemoveItemName failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.RemoveItemName(itemId);
    SendInventoryChangeMessages(messages);
}

void ClientGC::ProcessCasketItemLoadContents(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_CasketContents);
    notification.add_item_id(message.casket_item_id());
    SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
}

void ClientGC::ProcessCasketItemAdd(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCasketItem failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.CasketItemAdd(message.casket_item_id(), message.item_item_id());
    SendInventoryChangeMessages(messages);
}

void ClientGC::ProcessCasketItemExtract(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCasketItem failed, ignoring\n");
        return;
    }

    InventoryChangeMessages messages = m_inventory.CasketItemExtract(message.casket_item_id(), message.item_item_id());
    SendInventoryChangeMessages(messages);
}

// also used by gc_server.cpp

namespace
{
struct MessageNameEntry
{
    uint32_t id;
    const char *name;
};

constexpr MessageNameEntry MessageNames[] = {
#include "message_names.inc"
};
}

const char *MessageName(uint32_t type)
{
    auto it = std::lower_bound(std::begin(MessageNames), std::end(MessageNames), type,
        [](const MessageNameEntry &entry, uint32_t id) { return entry.id < id; });

    if (it != std::end(MessageNames) && it->id == type)
    {
        return it->name;
    }

    static thread_local char buffer[16];
    snprintf(buffer, sizeof(buffer), "%u", type);
    return buffer;
}

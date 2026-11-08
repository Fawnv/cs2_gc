#include "stdafx.h"
#include "steammatchmakingserversproxy.h"
#include "config.h"

MatchMakingKeyValuePair_t *SteamMatchmakingServersProxy::ModifyFilters(MatchMakingKeyValuePair_t *pchFilters, uint32 nFilters, std::vector<MatchMakingKeyValuePair_t> &buffer)
{
    buffer.reserve(nFilters + 1);
    buffer.assign(pchFilters, pchFilters + nFilters);

    if (GetConfig().m_showCsgoGCServersOnly)
    {
        buffer.push_back({ "gametagsand", "cs2_gc" });
    }

    return buffer.data();
}

PROXY_FUNC(HServerListRequest, SteamMatchmakingServersProxy::RequestInternetServerList, AppId_t iApp,
    MatchMakingKeyValuePair_t **ppchFilters,
    uint32 nFilters,
    ISteamMatchmakingServerListResponse *pRequestServersResponse)
{

    std::vector<MatchMakingKeyValuePair_t> buffer;
    MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
    return original(iApp, &filters, buffer.size(), pRequestServersResponse);
}

PROXY_FUNC(HServerListRequest, SteamMatchmakingServersProxy::RequestLANServerList, AppId_t iApp,
    ISteamMatchmakingServerListResponse *pRequestServersResponse)
{

    return original(iApp, pRequestServersResponse);
}

PROXY_FUNC(HServerListRequest, SteamMatchmakingServersProxy::RequestFriendsServerList, AppId_t iApp,
    MatchMakingKeyValuePair_t **ppchFilters,
    uint32 nFilters,
    ISteamMatchmakingServerListResponse *pRequestServersResponse)
{

    std::vector<MatchMakingKeyValuePair_t> buffer;
    MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
    return original(iApp, &filters, buffer.size(), pRequestServersResponse);
}

PROXY_FUNC(HServerListRequest, SteamMatchmakingServersProxy::RequestFavoritesServerList, AppId_t iApp,
    MatchMakingKeyValuePair_t **ppchFilters,
    uint32 nFilters,
    ISteamMatchmakingServerListResponse *pRequestServersResponse)
{

    std::vector<MatchMakingKeyValuePair_t> buffer;
    MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
    return original(iApp, &filters, buffer.size(), pRequestServersResponse);
}

PROXY_FUNC(HServerListRequest, SteamMatchmakingServersProxy::RequestHistoryServerList, AppId_t iApp,
    MatchMakingKeyValuePair_t **ppchFilters,
    uint32 nFilters,
    ISteamMatchmakingServerListResponse *pRequestServersResponse)
{

    std::vector<MatchMakingKeyValuePair_t> buffer;
    MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
    return original(iApp, &filters, buffer.size(), pRequestServersResponse);
}

PROXY_FUNC(HServerListRequest, SteamMatchmakingServersProxy::RequestSpectatorServerList, AppId_t iApp,
    MatchMakingKeyValuePair_t **ppchFilters,
    uint32 nFilters,
    ISteamMatchmakingServerListResponse *pRequestServersResponse)
{

    std::vector<MatchMakingKeyValuePair_t> buffer;
    MatchMakingKeyValuePair_t *filters = ModifyFilters(*ppchFilters, nFilters, buffer);
    return original(iApp, &filters, buffer.size(), pRequestServersResponse);
}

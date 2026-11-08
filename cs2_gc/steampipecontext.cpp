#include "stdafx.h"
#include "steampipecontext.h"
#include "config.h"
#include "gc_shared.h"
#include "steam_callbacks.h"

struct SteamNetworkingIdentity;

#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>

#include "steamfriendsproxy.h"
#include "steamgamecoordinatorproxy.h"
#include "steamgameserverproxy.h"
#include "steammatchmakingserversproxy.h"
#include "steamuserproxy.h"
#include "steamutilsproxy.h"

// compatibility with the old interface headers below
#define STEAM_METHOD_DESC(DESC) STEAM_DESC(DESC)
#define CALL_RESULT(RESULT_TYPE) STEAM_CALL_RESULT(RESULT_TYPE)

// now generate the proxy glue...
#include <proxy/steamfriendsproxy017.h>
#include <proxy/steamgamecoordinatorproxy001.h>
#include <proxy/steamgameserverproxy010.h>
#include <proxy/steamgameserverproxy011.h>
#include <proxy/steamgameserverproxy012.h>
#include <proxy/steamgameserverproxy013.h>
#include <proxy/steamgameserverproxy014.h>
#include <proxy/steamgameserverproxy015.h>
#include <proxy/steammatchmakingserversproxy002.h>
#include <proxy/steamuserproxy014.h>
#include <proxy/steamuserproxy015.h>
#include <proxy/steamuserproxy016.h>
#include <proxy/steamuserproxy017.h>
#include <proxy/steamuserproxy018.h>
#include <proxy/steamuserproxy019.h>
#include <proxy/steamuserproxy020.h>
#include <proxy/steamuserproxy021.h>
#include <proxy/steamuserproxy022.h>
#include <proxy/steamuserproxy023.h>
#include <proxy/steamutilsproxy002.h>
#include <proxy/steamutilsproxy005.h>
#include <proxy/steamutilsproxy006.h>
#include <proxy/steamutilsproxy007.h>
#include <proxy/steamutilsproxy008.h>
#include <proxy/steamutilsproxy009.h>
#include <proxy/steamutilsproxy010.h>

template<typename Proxy, typename... Args>
inline Proxy *GetOrCreate(std::unique_ptr<Proxy> &pointer, Args &&...args)
{
    if (!pointer)
    {
        pointer = std::make_unique<Proxy>(std::forward<Args>(args)...);
    }

    return pointer.get();
}

bool VersionNameIs(const char *version, const char *prefix)
{
    size_t prefixLength = strlen(prefix);
    if (!strncmp(version, prefix, prefixLength))
    {
        if (isdigit(static_cast<unsigned char>(version[prefixLength])))
        {
            return true;
        }
    }

    return false;
}

bool VersionNumberIs(const char *version, const char *suffix)
{
    return std::string_view{ version }.ends_with(suffix);
}

SteamPipeContext::SteamPipeContext(HSteamPipe pipe, bool gameServer, ISteamClient &steamClient, CCallbackMgr &callbackMgr)
    : m_steamPipe{ pipe }
    , m_gameServer{ gameServer }
    , m_steamClient{ steamClient }
    , m_callbackMgr{ callbackMgr }
{
}

SteamPipeContext::~SteamPipeContext() = default;

void SteamPipeContext::SetUser(HSteamUser user)
{
    assert(!m_steamUser && user);
    m_steamUser = user;
}

void SteamPipeContext::ClearUser(HSteamUser user)
{
    // really we should destroy some state... but we assume that
    // this context is going to get wiped anyway right after
    assert(user && m_steamUser == user);
    m_steamUser = 0;
}

void SteamPipeContext::PostToGC(GCEvent type, uint64_t id, const void *data, uint32_t size)
{
    std::lock_guard lock{ m_pendingMutex };
    if (m_proxySteamGameCoordinator)
    {
        m_proxySteamGameCoordinator->GetGC().PostToGC(type, id, data, size);
        return;
    }

    // the game can do these before it asks for the gc: a listen server sometimes authenticates
    // the local player first, and losing that ClientConnected leaves the player with default
    // weapons for the whole match. keep them until the gc exists
    bool keep = type == GCEvent::ClientConnected || type == GCEvent::ClientDisconnected
        || type == GCEvent::SetAuthTicket || type == GCEvent::ClearAuthTicket;
    if (!keep || m_pendingEvents.size() >= 64)
    {
        return;
    }

    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    m_pendingEvents.push_back({ type, id, std::vector<uint8_t>(bytes, bytes + (data ? size : 0)) });
}

void SteamPipeContext::RunFrame()
{
    if (m_proxySteamGameCoordinator)
    {
        m_proxySteamGameCoordinator->RunFrame();
    }
}

void *SteamPipeContext::GetInterface(const char *version, void *original)
{
    auto it = m_interfaces.find(version);
    if (it != m_interfaces.end())
    {
        return it->second.interface;
    }

    InterfaceProxy proxy = CreateInterfaceProxy(version, original);
    if (!proxy.interface)
    {
        return nullptr;
    }

    auto result = m_interfaces.emplace(version, std::move(proxy));
    return result.first->second.interface;
}

SteamPipeContext::InterfaceProxy SteamPipeContext::CreateInterfaceProxy(const char *version, void *original)
{
#define PROXY_INTERFACE(base, number, ...) \
    if (VersionNumberIs(version, #number)) \
    { \
        auto *proxy = new base##Proxy##number{ static_cast<I##base##number *>(original), __VA_ARGS__ }; \
        void *interface = static_cast<I##base##number *>(proxy); \
        return InterfaceProxy{ interface, std::unique_ptr<void, void (*)(void *)>(proxy, [](void *p) { delete static_cast<base##Proxy##number *>(p); }) }; \
    }

    if (VersionNameIs(version, "SteamUtils"))
    {
        // old csgo builds fetch SteamUtils with a stale version...
        // technically no need to handle this, but do it for completeness sake
        PROXY_INTERFACE(SteamUtils, 002, GetOrCreate(m_proxySteamUtils));

        PROXY_INTERFACE(SteamUtils, 005, GetOrCreate(m_proxySteamUtils));
        PROXY_INTERFACE(SteamUtils, 006, GetOrCreate(m_proxySteamUtils));
        PROXY_INTERFACE(SteamUtils, 007, GetOrCreate(m_proxySteamUtils));
        PROXY_INTERFACE(SteamUtils, 008, GetOrCreate(m_proxySteamUtils));
        PROXY_INTERFACE(SteamUtils, 009, GetOrCreate(m_proxySteamUtils));
        PROXY_INTERFACE(SteamUtils, 010, GetOrCreate(m_proxySteamUtils));
        Platform::Print("Can't hook {}\n", version);
        return {};
    }

    if (!m_steamUser)
    {
        // remaining ones need user context
        return {};
    }

    if (VersionNameIs(version, "SteamGameCoordinator"))
    {
        PROXY_INTERFACE(SteamGameCoordinator, 001, GetOrCreateGameCoordinator());
        Platform::Print("Can't hook {}\n", version);
    }
    else if (VersionNameIs(version, "SteamGameServer"))
    {
        PROXY_INTERFACE(SteamGameServer, 010, GetOrCreate(m_proxySteamGameServer, *this));
        PROXY_INTERFACE(SteamGameServer, 011, GetOrCreate(m_proxySteamGameServer, *this));
        PROXY_INTERFACE(SteamGameServer, 012, GetOrCreate(m_proxySteamGameServer, *this));
        PROXY_INTERFACE(SteamGameServer, 013, GetOrCreate(m_proxySteamGameServer, *this));
        PROXY_INTERFACE(SteamGameServer, 014, GetOrCreate(m_proxySteamGameServer, *this));
        PROXY_INTERFACE(SteamGameServer, 015, GetOrCreate(m_proxySteamGameServer, *this));
        Platform::Print("Can't hook {}\n", version);
    }
    else if (VersionNameIs(version, "SteamFriends") && !m_gameServer && GetConfig().m_matchXp)
    {
        // rich presence tells ClientGC when matches start and end (match xp)
        PROXY_INTERFACE(SteamFriends, 017, GetOrCreate(m_proxySteamFriends, *this));
        Platform::Print("Can't hook {}, no match xp\n", version);
    }
    else if (VersionNameIs(version, "SteamMatchMakingServers"))
    {
        PROXY_INTERFACE(SteamMatchmakingServers, 002, GetOrCreate(m_proxySteamMatchmakingServers));
        Platform::Print("Can't hook {}\n", version);
    }
    else if (VersionNameIs(version, "SteamUser"))
    {
        PROXY_INTERFACE(SteamUser, 014, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 015, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 016, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 017, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 018, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 019, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 020, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 021, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 022, GetOrCreate(m_proxySteamUser, *this));
        PROXY_INTERFACE(SteamUser, 023, GetOrCreate(m_proxySteamUser, *this));
        Platform::Print("Can't hook {}\n", version);
    }
#undef PROXY_INTERFACE

    // not proxied
    return {};
}

SteamGameCoordinatorProxy *SteamPipeContext::GetOrCreateGameCoordinator()
{
    std::lock_guard lock{ m_pendingMutex };
    if (!m_proxySteamGameCoordinator)
    {
        if (m_gameServer)
        {
            m_proxySteamGameCoordinator = CreateSteamGameCoordinatorProxyServer(m_steamClient, m_steamPipe, m_steamUser, m_callbackMgr);
        }
        else
        {
            m_proxySteamGameCoordinator = CreateSteamGameCoordinatorProxyClient(m_steamClient, m_steamPipe, m_steamUser, m_callbackMgr);
        }

        if (!m_pendingEvents.empty())
        {
            Platform::Print("{}: delivering {} events from before the gc existed\n",
                m_gameServer ? "ServerGC" : "ClientGC", m_pendingEvents.size());
        }

        for (PendingEvent &event : m_pendingEvents)
        {
            m_proxySteamGameCoordinator->GetGC().PostToGC(event.type, event.id, event.buffer.data(),
                static_cast<uint32_t>(event.buffer.size()));
        }

        m_pendingEvents.clear();
    }

    return m_proxySteamGameCoordinator.get();
}

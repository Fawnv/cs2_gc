#pragma once

#include <steam/steam_api_common.h>
#include <mutex>

class CCallbackMgr;
class ISteamClient;
class SteamFriendsProxy;
class SteamGameCoordinatorProxy;
class SteamGameServerProxy;
class SteamMatchmakingServersProxy;
class SteamUserProxy;
class SteamUtilsProxy;

enum class GCEvent;

bool VersionNameIs(const char *version, const char *prefix);
bool VersionNumberIs(const char *version, const char *suffix);

class SteamPipeContext final : public NonCopyable
{
public:
    SteamPipeContext(HSteamPipe pipe, bool gameServer, ISteamClient &steamClient, CCallbackMgr &callbackMgr);
    ~SteamPipeContext();

    void SetUser(HSteamUser user);
    void ClearUser(HSteamUser user);

    // FIXME: need to revisit these getters...
    HSteamUser GetUser() const { return m_steamUser; }
    bool IsGameServer() const { return m_gameServer; }

    void *GetInterface(const char *version, void *original);

    void RunFrame();

    void PostToGC(GCEvent type, uint64_t id, const void *data, uint32_t size);

private:
    struct InterfaceProxy
    {
        void *interface{};
        std::unique_ptr<void, void (*)(void *)> owner{ nullptr, nullptr };
    };

    // an event posted before the game asked for the gc, delivered once it does
    struct PendingEvent
    {
        GCEvent type;
        uint64_t id;
        std::vector<uint8_t> buffer;
    };

    InterfaceProxy CreateInterfaceProxy(const char *version, void *original);
    SteamGameCoordinatorProxy *GetOrCreateGameCoordinator();

    const HSteamPipe m_steamPipe;
    const bool m_gameServer;

    ISteamClient &m_steamClient;
    CCallbackMgr &m_callbackMgr;

    // currently one user per pipe, trivial to change later if needed (not)
    HSteamUser m_steamUser{};

    std::unique_ptr<SteamGameCoordinatorProxy> m_proxySteamGameCoordinator;
    std::recursive_mutex m_pendingMutex; // recursive: making the gc goes back through our steam hooks
    std::vector<PendingEvent> m_pendingEvents;
    std::unique_ptr<SteamUtilsProxy> m_proxySteamUtils;
    std::unique_ptr<SteamFriendsProxy> m_proxySteamFriends;
    std::unique_ptr<SteamGameServerProxy> m_proxySteamGameServer;
    std::unique_ptr<SteamUserProxy> m_proxySteamUser;
    std::unique_ptr<SteamMatchmakingServersProxy> m_proxySteamMatchmakingServers;
    StringMap<InterfaceProxy> m_interfaces;
};

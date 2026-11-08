// this file sucks, don't scroll down!!! all you need to know is
// that this is the bridge betweem the game and ClientGC/ServerGC
#include "stdafx.h"
#include "steam_hook.h"
#include "appid.h"
#include "config.h"
#include "platform.h"
#include "steam_callbacks.h"
#include <funchook.h>

struct SteamNetworkingIdentity;

// defines STEAM_PRIVATE_API
#include <steam/steam_api_common.h>

#undef STEAM_PRIVATE_API // we need these public so we can proxy them
#define STEAM_PRIVATE_API(...) __VA_ARGS__

#include <steam/steam_api.h>
#include <steam/steam_gameserver.h>

#include <proxy/steamproxy.h>
#include "steampipecontext.h"

// glue for the old interfaces below...
class ISteamMasterServerUpdater;
class ISteamUnifiedMessages;
typedef void (*SteamAPI_PostAPIResultInProcess_t)(SteamAPICall_t, void *, uint32, int);

static ISteamClient *s_actualSteamClient;

static CCallbackMgr s_callbackMgrClient;
static CCallbackMgr s_callbackMgrServer;

class SteamClientProxy final
{
public:
    ~SteamClientProxy();

    template<typename T>
    T *ProxyInterface(T *original, HSteamUser user, HSteamPipe pipe, const char *version);

    void RunFrame(bool gameServer);

    PROXY_FUNC(HSteamPipe, CreateSteamPipe);
    PROXY_FUNC(bool, BReleaseSteamPipe, HSteamPipe hSteamPipe);
    PROXY_FUNC(HSteamUser, ConnectToGlobalUser, HSteamPipe hSteamPipe);
    PROXY_FUNC(HSteamUser, CreateLocalUser, HSteamPipe *phSteamPipe, EAccountType eAccountType);
    PROXY_FUNC(void, ReleaseUser, HSteamPipe hSteamPipe, HSteamUser hUser);
    PROXY_FUNC(ISteamUser *, GetISteamUser, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion);
    PROXY_FUNC(ISteamFriends *, GetISteamFriends, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion);
    PROXY_FUNC(ISteamGameServer *, GetISteamGameServer, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion);
    PROXY_FUNC(ISteamUtils *, GetISteamUtils, HSteamPipe hSteamPipe, const char *pchVersion);
    PROXY_FUNC(ISteamMatchmakingServers *, GetISteamMatchmakingServers, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion);
    PROXY_FUNC(void *, GetISteamGenericInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion);
    PROXY_FUNC(ISteamUserStats *, GetISteamUserStats, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion);

private:
    SteamPipeContext *GetContext(HSteamPipe pipe, HSteamUser user);

    std::map<HSteamPipe, SteamPipeContext> m_pipeContext;
};

SteamPipeContext *SteamClientProxy::GetContext(HSteamPipe pipe, HSteamUser user)
{
    auto it = m_pipeContext.find(pipe);
    if (it == m_pipeContext.end())
    {
        return nullptr;
    }

    assert(!user || it->second.GetUser() == user);
    return &it->second;
}

SteamClientProxy::~SteamClientProxy()
{
    // debug schizo
    assert(m_pipeContext.empty());
}

PROXY_FUNC(HSteamPipe, SteamClientProxy::CreateSteamPipe)
{
    HSteamPipe pipe = original();
    if (pipe)
    {
        auto [it, inserted] = m_pipeContext.try_emplace(pipe, pipe, false, *s_actualSteamClient, s_callbackMgrClient);
        assert(inserted);
    }

    return pipe;
}

PROXY_FUNC(bool, SteamClientProxy::BReleaseSteamPipe, HSteamPipe hSteamPipe)
{
    m_pipeContext.erase(hSteamPipe);
    return original(hSteamPipe);
}

PROXY_FUNC(HSteamUser, SteamClientProxy::ConnectToGlobalUser, HSteamPipe hSteamPipe)
{
    HSteamUser user = original(hSteamPipe);
    if (user)
    {
        SteamPipeContext *context = GetContext(hSteamPipe, 0);
        if (context)
        {
            context->SetUser(user);
        }
    }

    return user;
}

PROXY_FUNC(HSteamUser, SteamClientProxy::CreateLocalUser, HSteamPipe *phSteamPipe, EAccountType eAccountType)
{
    HSteamUser user = original(phSteamPipe, eAccountType);
    if (user)
    {
        assert(*phSteamPipe);

        // now this looks like a gameserver account
        assert(eAccountType == k_EAccountTypeGameServer || eAccountType == k_EAccountTypeAnonGameServer);
        auto [it, inserted] = m_pipeContext.try_emplace(*phSteamPipe, *phSteamPipe, true, *s_actualSteamClient, s_callbackMgrServer);
        assert(inserted);

        it->second.SetUser(user);
    }

    return user;
}

PROXY_FUNC(void, SteamClientProxy::ReleaseUser, HSteamPipe hSteamPipe, HSteamUser hUser)
{
    SteamPipeContext *context = GetContext(hSteamPipe, 0);
    if (context)
    {
        context->ClearUser(hUser);
    }

    original(hSteamPipe, hUser);
}

template<typename T>
T *SteamClientProxy::ProxyInterface(T *original, HSteamUser user, HSteamPipe pipe, const char *version)
{
    if (!original)
    {
        return nullptr;
    }

    SteamPipeContext *context = GetContext(pipe, user);
    if (!context)
    {
        return original;
    }

    T *result = static_cast<T *>(context->GetInterface(version, original));
    return result ? result : original;
}

void SteamClientProxy::RunFrame(bool gameServer)
{
    for (auto &[pipe, context] : m_pipeContext)
    {
        if (context.IsGameServer() == gameServer)
        {
            context.RunFrame();
        }
    }
}

PROXY_FUNC(ISteamUser *, SteamClientProxy::GetISteamUser, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamUser, hSteamPipe, pchVersion), hSteamUser, hSteamPipe, pchVersion);
}

PROXY_FUNC(ISteamFriends *, SteamClientProxy::GetISteamFriends, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamUser, hSteamPipe, pchVersion), hSteamUser, hSteamPipe, pchVersion);
}

PROXY_FUNC(ISteamGameServer *, SteamClientProxy::GetISteamGameServer, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamUser, hSteamPipe, pchVersion), hSteamUser, hSteamPipe, pchVersion);
}

PROXY_FUNC(ISteamUtils *, SteamClientProxy::GetISteamUtils, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamPipe, pchVersion), 0, hSteamPipe, pchVersion);
}

PROXY_FUNC(ISteamMatchmakingServers *, SteamClientProxy::GetISteamMatchmakingServers, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamUser, hSteamPipe, pchVersion), hSteamUser, hSteamPipe, pchVersion);
}

PROXY_FUNC(void *, SteamClientProxy::GetISteamGenericInterface, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamUser, hSteamPipe, pchVersion), hSteamUser, hSteamPipe, pchVersion);
}

PROXY_FUNC(ISteamUserStats *, SteamClientProxy::GetISteamUserStats, HSteamUser hSteamUser, HSteamPipe hSteamPipe, const char *pchVersion)
{
    return ProxyInterface(original(hSteamUser, hSteamPipe, pchVersion), hSteamUser, hSteamPipe, pchVersion);
}

static SteamClientProxy s_steamClientProxy;

// steamclient proxy glue
#include <proxy/steamclientproxy010.h>
#include <proxy/steamclientproxy011.h>
#include <proxy/steamclientproxy012.h>
#include <proxy/steamclientproxy013.h>
#include <proxy/steamclientproxy014.h>
#include <proxy/steamclientproxy015.h>
#include <proxy/steamclientproxy016.h>
#include <proxy/steamclientproxy017.h>
#include <proxy/steamclientproxy018.h>
#include <proxy/steamclientproxy019.h>
#include <proxy/steamclientproxy020.h>
#include <proxy/steamclientproxy023.h>

using CreateInterface_t = void *(*)(const char *, int *);

static CreateInterface_t Og_CreateInterface;

static void *Hk_CreateInterface(const char *name, int *errorCode)
{
    void *result = Og_CreateInterface(name, errorCode);

    if (VersionNameIs(name, "SteamClient"))
    {
        // this assumes the original pointer won't change, which it shouln't
#define CHECK_STEAMCLIENT(version) \
    if (VersionNumberIs(name, #version)) \
    { \
        static SteamClientProxy##version proxy{ static_cast<ISteamClient##version *>(result), &s_steamClientProxy }; \
        return static_cast<ISteamClient##version *>(&proxy); \
    }
        CHECK_STEAMCLIENT(023)
        CHECK_STEAMCLIENT(020)
        CHECK_STEAMCLIENT(019)
        CHECK_STEAMCLIENT(018)
        CHECK_STEAMCLIENT(017)
        CHECK_STEAMCLIENT(016)
        CHECK_STEAMCLIENT(015)
        CHECK_STEAMCLIENT(014)
        CHECK_STEAMCLIENT(013)
        CHECK_STEAMCLIENT(012)
        CHECK_STEAMCLIENT(011)
        CHECK_STEAMCLIENT(010)
#undef CHECK_STEAMCLIENT
        Platform::Print("Can't hook {}\n", name);
    }

    return result;
}

static void (*Og_SteamAPI_RegisterCallback)(class CCallbackBase *pCallback, int iCallback);
static void (*Og_SteamAPI_UnregisterCallback)(class CCallbackBase *pCallback);
static void (*Og_SteamAPI_RunCallbacks)();
static void (*Og_SteamGameServer_RunCallbacks)();

static void Hk_SteamAPI_RegisterCallback(class CCallbackBase *pCallback, int iCallback)
{
    if (CCallbackMgr::IsGameServer(pCallback))
    {
        s_callbackMgrServer.RegisterCallback(pCallback, iCallback);
    }
    else
    {
        s_callbackMgrClient.RegisterCallback(pCallback, iCallback);
    }

    if (!CCallbackMgr::SkipSteamRegistration(iCallback))
    {
        Og_SteamAPI_RegisterCallback(pCallback, iCallback);
    }
}

static void Hk_SteamAPI_UnregisterCallback(class CCallbackBase *pCallback)
{
    if (CCallbackMgr::IsGameServer(pCallback))
    {
        s_callbackMgrServer.UnregisterCallback(pCallback);
    }
    else
    {
        s_callbackMgrClient.UnregisterCallback(pCallback);
    }

    if (!CCallbackMgr::SkipSteamRegistration(pCallback->GetICallback()))
    {
        Og_SteamAPI_UnregisterCallback(pCallback);
    }
}

static void Hk_SteamAPI_RunCallbacks()
{
    s_steamClientProxy.RunFrame(false);

    static bool runningCallbacks;
    if (!runningCallbacks)
    {
        runningCallbacks = true;
        s_callbackMgrClient.RunCallbacks();
        Og_SteamAPI_RunCallbacks();
        runningCallbacks = false;
    }
}

static void Hk_SteamGameServer_RunCallbacks()
{
    s_steamClientProxy.RunFrame(true);

    static bool runningCallbacks;
    if (!runningCallbacks)
    {
        runningCallbacks = true;
        s_callbackMgrServer.RunCallbacks();
        Og_SteamGameServer_RunCallbacks();
        runningCallbacks = false;
    }
}

// shows a message box and exits on failure
static void HookCreate(const char *name, void *target, void *hook, void **bridge)
{
    funchook_t *funchook = funchook_create();
    if (!funchook)
    {
        // unlikely (only allocates) but check anyway
        Platform::Error("funchook_create failed for %s", name);
    }

    void *temp = target;
    int result = funchook_prepare(funchook, &temp, hook);
    if (result != 0)
    {
        Platform::Error("funchook_prepare failed for %s: %s", name, funchook_error_message(funchook));
    }

    *bridge = temp;

    result = funchook_install(funchook, 0);
    if (result != 0)
    {
        Platform::Error("funchook_install failed for %s: %s", name, funchook_error_message(funchook));
    }
}

#define INLINE_HOOK(a) HookCreate(#a, reinterpret_cast<void *>(p##a), reinterpret_cast<void *>(Hk_##a), reinterpret_cast<void **>(&Og_##a));

void SteamHookInstall(bool dedicated)
{
    AppId::Init();

    // Steam sets this when it starts the game, set it anyway for manual launches
    Platform::SetEnvVar("SteamAppId", std::to_string(AppId::GetOverride()).c_str());
    Platform::SetEnvVar("SteamGameId", std::to_string(AppId::GetOverride()).c_str());

    // load the game's steam api and don't free it so our hooks persist
    void *steamApi = Platform::LoadGameLibrary(STEAM_API_LIB);
    if (!steamApi)
    {
        Platform::Error("Could not load steam_api64.dll from the game folder");
    }

    // csgo_gc initialized the steam api here just to find steamclient. The running
    // Steam client publishes the path in the registry, which is also where steam_api64
    // looks, so read it from there and leave the steam api alone until the game inits it.
    wchar_t steamClientPath[4096];
    if (!Platform::SteamClientPath(steamClientPath, sizeof(steamClientPath), dedicated))
    {
        Platform::Error("Could not find steamclient64.dll.\n"
                        "- Ensure that Steam is running and you are logged in.\n"
                        "- Restart Steam and try again.\n"
                        "- A dedicated server (-dedicated) can run without Steam: put steamcmd's\n"
                        "  steamclient64.dll next to cs2_gc.exe.");
    }

    // load steamclient and don't free it so our hooks persist. steam_api64 loads
    // the same path later and gets this module (with our hook) back.
    void *steamClient = Platform::LoadDynamicLibrary(steamClientPath);
    if (!steamClient)
    {
        Platform::Error("Could not load steamclient64.dll");
    }

    auto pCreateInterface = reinterpret_cast<CreateInterface_t>(Platform::GetSymbol(steamClient, "CreateInterface"));
    if (!pCreateInterface)
    {
        Platform::Error("Could not get steamclient factory");
    }

    // get the actual steamclient instance, gets used if we want to actually use any steam interfaces
    s_actualSteamClient = static_cast<ISteamClient *>(pCreateInterface(STEAMCLIENT_INTERFACE_VERSION, nullptr));
    if (!s_actualSteamClient)
    {
        Platform::Error("Could not get %s", STEAMCLIENT_INTERFACE_VERSION);
    }

    // see if we should write funchook logs to file
    static std::string funchookLog = Platform::DataPath("gc_log.txt");
    if (GetConfig().m_logOutput == LogOutputFile)
    {
        // same file as gc logs... both will open, append, and close so it's fine
        funchook_set_debug_file(funchookLog.c_str());
    }

    // hook for steamclient proxy
    INLINE_HOOK(CreateInterface);

#define GET_STEAM_API_FUNC_CHECKED(name) \
    void *p##name = Platform::GetSymbol(steamApi, #name); \
    if (!p##name) \
    { \
        Platform::Error("Could not get %s", #name); \
    }
    GET_STEAM_API_FUNC_CHECKED(SteamAPI_RegisterCallback);
    GET_STEAM_API_FUNC_CHECKED(SteamAPI_UnregisterCallback);
    GET_STEAM_API_FUNC_CHECKED(SteamAPI_RunCallbacks);
    GET_STEAM_API_FUNC_CHECKED(SteamGameServer_RunCallbacks);
#undef GET_STEAM_API_FUNC_CHECKED

    // steam api hooks for gc callbacks
    INLINE_HOOK(SteamAPI_RegisterCallback);
    INLINE_HOOK(SteamAPI_UnregisterCallback);
    INLINE_HOOK(SteamAPI_RunCallbacks);
    INLINE_HOOK(SteamGameServer_RunCallbacks);
}

// these are here for the networking code, just bounce off the trampolines

S_API void S_CALLTYPE SteamAPI_RegisterCallback(class CCallbackBase *pCallback, int iCallback)
{
    return Og_SteamAPI_RegisterCallback(pCallback, iCallback);
}

S_API void S_CALLTYPE SteamAPI_UnregisterCallback(class CCallbackBase *pCallback)
{
    return Og_SteamAPI_UnregisterCallback(pCallback);
}

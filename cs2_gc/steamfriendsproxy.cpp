#include "stdafx.h"
#include "steamfriendsproxy.h"
#include "gc_shared.h"
#include "steampipecontext.h"

SteamFriendsProxy::SteamFriendsProxy(SteamPipeContext &context)
    : m_context{ context }
{
}

PROXY_FUNC(bool, SteamFriendsProxy::SetRichPresence, const char *pchKey, const char *pchValue)
{
    if (pchKey)
    {
        // "key\0value", a null value deletes the key
        std::string buffer{ pchKey };
        buffer.push_back('\0');
        buffer.append(pchValue ? pchValue : "");
        m_context.PostToGC(GCEvent::RichPresence, 0, buffer.data(), static_cast<uint32_t>(buffer.size()));
    }

    return original(pchKey, pchValue);
}

PROXY_FUNC(void, SteamFriendsProxy::ClearRichPresence)
{
    m_context.PostToGC(GCEvent::RichPresence, 1, nullptr, 0);
    original();
}

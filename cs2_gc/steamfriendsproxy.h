#pragma once

#include <proxy/steamproxy.h>
#include <steam/isteamfriends.h>

class SteamPipeContext;

// the game keeps its Steam rich presence up to date (game:state, game:mode, game:map,
// game:score...) on any server, bots and community ones included: that's how ClientGC
// sees matches start and end and hands out match xp
class SteamFriendsProxy final
{
public:
    SteamFriendsProxy(SteamPipeContext &context);

    PROXY_FUNC(bool, SetRichPresence, const char *pchKey, const char *pchValue);
    PROXY_FUNC(void, ClearRichPresence);

private:
    SteamPipeContext &m_context;
};

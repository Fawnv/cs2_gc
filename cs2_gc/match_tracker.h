#pragma once

// Follows the game's Steam rich presence to see matches start and end. The client sets it
// on any server (client.dll, the function that writes game:state): offline with bots
// (game:server "offline"), on community servers ("community") and so on, a batch of keys
// every update in this order: game:state ("game" or "lobby"), game:mode, game:map,
// game:score ("[ 8 : 5 ]", the player's team first; deleted while it's 0:0), game:server.
// A match ends when the player goes back to the lobby, the map changes, the score starts
// over (a new match on the same map) or the game closes.
struct MatchSummary
{
    std::string mode; // casual, competitive, premier, scrimcomp2v2 (wingman), deathmatch...
    std::string map;
    std::string server; // offline, community...
    uint32_t roundsWon{};
    uint32_t roundsLost{};
    uint32_t seconds{};
    bool serverMovedOn{}; // the map changed or the score started over while the player was there
};

// was the match played to the end? As in CS2, leaving early earns nothing. The server moving
// on means it was over; otherwise the score (or time) when the player left has to show the end,
// by the game's own gamemode_*.cfg: casual and retakes 15 rounds (8 wins clinch), competitive
// and premier 24 (13), wingman 16 (9), deathmatch 10 minutes. Other modes count only when
// the server moves on (community servers change the map after a match)
bool MatchFinished(const MatchSummary &match);

class MatchTracker
{
public:
    // one SetRichPresence call, returns the match that just ended if there was one
    std::optional<MatchSummary> Update(std::string_view key, std::string_view value, uint32_t now);

    // ClearRichPresence, or the game is closing
    std::optional<MatchSummary> End(uint32_t now);

    bool InMatch() const { return m_inMatch; }

    // matches shorter than this give nothing (a quick look at a server, a crash on load)
    static constexpr uint32_t MinSeconds = 60;

private:
    void Start(uint32_t now);
    std::optional<MatchSummary> Finish(uint32_t now, bool serverMovedOn = false);

    bool m_inMatch{};
    uint32_t m_started{};
    MatchSummary m_match;
};

// match xp before the weekly bonus, CS2's rates: 30 per round won in competitive and premier,
// 15 in wingman; deathmatch and casual pay by score in CS2, which rich presence doesn't have,
// so they go by time (up to 200 and 350 xp, as in CS2)
uint32_t MatchBaseXp(const MatchSummary &match);

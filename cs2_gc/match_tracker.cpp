#include "stdafx.h"
#include "match_tracker.h"
#include "platform.h"

// "[ 8 : 5 ]" -> 8, 5; anything else (or a deleted key) is 0:0
static std::pair<uint32_t, uint32_t> ParseScore(std::string_view value)
{
    uint32_t numbers[2]{};
    int found = 0;
    for (size_t i = 0; i < value.size() && found < 2;)
    {
        if (value[i] < '0' || value[i] > '9')
        {
            i++;
            continue;
        }

        uint32_t number = 0;
        auto [end, error] = std::from_chars(value.data() + i, value.data() + value.size(), number);
        if (error != std::errc{})
        {
            return {};
        }
        numbers[found++] = number;
        i = static_cast<size_t>(end - value.data());
    }

    return found == 2 ? std::pair{ numbers[0], numbers[1] } : std::pair<uint32_t, uint32_t>{};
}

void MatchTracker::Start(uint32_t now)
{
    m_inMatch = true;
    m_started = now;
    m_match.roundsWon = 0;
    m_match.roundsLost = 0;
    m_match.seconds = 0;
}

std::optional<MatchSummary> MatchTracker::Finish(uint32_t now, bool serverMovedOn)
{
    if (!m_inMatch)
    {
        return std::nullopt;
    }

    m_inMatch = false;
    MatchSummary match = m_match;
    match.seconds = now > m_started ? now - m_started : 0;
    match.serverMovedOn = serverMovedOn;

    Platform::Print("Match: {} on {} ({}) over after {} s, rounds {}:{}\n", match.mode, match.map, match.server,
        match.seconds, match.roundsWon, match.roundsLost);

    if (match.server == "watch" || match.seconds < MinSeconds)
    {
        // a demo or GOTV, or too short to count
        return std::nullopt;
    }

    return match;
}

std::optional<MatchSummary> MatchTracker::Update(std::string_view key, std::string_view value, uint32_t now)
{
    std::optional<MatchSummary> ended;

    if (key == "game:state")
    {
        bool inGame = (value == "game");
        if (inGame && !m_inMatch)
        {
            m_match = {};
            Start(now);
            Platform::Print("Match: started\n");
        }
        else if (!inGame && m_inMatch)
        {
            ended = Finish(now);
        }
        return ended;
    }

    if (!m_inMatch)
    {
        return std::nullopt;
    }

    if (key == "game:mode")
    {
        m_match.mode = value;
    }
    else if (key == "game:server")
    {
        m_match.server = value;
    }
    else if (key == "game:map")
    {
        if (!value.empty() && !m_match.map.empty() && value != m_match.map)
        {
            // the server moved on to the next map, the match is over
            ended = Finish(now, true);
            Start(now);
            Platform::Print("Match: next map {}\n", value);
        }

        if (!value.empty())
        {
            m_match.map = value;
        }
    }
    else if (key == "game:score")
    {
        auto [won, lost] = ParseScore(value);
        if (won + lost < m_match.roundsWon + m_match.roundsLost)
        {
            // the score started over: a new match on the same map
            ended = Finish(now, true);
            Start(now);
            Platform::Print("Match: started over on {}\n", m_match.map);
        }

        m_match.roundsWon = won;
        m_match.roundsLost = lost;
    }

    return ended;
}

std::optional<MatchSummary> MatchTracker::End(uint32_t now)
{
    return Finish(now);
}

bool MatchFinished(const MatchSummary &match)
{
    if (match.serverMovedOn)
    {
        return true;
    }

    // rounds a match lasts and the wins that clinch it (mp_maxrounds, mp_match_can_clinch)
    struct RoundLimits
    {
        std::string_view mode;
        uint32_t maxRounds;
        uint32_t clinch;
    };
    static constexpr RoundLimits Limits[] = {
        { "casual", 15, 8 },
        { "retakes", 15, 8 },
        { "competitive", 24, 13 },
        { "premier", 24, 13 },
        { "scrimcomp5v5", 24, 13 },
        { "scrimcomp2v2", 16, 9 },
    };

    for (const RoundLimits &limits : Limits)
    {
        if (match.mode == limits.mode)
        {
            uint32_t most = (std::max)(match.roundsWon, match.roundsLost);
            return most >= limits.clinch || match.roundsWon + match.roundsLost >= limits.maxRounds;
        }
    }

    if (match.mode == "deathmatch")
    {
        // mp_roundtime 10, a little slack for the last update before the end
        return match.seconds >= 9 * 60 + 30;
    }

    return false;
}

uint32_t MatchBaseXp(const MatchSummary &match)
{
    std::string_view mode = match.mode;
    uint32_t minutes = match.seconds / 60;

    if (mode == "competitive" || mode == "premier" || mode == "scrimcomp5v5")
    {
        return 30 * match.roundsWon;
    }

    if (mode == "scrimcomp2v2")
    {
        return 15 * match.roundsWon;
    }

    if (mode == "deathmatch")
    {
        // CS2: score / 5, a 10 minute match tops out around 200
        return (std::min)(15 * minutes, 200u);
    }

    if (mode == "training" || mode == "cooperative" || mode == "coopmission")
    {
        return 0;
    }

    // casual and the rest (arms race, retakes, community server modes): CS2 pays by score up to 350
    return (std::min)(10 * minutes, 350u);
}

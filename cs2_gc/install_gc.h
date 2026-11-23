#pragma once

#ifdef _WIN32
typedef wchar_t gc_char_t;
#else
typedef char gc_char_t;
#endif

// Entry point used by the launcher. Must be called after tier0 is loaded
// and before Source2Main, so the Steam hooks are in place when the engine
// initializes the Steam API.
struct GCInstallParams
{
    const gc_char_t *gameBinDir; // <cs2>/game/bin/win64 or linuxsteamrt64
    const gc_char_t *gameDir;    // <cs2>/game/csgo
    const gc_char_t *dataDir;    // <launcher dir>/cs2_gc
    bool dedicated;
};

void InstallGC(const GCInstallParams &params);

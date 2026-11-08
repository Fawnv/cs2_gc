#pragma once

// Entry point used by the launcher. Must be called after tier0.dll is loaded
// and before Source2Main, so the Steam hooks are in place when the engine
// initializes the Steam API.
struct GCInstallParams
{
    const wchar_t *gameBinDir; // <cs2>\game\bin\win64
    const wchar_t *gameDir; // <cs2>\game\csgo
    const wchar_t *dataDir; // <launcher dir>\cs2_gc
    bool dedicated;
};

void InstallGC(const GCInstallParams &params);

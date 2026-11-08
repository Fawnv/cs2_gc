#include "stdafx.h"
#include "platform.h"
#include "config.h"
#include "steam_hook.h"
#include "install_gc.h"
#include "mod_loader.h"

void InstallGC(const GCInstallParams &params)
{
    Platform::SetPaths(params.gameBinDir, params.gameDir, params.dataDir);
    Platform::Initialize();
    LoadConfig();
    SteamHookInstall(params.dedicated);
    ModLoader::Install();
}

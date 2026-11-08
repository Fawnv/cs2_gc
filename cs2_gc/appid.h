// CS2 always runs as app 730. csgo_gc needed app id overrides for the
// standalone CS:GO app (4465480); cs2_gc keeps the same API but never
// rewrites steam.inf or anything else in the game folder.
#pragma once

namespace AppId
{

constexpr uint32_t CS2 = 730;

// logs the app id, nothing else
void Init();

// always 730
uint32_t GetOverride();

// always true: no serverbrowser patch or stats spoofing needed
bool IsOriginal();

}

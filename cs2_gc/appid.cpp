#include "stdafx.h"
#include "appid.h"

namespace AppId
{

void Init()
{
    Platform::Print("Using app id {}\n", CS2);
}

uint32_t GetOverride()
{
    return CS2;
}

bool IsOriginal()
{
    return true;
}

}

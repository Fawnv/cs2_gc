#include "stdafx.h"
#include "platform.h"
#include "config.h" // yuck
#include <windows.h>

namespace Platform
{

static std::wstring s_gameBinDir;
static std::string s_gameDir; // utf-8
static std::string s_dataDir; // utf-8

static std::string ToUtf8(std::wstring_view wide)
{
    if (wide.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

static std::wstring ToWide(std::string_view utf8)
{
    if (utf8.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

void SetPaths(const wchar_t *gameBinDir, const wchar_t *gameDir, const wchar_t *dataDir)
{
    s_gameBinDir = gameBinDir;
    s_gameDir = ToUtf8(gameDir);
    s_dataDir = ToUtf8(dataDir);
}

std::string DataPath(std::string_view name)
{
    std::string path = s_dataDir;
    if (!path.empty())
    {
        path.push_back('\\');
    }

    path.append(name);
    return path;
}

std::string GamePath(std::string_view name)
{
    std::string path = s_gameDir;
    if (!path.empty())
    {
        path.push_back('\\');
    }

    path.append(name);
    return path;
}

FILE *OpenFile(const std::string &utf8Path, const char *mode)
{
    return _wfopen(ToWide(utf8Path).c_str(), ToWide(mode).c_str());
}

using ConColorMsg_t = void (*)(const uint8_t *, const char *, ...);
static ConColorMsg_t s_ConColorMsg;

void Initialize()
{
    // remove the old log file
    RemoveFile(DataPath("gc_log.txt"));

    HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
    if (tier0)
    {
        // void ConColorMsg(const Color &, const char *, ...), x64 mangling
        s_ConColorMsg = (ConColorMsg_t)GetProcAddress(tier0, "?ConColorMsg@@YAXAEBVColor@@PEBDZZ");
    }
}

void PrintV(std::string_view fmt, std::format_args args)
{
    LogOutput logOutput = GetConfig().m_logOutput;
    if (logOutput <= LogOutputNone)
    {
        // no logging
        return;
    }

    std::string buffer = std::vformat(fmt, args);

    if (s_ConColorMsg)
    {
        uint8_t color[4] = { 0, 255, 128, 255 };
        s_ConColorMsg(color, "[GC] %s", buffer.c_str());
    }
    else
    {
        // no game console (self test, or tier0 missing): debugger + stdout
        OutputDebugStringA(buffer.c_str());
        fputs(buffer.c_str(), stdout);
    }

    // optionally also log to file
    if (logOutput >= LogOutputFile)
    {
        // timestamp it
        auto now = std::chrono::system_clock::now();
        std::string nowString = std::format("{:%H:%M:%S}", now);

        FILE *f = OpenFile(DataPath("gc_log.txt"), "a");
        if (f)
        {
            fprintf(f, "[%s] %s", nowString.c_str(), buffer.c_str());
            fclose(f);
        }
    }
}

void Error(const char *format, ...)
{
    va_list ap;
    char buffer[4096];

    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    MessageBoxW(nullptr, ToWide(buffer).c_str(), L"cs2_gc", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

bool SteamClientPath(void *buffer, size_t bufferSize, bool dedicated)
{
    wchar_t *out = reinterpret_cast<wchar_t *>(buffer);
    DWORD outChars = static_cast<DWORD>(bufferSize / sizeof(wchar_t));

    // already loaded? (we get called before steam_api64 normally loads it)
    HMODULE steamclient = GetModuleHandleW(L"steamclient64.dll");
    if (steamclient)
    {
        DWORD result = GetModuleFileNameW(steamclient, out, outChars);
        return (result > 0 && result < outChars);
    }

    // the running Steam client publishes its dll path here, steam_api64 reads the same value
    DWORD size = static_cast<DWORD>(bufferSize);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Valve\\Steam\\ActiveProcess",
        L"SteamClientDll64",
        RRF_RT_REG_SZ,
        nullptr,
        out,
        &size);

    if (status == ERROR_SUCCESS && out[0] != L'\0' && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
    {
        return true;
    }

    if (!dedicated)
    {
        return false;
    }

    // a dedicated server without Steam: the copy steamcmd puts in the game, or one next to cs2_gc.exe
    wchar_t exePath[MAX_PATH];
    DWORD exeLength = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir{ exePath, exeLength };
    exeDir.resize(exeDir.find_last_of(L"\\/") + 1);
    for (const std::wstring &candidate : { s_gameBinDir + L"\\steamclient64.dll", exeDir + L"steamclient64.dll" })
    {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES && candidate.size() < outChars)
        {
            wcscpy_s(out, outChars, candidate.c_str());
            return true;
        }
    }

    return false;
}

void *LoadDynamicLibrary(const void *pathBuffer)
{
    const wchar_t *path = reinterpret_cast<const wchar_t *>(pathBuffer);
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

void *LoadGameLibrary(const wchar_t *name)
{
    std::wstring path = s_gameBinDir;
    path.push_back(L'\\');
    path.append(name);
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

void *GetSymbol(void *handle, const char *symbol)
{
    return GetProcAddress(static_cast<HMODULE>(handle), symbol);
}

void SetEnvVar(const char *name, const char *value)
{
    SetEnvironmentVariableA(name, value);
}

static void *Q_memmem(const void *_haystack, size_t haystack_len, const void *_needle, size_t needle_len)
{
    uint8_t *haystack = (uint8_t *)_haystack;
    uint8_t *needle = (uint8_t *)_needle;

    uint8_t *ptr = haystack;
    uint8_t *end = haystack + haystack_len;

    while ((size_t)(end - ptr) >= needle_len)
    {
        ptr = (uint8_t *)memchr(ptr, *needle, end - ptr);
        if (!ptr)
        {
            return NULL;
        }

        if (!memcmp(ptr, needle, needle_len))
        {
            return ptr;
        }

        ptr++;
    }

    return NULL;
}

bool PatchGraffitiPublicKey(std::string_view moduleName, const void *original, const void *replacement, size_t size)
{
    std::string actualModuleName;
    actualModuleName.assign(moduleName);
    actualModuleName.append(".dll");

    HMODULE module = GetModuleHandleA(actualModuleName.c_str());
    if (!module)
    {
        return false;
    }

    LPBYTE moduleBase = reinterpret_cast<LPBYTE>(module);
    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(moduleBase);
    PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(moduleBase + dosHeader->e_lfanew);
    DWORD moduleSize = ntHeaders->OptionalHeader.SizeOfImage;

    void *address = Q_memmem(moduleBase, moduleSize, original, size);
    if (!address)
    {
        return false;
    }

    DWORD oldProtect;
    if (VirtualProtect(address, size, PAGE_READWRITE, &oldProtect))
    {
        memcpy(address, replacement, size);
        VirtualProtect(address, size, oldProtect, &oldProtect);
        return true;
    }

    return false;
}

bool RemoveFile(const std::string &utf8Path)
{
    return DeleteFileW(ToWide(utf8Path).c_str()) ? true : false;
}

} // namespace Platform

// cs2_gc launcher: does what cs2.exe (Valve's source2launcher) does, plus
// installs the GC hooks before the engine starts:
//   - finds the CS2 install (or uses the folder it was copied into)
//   - always runs the game with -insecure (VAC off, no official servers)
//   - loads tier0 + engine2 from <cs2>\game\bin\win64
//   - installs cs2_gc
//   - calls Source2Main(hInstance, hPrev, cmdline, nShowCmd, "<bin dir>", "csgo")
//
// Nothing in the CS2 folder is modified. All cs2_gc files live in
// <launcher dir>\cs2_gc (or -gcdata <dir>).
#include <windows.h>
#include <shlwapi.h>
#include <wchar.h>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "../cs2_gc/install_gc.h"

#define DLL_EXPORT extern "C" __declspec(dllexport)

DLL_EXPORT DWORD NvOptimusEnablement = 1;
DLL_EXPORT int AmdPowerXpressRequestHighPerformance = 1;

// cs2.exe exports these and the engine looks them up on the main module.
// We never allow secure mode: a modified client must not be VAC secured.
DLL_EXPORT bool BSecureAllowed(void *, int, int)
{
    return false;
}

DLL_EXPORT int CountFilesCompletedTrustCheck()
{
    return 0;
}

DLL_EXPORT int CountFilesNeedTrustCheck()
{
    return 0;
}

DLL_EXPORT int CountItemsToReport()
{
    return 0;
}

DLL_EXPORT int GetTotalFilesLoaded()
{
    return 0;
}

// tier0 exports, resolved at runtime (same ones cs2.exe uses)
using Plat_LoadModule_t = void *(*)(const char *name, int *error, void *reserved);
using Plat_FindModuleByName_t = void *(*)(const char *name, void *, void *, void *);
using Plat_GetModuleProcAddress_t = void *(*)(void *module, const char *name);

using Source2Main_t = int (*)(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd, const char *baseDir, const char *game);

static void ErrorMessageBox(const wchar_t *format, ...)
{
    va_list ap;
    wchar_t buffer[4096];

    va_start(ap, format);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, format, ap);
    va_end(ap);

    MessageBoxW(nullptr, buffer, L"cs2_gc", MB_OK | MB_ICONERROR);
}

static std::wstring LastErrorString()
{
    wchar_t buffer[1024];
    DWORD error = GetLastError();

    DWORD result = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
        nullptr, error, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);

    if (!result)
    {
        _snwprintf_s(buffer, std::size(buffer), _TRUNCATE, L"Unknown error (%lu)", error);
    }

    return buffer;
}

static std::wstring DirName(std::wstring path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        path.resize(slash);
    }

    return path;
}

static bool FileExists(const std::wstring &path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string ToUtf8(const std::wstring &wide)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

// <root>\game\bin\win64 if <root> looks like a CS2 install
static bool IsCS2Root(const std::wstring &root)
{
    return FileExists(root + L"\\game\\bin\\win64\\engine2.dll")
        && FileExists(root + L"\\game\\csgo\\pak01_dir.vpk");
}

// value of -name "<value>" from the command line
static std::wstring GetParam(int argc, wchar_t **argv, const wchar_t *name)
{
    for (int i = 1; i + 1 < argc; i++)
    {
        if (!_wcsicmp(argv[i], name))
        {
            return argv[i + 1];
        }
    }

    return {};
}

static bool HasParam(int argc, wchar_t **argv, const wchar_t *name)
{
    for (int i = 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], name))
        {
            return true;
        }
    }

    return false;
}

// steamapps/libraryfolders.vdf -> every "path" entry
static std::vector<std::wstring> SteamLibraries()
{
    std::vector<std::wstring> result;

    wchar_t steamPath[MAX_PATH];
    DWORD size = sizeof(steamPath);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steamPath, &size) != ERROR_SUCCESS)
    {
        return result;
    }

    std::wstring steam = steamPath;
    result.push_back(steam);

    std::wstring vdfPath = steam + L"\\steamapps\\libraryfolders.vdf";
    HANDLE file = CreateFileW(vdfPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    std::string text;
    char chunk[4096];
    DWORD read;
    while (ReadFile(file, chunk, sizeof(chunk), &read, nullptr) && read)
    {
        text.append(chunk, read);
    }
    CloseHandle(file);

    // "path"		"D:\\SteamLibrary"
    size_t pos = 0;
    while ((pos = text.find("\"path\"", pos)) != std::string::npos)
    {
        pos += 6;
        size_t open = text.find('"', pos);
        if (open == std::string::npos)
        {
            break;
        }

        std::string value;
        size_t i = open + 1;
        for (; i < text.size() && text[i] != '"'; i++)
        {
            if (text[i] == '\\' && i + 1 < text.size())
            {
                i++;
            }
            value.push_back(text[i]);
        }
        pos = i;

        int wideSize = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        std::wstring wide(wideSize > 0 ? wideSize - 1 : 0, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), wideSize);
        result.push_back(wide);
    }

    return result;
}

static std::wstring FindCS2Root(const std::wstring &exeDir, int argc, wchar_t **argv)
{
    // explicit -cs2path "<install dir>"
    std::wstring param = GetParam(argc, argv, L"-cs2path");
    if (!param.empty())
    {
        return IsCS2Root(param) ? param : std::wstring{};
    }

    // copied into <cs2>\game\bin\win64 next to cs2.exe
    std::wstring candidate = DirName(DirName(DirName(exeDir)));
    if (IsCS2Root(candidate))
    {
        return candidate;
    }

    for (const std::wstring &library : SteamLibraries())
    {
        candidate = library + L"\\steamapps\\common\\Counter-Strike Global Offensive";
        if (IsCS2Root(candidate))
        {
            return candidate;
        }
    }

    return {};
}

// tier0 reads the process command line, so -insecure has to be on the real one.
// If it's missing, start ourselves again with it and exit.
static bool RelaunchInsecure(const std::wstring &exePath)
{
    std::wstring commandLine = GetCommandLineW();
    commandLine.append(L" -insecure");

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &processInfo))
    {
        ErrorMessageBox(L"Could not restart with -insecure:\n%ls", LastErrorString().c_str());
        return false;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    wchar_t exePathBuffer[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, exePathBuffer, static_cast<DWORD>(std::size(exePathBuffer)));
    if (!length || length == std::size(exePathBuffer))
    {
        ErrorMessageBox(L"GetModuleFileName failed:\n%ls", LastErrorString().c_str());
        return 1;
    }

    std::wstring exePath = exePathBuffer;
    std::wstring exeDir = DirName(exePath);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (HasParam(argc, argv, L"-trusted"))
    {
        ErrorMessageBox(L"cs2_gc never runs in trusted/secure mode. Remove -trusted from the launch options.");
        return 1;
    }

    if (!HasParam(argc, argv, L"-insecure"))
    {
        return RelaunchInsecure(exePath) ? 0 : 1;
    }

    std::wstring root = FindCS2Root(exeDir, argc, argv);
    if (root.empty())
    {
        ErrorMessageBox(L"Could not find Counter-Strike 2.\n\n"
                        L"Pass the install folder explicitly, e.g.\n"
                        L"cs2_gc.exe -cs2path \"D:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\"");
        return 1;
    }

    std::wstring binDir = root + L"\\game\\bin\\win64";
    std::wstring gameDir = root + L"\\game\\csgo";

    std::wstring dataDir = GetParam(argc, argv, L"-gcdata");
    if (dataDir.empty())
    {
        dataDir = exeDir + L"\\cs2_gc";
    }

    CreateDirectoryW(dataDir.c_str(), nullptr);

    bool dedicated = HasParam(argc, argv, L"-dedicated");
    LocalFree(argv);

    // same environment cs2.exe sets up
    SetEnvironmentVariableW(L"Source2Launcher", L"1");

    // implicit loads by name (steam_api64.dll, tier0.dll...) must come from the game
    SetDllDirectoryW(binDir.c_str());
    {
        std::wstring path = binDir + L";";
        wchar_t current[32767];
        DWORD currentLength = GetEnvironmentVariableW(L"PATH", current, static_cast<DWORD>(std::size(current)));
        if (currentLength && currentLength < std::size(current))
        {
            path.append(current);
        }
        SetEnvironmentVariableW(L"PATH", path.c_str());
    }

    // Copied next to cs2.exe: same as double clicking cs2.exe (known to work), the engine
    // finds csgo\gameinfo.gi relative to the exe. From anywhere else the engine falls back
    // to "csgo\gameinfo.gi" relative to the working directory, so that has to be <cs2>\game.
    bool besideCs2 = !_wcsicmp(exeDir.c_str(), binDir.c_str());
    if (besideCs2)
    {
        SetCurrentDirectoryW(binDir.c_str());
    }
    else
    {
        std::wstring gameRoot = root + L"\\game";
        SetCurrentDirectoryW(gameRoot.c_str());
    }

    std::wstring tier0Path = binDir + L"\\tier0.dll";
    HMODULE tier0 = LoadLibraryExW(tier0Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!tier0)
    {
        ErrorMessageBox(L"Could not load '%ls':\n%ls", tier0Path.c_str(), LastErrorString().c_str());
        return 1;
    }

    auto Plat_LoadModule = reinterpret_cast<Plat_LoadModule_t>(GetProcAddress(tier0, "Plat_LoadModule"));
    auto Plat_GetModuleProcAddress = reinterpret_cast<Plat_GetModuleProcAddress_t>(GetProcAddress(tier0, "Plat_GetModuleProcAddress"));

    // cs2.exe goes through tier0 so the module gets registered there
    void *engine = nullptr;
    if (Plat_LoadModule)
    {
        int error = 0;
        engine = Plat_LoadModule("engine2", &error, nullptr);
    }

    void *source2Main = nullptr;
    if (engine && Plat_GetModuleProcAddress)
    {
        source2Main = Plat_GetModuleProcAddress(engine, "Source2Main");
    }

    if (!source2Main)
    {
        // fall back to plain LoadLibrary with the absolute path
        std::wstring enginePath = binDir + L"\\engine2.dll";
        HMODULE engineModule = LoadLibraryExW(enginePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!engineModule)
        {
            ErrorMessageBox(L"Could not load '%ls':\n%ls", enginePath.c_str(), LastErrorString().c_str());
            return 1;
        }

        source2Main = GetProcAddress(engineModule, "Source2Main");
        if (!source2Main)
        {
            ErrorMessageBox(L"Could not find Source2Main in '%ls'", enginePath.c_str());
            return 1;
        }
    }

    GCInstallParams params{};
    params.gameBinDir = binDir.c_str();
    params.gameDir = gameDir.c_str();
    params.dataDir = dataDir.c_str();
    params.dedicated = dedicated;
    InstallGC(params);

    // cs2.exe passes the directory of the exe (game\bin\win64) as UTF-8
    std::string baseDir = ToUtf8(binDir);

    static_cast<Source2Main_t>(source2Main)(hInstance, hPrevInstance, lpCmdLine, nShowCmd, baseDir.c_str(), "csgo");

    // cs2.exe ignores the result too
    return 0;
}

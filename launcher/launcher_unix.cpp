#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/limits.h>

#include "../cs2_gc/install_gc.h"

#define DLL_EXPORT extern "C" __attribute__((visibility("default")))

extern "C" {
__attribute__((visibility("default"))) int NvOptimusEnablement = 1;
__attribute__((visibility("default"))) int AmdPowerXpressRequestHighPerformance = 1;
}

// cs2 exports these and the engine looks them up on the main module.
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

// tier0 exports, resolved at runtime (same ones cs2 uses)
using Plat_LoadModule_t = void *(*)(const char *name, int *error, void *reserved);
using Plat_GetModuleProcAddress_t = void *(*)(void *module, const char *name);

using Source2Main_t = int (*)(int argc, char **argv, const char *baseDir, const char *game);

static void ErrorMessageBox(const char *format, ...)
{
    va_list ap;
    char message[4096];

    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);

    fprintf(stderr, "cs2_gc: %s\n", message);

    // Try desktop notification if DISPLAY is set
    if (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))
    {
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "zenity --error --text='%s' 2>/dev/null || kdialog --error '%s' 2>/dev/null", message, message);
        int res = system(cmd);
        (void)res;
    }
}

static std::string GetExecutablePath()
{
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
    {
        buf[len] = '\0';
        return std::string(buf);
    }
    return {};
}

static std::string DirName(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
    {
        return {};
    }
    return path.substr(0, slash);
}

static bool FileExists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool IsCS2Root(const std::string &dir)
{
    return FileExists(dir + "/game/csgo/gameinfo.gi");
}

static std::string GetParam(int argc, char **argv, const char *name)
{
    size_t nameLen = strlen(name);
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], name))
        {
            if (i + 1 < argc)
            {
                return argv[i + 1];
            }
            return {};
        }

        if (!strncmp(argv[i], name, nameLen) && argv[i][nameLen] == '=')
        {
            return &argv[i][nameLen + 1];
        }
    }
    return {};
}

static bool HasFlag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], name))
        {
            return true;
        }
    }
    return false;
}

static std::vector<std::string> SteamLibraries()
{
    std::vector<std::string> result;
    const char *home = getenv("HOME");
    if (!home)
    {
        return result;
    }

    std::vector<std::string> steamRoots = {
        std::string(home) + "/.steam/steam",
        std::string(home) + "/.local/share/Steam",
        std::string(home) + "/.steam/root"
    };

    for (const auto &steam : steamRoots)
    {
        if (FileExists(steam))
        {
            result.push_back(steam);
            std::string vdfPath = steam + "/steamapps/libraryfolders.vdf";
            FILE *f = fopen(vdfPath.c_str(), "rb");
            if (f)
            {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0)
                {
                    std::string text(sz, '\0');
                    if (fread(text.data(), 1, sz, f) == static_cast<size_t>(sz))
                    {
                        size_t pos = 0;
                        while ((pos = text.find("\"path\"", pos)) != std::string::npos)
                        {
                            pos += 6;
                            size_t open = text.find('"', pos);
                            if (open == std::string::npos)
                            {
                                break;
                            }
                            size_t close = text.find('"', open + 1);
                            if (close == std::string::npos)
                            {
                                break;
                            }
                            std::string path = text.substr(open + 1, close - open - 1);
                            result.push_back(path);
                            pos = close + 1;
                        }
                    }
                }
                fclose(f);
            }
            break;
        }
    }

    return result;
}

static std::string FindCS2Root(const std::string &exeDir, int argc, char **argv)
{
    std::string param = GetParam(argc, argv, "-cs2path");
    if (!param.empty())
    {
        return IsCS2Root(param) ? param : std::string{};
    }

    // Copied into <cs2>/game/bin/linuxsteamrt64
    std::string candidate = DirName(DirName(DirName(exeDir)));
    if (IsCS2Root(candidate))
    {
        return candidate;
    }

    for (const std::string &library : SteamLibraries())
    {
        candidate = library + "/steamapps/common/Counter-Strike Global Offensive";
        if (IsCS2Root(candidate))
        {
            return candidate;
        }
    }

    return {};
}

int main(int argc, char **argv)
{
    std::string exePath = GetExecutablePath();
    std::string exeDir = DirName(exePath);

    bool insecure = HasFlag(argc, argv, "-insecure");
    if (!insecure)
    {
        // CS2 requires -insecure when using custom/untrusted files
        std::vector<char *> newArgv;
        for (int i = 0; i < argc; i++)
        {
            newArgv.push_back(argv[i]);
        }
        char insecureFlag[] = "-insecure";
        newArgv.push_back(insecureFlag);
        newArgv.push_back(nullptr);

        execv(exePath.c_str(), newArgv.data());
        ErrorMessageBox("Failed to relaunch with -insecure");
        return 1;
    }

    bool dedicated = HasFlag(argc, argv, "-dedicated");

    std::string root = FindCS2Root(exeDir, argc, argv);
    if (root.empty())
    {
        ErrorMessageBox("Could not find Counter-Strike 2.\n"
                        "Put cs2_gc in the game folder, next to the original files, or pass:\n"
                        "  -cs2path <path to Counter-Strike Global Offensive>");
        return 1;
    }

    std::string binDir = root + "/game/bin/linuxsteamrt64";
    std::string gameDir = root + "/game/csgo";

    std::string dataDir = GetParam(argc, argv, "-gcdata");
    if (dataDir.empty())
    {
        dataDir = exeDir + "/cs2_gc";
    }

    // Set LD_LIBRARY_PATH so the engine finds libraries in linuxsteamrt64
    const char *oldLd = getenv("LD_LIBRARY_PATH");
    std::string newLd = binDir;
    if (oldLd && *oldLd)
    {
        newLd += ":";
        newLd += oldLd;
    }
    setenv("LD_LIBRARY_PATH", newLd.c_str(), 1);

    bool besideCs2 = (exeDir == binDir);
    if (besideCs2)
    {
        if (chdir(binDir.c_str()) != 0) {}
    }
    else
    {
        std::string gameRoot = root + "/game";
        if (chdir(gameRoot.c_str()) != 0) {}
    }

    std::string tier0Path = binDir + "/libtier0.so";
    void *tier0 = dlopen(tier0Path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!tier0)
    {
        ErrorMessageBox("Could not load '%s':\n%s", tier0Path.c_str(), dlerror());
        return 1;
    }

    auto Plat_LoadModule = reinterpret_cast<Plat_LoadModule_t>(dlsym(tier0, "Plat_LoadModule"));
    auto Plat_GetModuleProcAddress = reinterpret_cast<Plat_GetModuleProcAddress_t>(dlsym(tier0, "Plat_GetModuleProcAddress"));

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
        std::string enginePath = binDir + "/libengine2.so";
        void *engineModule = dlopen(enginePath.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!engineModule)
        {
            ErrorMessageBox("Could not load '%s':\n%s", enginePath.c_str(), dlerror());
            return 1;
        }

        source2Main = dlsym(engineModule, "Source2Main");
        if (!source2Main)
        {
            ErrorMessageBox("Could not find Source2Main in '%s'", enginePath.c_str());
            return 1;
        }
    }

    GCInstallParams params{};
    params.gameBinDir = binDir.c_str();
    params.gameDir = gameDir.c_str();
    params.dataDir = dataDir.c_str();
    params.dedicated = dedicated;
    InstallGC(params);

    return reinterpret_cast<Source2Main_t>(source2Main)(argc, argv, binDir.c_str(), "csgo");
}

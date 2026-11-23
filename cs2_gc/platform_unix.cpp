#include "stdafx.h"
#include "platform.h"
#include "config.h"
#include "keyvalue.h"

#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <link.h>
#include <elf.h>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Platform
{

static std::string s_gameBinDir;
static std::string s_gameDir;
static std::string s_dataDir;

static std::string WideToUtf8(const wchar_t *w)
{
    if (!w) return "";
    std::mbstate_t state{};
    const wchar_t *src = w;
    size_t len = std::wcsrtombs(nullptr, &src, 0, &state);
    if (len == static_cast<size_t>(-1)) return "";
    std::string s(len, '\0');
    src = w;
    std::wcsrtombs(s.data(), &src, len, &state);
    return s;
}

void SetPaths(const char *gameBinDir, const char *gameDir, const char *dataDir)
{
    s_gameBinDir = gameBinDir ? gameBinDir : "";
    s_gameDir = gameDir ? gameDir : "";
    s_dataDir = dataDir ? dataDir : "";
}

void SetPaths(const wchar_t *gameBinDir, const wchar_t *gameDir, const wchar_t *dataDir)
{
    SetPaths(WideToUtf8(gameBinDir).c_str(), WideToUtf8(gameDir).c_str(), WideToUtf8(dataDir).c_str());
}

static std::string NormalizePath(std::string_view path)
{
    std::string result{ path };
    for (char &c : result)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    return result;
}

std::string DataPath(std::string_view name)
{
    std::string norm = NormalizePath(name);
    std::string path = s_dataDir;
    if (!path.empty() && path.back() != '/')
    {
        path.push_back('/');
    }
    path.append(norm);
    return path;
}

std::string GamePath(std::string_view name)
{
    std::string norm = NormalizePath(name);
    std::string path = s_gameDir;
    if (!path.empty() && path.back() != '/')
    {
        path.push_back('/');
    }
    path.append(norm);
    return path;
}

FILE *OpenFile(const std::string &utf8Path, const char *mode)
{
    std::string norm = NormalizePath(utf8Path);
    return fopen(norm.c_str(), mode);
}

using ConColorMsg_t = void (*)(const uint8_t *, const char *, ...);
static ConColorMsg_t s_ConColorMsg;

void Initialize()
{
    RemoveFile(DataPath("gc_log.txt"));

    void *tier0 = dlopen("libtier0.so", RTLD_NOLOAD | RTLD_LAZY);
    if (!tier0 && !s_gameBinDir.empty())
    {
        std::string tier0Path = s_gameBinDir + "/libtier0.so";
        tier0 = dlopen(tier0Path.c_str(), RTLD_LAZY);
    }
    if (tier0)
    {
        // void ConColorMsg(const Color &, const char *, ...), GCC/Clang mangling
        s_ConColorMsg = reinterpret_cast<ConColorMsg_t>(dlsym(tier0, "_Z11ConColorMsgRK5ColorPKcz"));
    }
}

void PrintV(std::string_view fmt, std::format_args args)
{
    LogOutput logOutput = GetConfig().m_logOutput;
    if (logOutput <= LogOutputNone)
    {
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
        fputs(buffer.c_str(), stdout);
        fflush(stdout);
    }

    if (logOutput >= LogOutputFile)
    {
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

    Print("Fatal error: {}\n", buffer);
    fprintf(stderr, "cs2_gc: %s\n", buffer);

    if (getenv("DISPLAY") || getenv("WAYLAND_DISPLAY"))
    {
        std::string cmd = std::format("zenity --error --title=\"cs2_gc\" --text=\"{}\" 2>/dev/null", buffer);
        if (system(cmd.c_str()) != 0)
        {
            cmd = std::format("kdialog --error \"{}\" --title \"cs2_gc\" 2>/dev/null", buffer);
            if (system(cmd.c_str()) != 0)
            {
                cmd = std::format("xmessage -center \"cs2_gc: {}\" 2>/dev/null", buffer);
                [[maybe_unused]] int res = system(cmd.c_str());
            }
        }
    }

    exit(1);
}

struct ModuleInfo
{
    std::string fullPath;
    void *baseAddress = nullptr;
    size_t size = 0;
};

static bool GetModuleInfo(std::string_view moduleName, ModuleInfo &info)
{
    info.fullPath.assign("/");
    info.fullPath.append(moduleName);

    auto callback = [](dl_phdr_info *phdrInfo, size_t, void *data) -> int
    {
        ModuleInfo *moduleInfo = static_cast<ModuleInfo *>(data);
        if (phdrInfo->dlpi_name && strstr(phdrInfo->dlpi_name, moduleInfo->fullPath.c_str()))
        {
            moduleInfo->fullPath.assign(phdrInfo->dlpi_name);
            moduleInfo->baseAddress = reinterpret_cast<void *>(phdrInfo->dlpi_addr);
            size_t maxVaddr = 0;
            for (int i = 0; i < phdrInfo->dlpi_phnum; i++)
            {
                if (phdrInfo->dlpi_phdr[i].p_type == PT_LOAD)
                {
                    size_t endVaddr = phdrInfo->dlpi_phdr[i].p_vaddr + phdrInfo->dlpi_phdr[i].p_memsz;
                    if (endVaddr > maxVaddr)
                    {
                        maxVaddr = endVaddr;
                    }
                }
            }
            moduleInfo->size = maxVaddr;
            return 1;
        }
        return 0;
    };

    return dl_iterate_phdr(callback, &info) != 0;
}

bool SteamClientPath(void *buffer, size_t bufferSize, bool dedicated)
{
    char *out = reinterpret_cast<char *>(buffer);

    ModuleInfo moduleInfo;
    if (GetModuleInfo("steamclient.so", moduleInfo))
    {
        if (moduleInfo.fullPath.size() < bufferSize)
        {
            memcpy(out, moduleInfo.fullPath.data(), moduleInfo.fullPath.size() + 1);
            return true;
        }
    }

    const char *home = getenv("HOME");
    if (home)
    {
        std::vector<std::string> candidates = {
            std::string(home) + "/.steam/sdk64/steamclient.so",
            std::string(home) + "/.steam/steam/linux64/steamclient.so",
            std::string(home) + "/.local/share/Steam/linux64/steamclient.so",
            std::string(home) + "/.steam/root/linux64/steamclient.so",
            std::string(home) + "/.steam/bin/steamclient.so",
        };
        for (const auto &path : candidates)
        {
            if (FileExists(path) && path.size() < bufferSize)
            {
                memcpy(out, path.c_str(), path.size() + 1);
                return true;
            }
        }
    }

    if (!dedicated)
    {
        return false;
    }

    std::vector<std::string> dedicatedCandidates = {
        s_gameBinDir + "/steamclient.so",
        s_gameBinDir + "/../linux64/steamclient.so"
    };
    for (const auto &path : dedicatedCandidates)
    {
        if (FileExists(path) && path.size() < bufferSize)
        {
            memcpy(out, path.c_str(), path.size() + 1);
            return true;
        }
    }

    return false;
}

void *LoadDynamicLibrary(const void *pathBuffer)
{
    return dlopen(reinterpret_cast<const char *>(pathBuffer), RTLD_NOW);
}

void *LoadGameLibrary(const char *name)
{
    std::string path = s_gameBinDir.empty() ? name : (s_gameBinDir + "/" + name);
    return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
}

void *GetSymbol(void *handle, const char *symbol)
{
    return dlsym(handle, symbol);
}

void SetEnvVar(const char *name, const char *value)
{
    setenv(name, value, 1);
}

static void *Q_memmem(const void *_haystack, size_t haystack_len, const void *_needle, size_t needle_len)
{
    return memmem(_haystack, haystack_len, _needle, needle_len);
}

bool PatchGraffitiPublicKey(std::string_view moduleName, const void *original, const void *replacement, size_t size)
{
    std::string actualModuleName = "lib";
    actualModuleName.append(moduleName);
    actualModuleName.append(".so");

    ModuleInfo info;
    if (!GetModuleInfo(actualModuleName, info))
    {
        return false;
    }

    void *address = Q_memmem(info.baseAddress, info.size, original, size);
    if (!address)
    {
        return false;
    }

    size_t pageSize = sysconf(_SC_PAGESIZE);
    void *alignedAddress = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(address) & ~(pageSize - 1));
    size_t alignedSize = (reinterpret_cast<uintptr_t>(address) + size) - reinterpret_cast<uintptr_t>(alignedAddress);

    if (mprotect(alignedAddress, alignedSize, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
    {
        memcpy(address, replacement, size);
        mprotect(alignedAddress, alignedSize, PROT_READ | PROT_EXEC);
        return true;
    }

    return false;
}

bool RemoveFile(const std::string &utf8Path)
{
    std::string norm = NormalizePath(utf8Path);
    return unlink(norm.c_str()) == 0;
}

bool FileExists(const std::string &utf8Path)
{
    std::string norm = NormalizePath(utf8Path);
    struct stat st;
    if (stat(norm.c_str(), &st) != 0)
    {
        return false;
    }
    return !S_ISDIR(st.st_mode);
}

static std::string LanguageWord(std::string_view text)
{
    std::string word;
    for (char c : text)
    {
        if (!std::isalpha(static_cast<unsigned char>(c)) || word.size() >= 32)
        {
            return {};
        }
        word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return word;
}

std::string GameLanguage()
{
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    if (cmdline.is_open())
    {
        std::string arg;
        bool nextIsLang = false;
        while (std::getline(cmdline, arg, '\0'))
        {
            if (nextIsLang)
            {
                std::string lang = LanguageWord(arg);
                if (!lang.empty()) return lang;
                nextIsLang = false;
            }
            if (arg == "-language")
            {
                nextIsLang = true;
            }
        }
    }

    if (s_gameDir.empty())
    {
        return {};
    }

    std::filesystem::path manifest = std::filesystem::path{ s_gameDir }.parent_path().parent_path().parent_path().parent_path() / "appmanifest_730.acf";
    std::string manifestPath = manifest.string();
    if (!FileExists(manifestPath))
    {
        return {};
    }

    KeyValueRead read{ manifestPath.c_str() };
    const KeyValue *state = read.Good() ? read.Root().GetSubkey("AppState") : nullptr;
    for (const char *block : { "UserConfig", "MountedConfig" })
    {
        const KeyValue *config = state ? state->GetSubkey(block) : nullptr;
        std::string language = config ? LanguageWord(config->GetString("language")) : std::string{};
        if (!language.empty())
        {
            return language;
        }
    }
    return {};
}

bool ReplaceWith(const std::string &from, const std::string &to)
{
    std::string normFrom = NormalizePath(from);
    std::string normTo = NormalizePath(to);
    return rename(normFrom.c_str(), normTo.c_str()) == 0;
}

bool CopyTo(const std::string &from, const std::string &to)
{
    std::string normFrom = NormalizePath(from);
    std::string normTo = NormalizePath(to);
    std::error_code ec;
    return std::filesystem::copy_file(normFrom, normTo, std::filesystem::copy_options::overwrite_existing, ec);
}

} // namespace Platform

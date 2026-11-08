#pragma once

namespace Platform
{

// where things are. Set by the launcher before Initialize().
// gameBinDir: <cs2>\game\bin\win64 (engine2.dll, tier0.dll, steam_api64.dll)
// gameDir:    <cs2>\game\csgo (pak01_dir.vpk)
// dataDir:    <launcher dir>\cs2_gc (config.txt, inventory.txt, gc_log.txt)
// cs2_gc only reads from the game folders, every file it writes goes to dataDir.
void SetPaths(const wchar_t *gameBinDir, const wchar_t *gameDir, const wchar_t *dataDir);

// absolute UTF-8 path to a file in dataDir / gameDir
std::string DataPath(std::string_view name);
std::string GamePath(std::string_view name);

// fopen with a UTF-8 path
FILE *OpenFile(const std::string &utf8Path, const char *mode);

// called right after we load into the process
void Initialize();

// print a debugging message to the in game console or something
void PrintV(std::string_view fmt, std::format_args args);

template<class... Args>
void Print(std::format_string<Args...> fmt, Args &&...args)
{
    PrintV(fmt.get(), std::make_format_args(args...));
}

// fatal error, show a message box if possible and exit the program
[[noreturn]] void Error(const char *format, ...);

// get steamclient64's path (from the running Steam client), UTF-16. A dedicated server may run
// without Steam: then steamclient64.dll from the game's bin folder or next to cs2_gc.exe (steamcmd's copy)
bool SteamClientPath(void *buffer, size_t bufferSize, bool dedicated = false);

// load a dynamic library from the provided UTF-16 path and increment its refcount
void *LoadDynamicLibrary(const void *pathBuffer);

// load a library that ships with the game from gameBinDir
void *LoadGameLibrary(const wchar_t *name);

// GetProcAddress
void *GetSymbol(void *handle, const char *symbol);

// set an envar to the specified value even if it's already set
void SetEnvVar(const char *name, const char *value);

// patch the graffiti public key in the specified module to get sprays working
// moduleName without extension, e.g. "client"
bool PatchGraffitiPublicKey(std::string_view moduleName, const void *original, const void *replacement, size_t size);

bool RemoveFile(const std::string &utf8Path);

} // namespace Platform

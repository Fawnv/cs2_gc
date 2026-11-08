#include "stdafx.h"
#include "vpk.h"

constexpr uint32_t VpkSignature = 0x55aa1234;
constexpr uint16_t VpkDirArchive = 0x7fff;
constexpr uint16_t VpkEntryTerminator = 0xffff;

namespace
{

struct Reader
{
    const char *data;
    size_t size;
    size_t pos{};

    bool ReadString(std::string_view &out)
    {
        const char *start = data + pos;
        const char *end = static_cast<const char *>(memchr(start, '\0', size - pos));
        if (!end)
        {
            return false;
        }

        out = std::string_view{ start, static_cast<size_t>(end - start) };
        pos += out.size() + 1;
        return true;
    }

    template<typename T>
    bool Read(T &out)
    {
        if (pos + sizeof(T) > size)
        {
            return false;
        }

        memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
};

#pragma pack(push, 1)
struct VpkEntry
{
    uint32_t crc;
    uint16_t preloadBytes;
    uint16_t archiveIndex;
    uint32_t entryOffset;
    uint32_t entryLength;
    uint16_t terminator;
};
#pragma pack(pop)

static_assert(sizeof(VpkEntry) == 18);

} // namespace

static bool ReadWholeFile(const std::string &path, std::vector<char> &out)
{
    FILE *f = Platform::OpenFile(path, "rb");
    if (!f)
    {
        return false;
    }

    _fseeki64(f, 0, SEEK_END);
    int64_t size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);

    out.resize(static_cast<size_t>(size));
    size_t read = fread(out.data(), 1, out.size(), f);
    fclose(f);

    return read == out.size();
}

static bool ReadFileRange(const std::string &path, uint64_t offset, uint32_t length, char *out)
{
    FILE *f = Platform::OpenFile(path, "rb");
    if (!f)
    {
        return false;
    }

    bool ok = _fseeki64(f, static_cast<int64_t>(offset), SEEK_SET) == 0
        && fread(out, 1, length, f) == length;

    fclose(f);
    return ok;
}

bool VpkReadFile(const std::string &dirVpkPath, std::string_view filePath, std::vector<char> &out)
{
    // split "scripts/items/items_game.txt" into folder, name and extension
    size_t slash = filePath.rfind('/');
    size_t dot = filePath.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
    {
        return false;
    }

    std::string_view wantFolder = (slash != std::string_view::npos) ? filePath.substr(0, slash) : std::string_view{ " " };
    std::string_view wantName = filePath.substr(slash == std::string_view::npos ? 0 : slash + 1, dot - (slash == std::string_view::npos ? 0 : slash + 1));
    std::string_view wantExt = filePath.substr(dot + 1);

    std::vector<char> dirData;
    if (!ReadWholeFile(dirVpkPath, dirData))
    {
        Platform::Print("Could not read {}\n", dirVpkPath);
        return false;
    }

    Reader reader{ dirData.data(), dirData.size() };

    uint32_t signature, version, treeSize;
    if (!reader.Read(signature) || !reader.Read(version) || !reader.Read(treeSize) || signature != VpkSignature)
    {
        Platform::Print("{} is not a vpk\n", dirVpkPath);
        return false;
    }

    size_t headerSize = (version == 1) ? 12 : 28;
    reader.pos = headerSize;

    std::string_view ext, folder, name;
    while (reader.ReadString(ext) && !ext.empty())
    {
        while (reader.ReadString(folder) && !folder.empty())
        {
            while (reader.ReadString(name) && !name.empty())
            {
                VpkEntry entry;
                if (!reader.Read(entry) || entry.terminator != VpkEntryTerminator)
                {
                    Platform::Print("Corrupt vpk tree in {}\n", dirVpkPath);
                    return false;
                }

                const char *preload = reader.data + reader.pos;
                reader.pos += entry.preloadBytes;

                if (ext != wantExt || folder != wantFolder || name != wantName)
                {
                    continue;
                }

                out.resize(entry.preloadBytes + entry.entryLength);
                memcpy(out.data(), preload, entry.preloadBytes);

                if (!entry.entryLength)
                {
                    return true;
                }

                char *dest = out.data() + entry.preloadBytes;

                if (entry.archiveIndex == VpkDirArchive)
                {
                    uint64_t offset = headerSize + treeSize + entry.entryOffset;
                    if (offset + entry.entryLength > dirData.size())
                    {
                        return false;
                    }

                    memcpy(dest, dirData.data() + offset, entry.entryLength);
                    return true;
                }

                // pak01_dir.vpk -> pak01_042.vpk
                std::string archivePath = dirVpkPath;
                size_t suffix = archivePath.rfind("_dir.vpk");
                if (suffix == std::string::npos)
                {
                    return false;
                }

                archivePath.replace(suffix, std::string::npos, std::format("_{:03}.vpk", entry.archiveIndex));
                return ReadFileRange(archivePath, entry.entryOffset, entry.entryLength, dest);
            }
        }
    }

    return false;
}

uint32_t Crc32(std::string_view data)
{
    static const auto table = []
    {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : data)
    {
        crc = table[(crc ^ c) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

template<typename T>
static void Append(std::string &out, const T &value)
{
    out.append(reinterpret_cast<const char *>(&value), sizeof(T));
}

bool VpkWrite(const std::string &dirVpkPath, std::span<const VpkFile> files)
{
    // extension -> folder -> name -> file, the layout the tree is written in
    std::map<std::string, std::map<std::string, std::map<std::string, const VpkFile *>>> tree;

    for (const VpkFile &file : files)
    {
        std::string_view path = file.path;
        size_t slash = path.rfind('/');
        size_t dot = path.rfind('.');
        bool hasExt = (dot != std::string_view::npos) && (slash == std::string_view::npos || dot > slash);

        size_t nameStart = (slash == std::string_view::npos) ? 0 : slash + 1;
        std::string folder = (slash == std::string_view::npos) ? " " : std::string{ path.substr(0, slash) };
        std::string name{ path.substr(nameStart, (hasExt ? dot : path.size()) - nameStart) };
        std::string ext = hasExt ? std::string{ path.substr(dot + 1) } : " ";

        tree[ext][folder][name] = &file;
    }

    std::string treeData, fileData;
    for (const auto &[ext, folders] : tree)
    {
        treeData.append(ext).push_back('\0');
        for (const auto &[folder, names] : folders)
        {
            treeData.append(folder).push_back('\0');
            for (const auto &[name, file] : names)
            {
                treeData.append(name).push_back('\0');

                VpkEntry entry{};
                entry.crc = Crc32(file->data);
                entry.preloadBytes = 0;
                entry.archiveIndex = VpkDirArchive;
                entry.entryOffset = static_cast<uint32_t>(fileData.size());
                entry.entryLength = static_cast<uint32_t>(file->data.size());
                entry.terminator = VpkEntryTerminator;
                Append(treeData, entry);

                fileData.append(file->data);
            }
            treeData.push_back('\0');
        }
        treeData.push_back('\0');
    }
    treeData.push_back('\0');

    std::string out;
    Append(out, VpkSignature);
    Append(out, uint32_t{ 2 }); // version
    Append(out, static_cast<uint32_t>(treeData.size()));
    Append(out, static_cast<uint32_t>(fileData.size()));
    Append(out, uint32_t{ 0 }); // archive md5 section
    Append(out, uint32_t{ 0 }); // other md5 section
    Append(out, uint32_t{ 0 }); // signature section
    out.append(treeData);
    out.append(fileData);

    FILE *f = Platform::OpenFile(dirVpkPath, "wb");
    if (!f)
    {
        return false;
    }

    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

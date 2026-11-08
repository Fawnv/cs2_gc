#pragma once

// Minimal read-only VPK (v1/v2) reader. CS2 keeps scripts/items/items_game.txt
// inside game\csgo\pak01_dir.vpk instead of a loose file.

// dirVpkPath: UTF-8 path to *_dir.vpk, filePath: e.g. "scripts/items/items_game.txt"
bool VpkReadFile(const std::string &dirVpkPath, std::string_view filePath, std::vector<char> &out);

struct VpkFile
{
    std::string path; // game relative, forward slashes, e.g. "scripts/items/items_game.txt"
    std::string data;
};

// writes a v2 *_dir.vpk with all data inside the dir file (archive index 0x7fff), no md5/signature sections
bool VpkWrite(const std::string &dirVpkPath, std::span<const VpkFile> files);

// standard CRC-32 (zlib/png), also how the client names Armory shop entries
uint32_t Crc32(std::string_view data);

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
// all data inside the _dir file. False (with the reason) when it can't be written, or when the data
// is over what a VPK v2 can address in one file (4 GB: offsets and sizes are 32-bit). Written to a
// .tmp file that then takes the vpk's place, so a vpk is never half written
bool VpkWrite(const std::string &dirVpkPath, std::span<const VpkFile> files, std::string *error = nullptr);

// standard CRC-32 (zlib/png), also how the client names Armory shop entries
uint32_t Crc32(std::string_view data);

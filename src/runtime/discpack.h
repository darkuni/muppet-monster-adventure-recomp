#pragma once
// Packed disc file (assets.dat): the whole disc in one compressed file, made once from the
// user's .cue/.bin. Every 2352-byte sector is stored in a compact form (Mode 2 Form 1 sectors
// keep only the subheader and 2048 data bytes; sync, header, EDC and ECC are regenerated on
// read) and deflated in blocks of 16 sectors. Building verifies that every sector comes back
// bit-identical, so the pack is a lossless copy of the disc.
#include <cstdint>
#include <string>

struct DiscLayout {
    int t1_sectors = 0;   // data track (LBA 0..t1_sectors-1)
    int t2_start = 0;     // disc LBA of the audio track
    int t2_sectors = 0;
};

using RawReader = bool (*)(int lba, uint8_t* out, bool* audio);
using PackProgress = void (*)(int done, int total);

bool pack_build(const std::string& out_path, const DiscLayout& lay, RawReader read, PackProgress progress, std::string* err);
bool pack_open(const std::string& path, DiscLayout* lay, std::string* err);
bool pack_read(int lba, uint8_t* out);   // 2352 bytes

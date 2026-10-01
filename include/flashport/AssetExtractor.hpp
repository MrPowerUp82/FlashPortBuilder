#pragma once
#include "flashport/Image.hpp"
#include "flashport/SWFDocument.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flashport {

struct AssetReport {
    std::uint64_t bitmaps{}, bitmapsFailed{}, bitmapsRaw{};
    std::uint64_t sounds{}, soundsUnsupported{};
    std::uint64_t streams{};
    std::uint64_t shapes{}, shapesFailed{};
    std::uint64_t sprites{};
    std::uint64_t symbols{};
    std::map<std::string, std::uint64_t> skipped; // character kinds not converted yet
    std::vector<std::string> problems;
    void problem(const std::string& p) { if (problems.size() < 40) problems.push_back(p); }
};

// Convert the SWF's characters into runtime-friendly files under `outDir`:
//   bitmaps/<id>.png   sounds/<id>.mp3|.wav   streams/<timeline>.mp3|.wav
//   shapes/<id>.svg    manifest.json (characters, sprites, symbol names)
void extractAssets(const SWFDocument& doc, const std::string& outDir, AssetReport& report);

struct DecodedBitmap {
    bool decoded = false;              // `image` holds straight RGBA
    ImageRGBA image;
    int width{}, height{};
    std::vector<std::uint8_t> original; // undecoded payload (PNG/GIF, or JPEG without decoder)
    std::string originalExtension;
    std::string error;
};

// Decode DefineBits(+JPEGTables)/JPEG2/JPEG3/JPEG4/DefineBitsLossless(2) to straight RGBA.
DecodedBitmap decodeBitmapTag(const SWFDocument& doc, const TagRecord& tag, const std::uint8_t* jpegTables,
                              std::size_t jpegTablesLen);

void printAssetReport(const AssetReport& r, std::ostream& out);

} // namespace flashport

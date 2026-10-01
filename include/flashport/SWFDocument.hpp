#pragma once
#include "flashport/SWFReader.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace flashport {

// One tag record inside a decompressed SWF. Tags nested in a DefineSprite
// carry the sprite's character id in `spriteId` (0 = main timeline).
struct TagRecord {
    std::uint16_t code{};
    std::size_t offset{};   // payload offset in SWFDocument::data
    std::uint32_t length{}; // payload length
    std::uint16_t spriteId{};
    std::uint32_t frame{};  // frame index within its timeline
};

// Decompressed SWF with a flat, recursive tag list. Unlike SWFSummary this
// keeps the bytes so later stages (decoders, asset extraction) can read payloads.
struct SWFDocument {
    std::string sourceName;
    std::uint8_t version{};
    Rect frameRect;
    double fps{};
    std::uint16_t frameCount{};
    std::vector<std::uint8_t> data; // always FWS (uncompressed)
    std::vector<TagRecord> tags;

    const std::uint8_t* payload(const TagRecord& t) const { return data.data() + t.offset; }
    std::vector<std::uint8_t> payloadCopy(const TagRecord& t) const {
        return {data.begin() + static_cast<std::ptrdiff_t>(t.offset),
                data.begin() + static_cast<std::ptrdiff_t>(t.offset + t.length)};
    }
};

std::vector<std::uint8_t> readFileBytes(const std::string& path);
std::vector<std::uint8_t> decompressSWF(const std::vector<std::uint8_t>& input);
bool looksLikeSWF(const std::vector<std::uint8_t>& bytes);
SWFDocument loadSWFDocument(const std::vector<std::uint8_t>& bytes, const std::string& sourceName);
SWFDocument loadSWFDocumentFile(const std::string& path);

struct EmbeddedBinary {
    std::uint16_t characterId{};
    std::vector<std::uint8_t> bytes;
};

// DefineBinaryData payloads, optionally only those that start with a SWF signature.
std::vector<EmbeddedBinary> binaryDataPayloads(const SWFDocument& doc, bool swfOnly);

} // namespace flashport

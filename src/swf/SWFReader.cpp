#include "flashport/SWFReader.hpp"
#include "flashport/ABCReader.hpp"
#include "flashport/BitReader.hpp"
#include "flashport/ByteReader.hpp"
#include "flashport/SWFDocument.hpp"
#include <algorithm>
#include <stdexcept>

namespace flashport {
namespace {

bool isSwfMagic(const std::string& sig) {
    return sig == "FWS" || sig == "CWS" || sig == "ZWS";
}

SWFSummary analyzeBuffer(const std::vector<std::uint8_t>& input,
                         const std::string& sourceName,
                         unsigned embeddedDepth);

void parseTags(const std::vector<std::uint8_t>& data,
               std::size_t start,
               std::size_t end,
               SWFSummary& out,
               const ABCReader& abcReader,
               unsigned spriteDepth,
               unsigned embeddedDepth) {
    if (spriteDepth > 64) throw std::runtime_error("DefineSprite nesting is too deep");
    ByteReader r(data, start);
    while (r.pos() + 2 <= end) {
        const auto recordHeader = r.u16();
        const std::uint16_t code = recordHeader >> 6;
        std::uint32_t length = recordHeader & 0x3f;
        if (length == 0x3f) length = r.u32();
        const auto payloadStart = r.pos();
        const auto payloadEnd = payloadStart + length;
        if (payloadEnd > end || payloadEnd > data.size()) {
            throw std::runtime_error("SWF tag extends beyond container: tag " + std::to_string(code));
        }

        ++out.tagCounts[code];
        ++out.totalTags;
        if (code == 12 || code == 59) out.hasAVM1Actions = true; // DoAction / DoInitAction

        try {
            if (code == 69 && length >= 4) { // FileAttributes
                ByteReader p(data, payloadStart);
                const auto flags = p.u32();
                if ((flags & 0x08u) != 0) out.actionScript3 = true;
            } else if (code == 76) { // SymbolClass
                ByteReader p(data, payloadStart);
                const auto count = p.u16();
                for (std::uint16_t i = 0; i < count && p.pos() < payloadEnd; ++i) {
                    const auto id = p.u16();
                    const auto name = p.cstring();
                    if (id == 0) out.documentClass = name;
                }
            } else if (code == 82 && length >= 5) { // DoABC
                ByteReader p(data, payloadStart);
                (void)p.u32();
                const auto name = p.cstring();
                const auto abc = p.bytes(payloadEnd - p.pos());
                out.abcBlocks.push_back(abcReader.analyze(abc, name));
                out.actionScript3 = true;
            } else if (code == 72 && length >= 4) { // legacy DoABCDefine
                ByteReader p(data, payloadStart);
                const auto abc = p.bytes(length);
                out.abcBlocks.push_back(abcReader.analyze(abc, "<DoABCDefine>"));
                out.actionScript3 = true;
            } else if (code == 39 && length >= 4) { // DefineSprite
                parseTags(data, payloadStart + 4, payloadEnd, out, abcReader,
                          spriteDepth + 1, embeddedDepth);
            } else if (code == 87 && length >= 6) { // DefineBinaryData
                ByteReader p(data, payloadStart);
                BinaryDataSummary binary;
                binary.characterId = p.u16();
                (void)p.u32(); // reserved
                binary.payloadBytes = payloadEnd - p.pos();
                if (binary.payloadBytes >= 3) {
                    binary.magic.assign(reinterpret_cast<const char*>(data.data() + p.pos()), 3);
                }

                if (isSwfMagic(binary.magic) && embeddedDepth < 8) {
                    const auto blob = p.bytes(binary.payloadBytes);
                    if (binary.magic != "ZWS") {
                        try {
                            binary.embeddedSwf = std::make_shared<SWFSummary>(
                                analyzeBuffer(blob,
                                    out.sourceName + "::BinaryData#" + std::to_string(binary.characterId),
                                    embeddedDepth + 1));
                        } catch (const std::exception&) {
                            // Keep the binary entry even if a corrupt/false-positive SWF signature is encountered.
                        }
                    }
                }
                out.binaryData.push_back(std::move(binary));
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("while parsing " + tagName(code) + " (#" +
                                     std::to_string(code) + "): " + e.what());
        }

        r.seek(payloadEnd);
        if (code == 0) break;
    }
}

SWFSummary analyzeBuffer(const std::vector<std::uint8_t>& input,
                         const std::string& sourceName,
                         unsigned embeddedDepth) {
    if (input.size() < 8) throw std::runtime_error("file is too small");

    SWFSummary out;
    out.sourceName = sourceName;
    out.signature.assign(reinterpret_cast<const char*>(input.data()), 3);
    out.version = input[3];
    out.actualFileLength = input.size();
    out.declaredFileLength = static_cast<std::uint32_t>(input[4]) |
                             (static_cast<std::uint32_t>(input[5]) << 8) |
                             (static_cast<std::uint32_t>(input[6]) << 16) |
                             (static_cast<std::uint32_t>(input[7]) << 24);

    const auto data = decompressSWF(input);
    BitReader bits(data, 8);
    const auto nbits = bits.bits(5);
    out.frameRect.xmin = bits.signedBits(nbits);
    out.frameRect.xmax = bits.signedBits(nbits);
    out.frameRect.ymin = bits.signedBits(nbits);
    out.frameRect.ymax = bits.signedBits(nbits);

    ByteReader r(data, bits.nextByte());
    const auto frameRateRaw = r.u16();
    out.fps = static_cast<double>(frameRateRaw) / 256.0;
    out.frameCount = r.u16();

    ABCReader abcReader;
    parseTags(data, r.pos(), data.size(), out, abcReader, 0, embeddedDepth);
    return out;
}

} // namespace

SWFSummary SWFReader::analyzeFile(const std::string& path) const {
    return analyzeBuffer(readFileBytes(path), path, 0);
}

SWFSummary SWFReader::analyzeBytes(const std::vector<std::uint8_t>& bytes,
                                   const std::string& sourceName) const {
    return analyzeBuffer(bytes, sourceName, 0);
}

std::string actionScriptKind(const SWFSummary& s) {
    if (s.actionScript3 && s.hasAVM1Actions) return "mixed AVM1 + AVM2";
    if (s.actionScript3) return "AVM2 / AS3";
    if (s.hasAVM1Actions) return "AVM1 / AS1-AS2";
    return "none detected";
}

std::string tagName(std::uint16_t code) {
    switch (code) {
        case 0: return "End";
        case 1: return "ShowFrame";
        case 2: return "DefineShape";
        case 6: return "DefineBits";
        case 7: return "DefineButton";
        case 8: return "JPEGTables";
        case 9: return "SetBackgroundColor";
        case 10: return "DefineFont";
        case 11: return "DefineText";
        case 12: return "DoAction";
        case 14: return "DefineSound";
        case 15: return "StartSound";
        case 17: return "DefineButtonSound";
        case 18: return "SoundStreamHead";
        case 19: return "SoundStreamBlock";
        case 20: return "DefineBitsLossless";
        case 21: return "DefineBitsJPEG2";
        case 22: return "DefineShape2";
        case 26: return "PlaceObject2";
        case 28: return "RemoveObject2";
        case 32: return "DefineShape3";
        case 33: return "DefineText2";
        case 34: return "DefineButton2";
        case 35: return "DefineBitsJPEG3";
        case 36: return "DefineBitsLossless2";
        case 37: return "DefineEditText";
        case 39: return "DefineSprite";
        case 43: return "FrameLabel";
        case 45: return "SoundStreamHead2";
        case 46: return "DefineMorphShape";
        case 48: return "DefineFont2";
        case 56: return "ExportAssets";
        case 59: return "DoInitAction";
        case 60: return "DefineVideoStream";
        case 61: return "VideoFrame";
        case 65: return "ScriptLimits";
        case 69: return "FileAttributes";
        case 70: return "PlaceObject3";
        case 72: return "DoABCDefine";
        case 73: return "DefineFontAlignZones";
        case 74: return "CSMTextSettings";
        case 75: return "DefineFont3";
        case 76: return "SymbolClass";
        case 77: return "Metadata";
        case 78: return "DefineScalingGrid";
        case 82: return "DoABC";
        case 83: return "DefineShape4";
        case 84: return "DefineMorphShape2";
        case 86: return "DefineSceneAndFrameLabelData";
        case 87: return "DefineBinaryData";
        case 88: return "DefineFontName";
        case 90: return "DefineBitsJPEG4";
        case 91: return "DefineFont4";
        default: return "Tag" + std::to_string(code);
    }
}

} // namespace flashport

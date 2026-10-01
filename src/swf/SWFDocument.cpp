#include "flashport/SWFDocument.hpp"
#include "flashport/BitReader.hpp"
#include "flashport/ByteReader.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <zlib.h>
#ifdef FLASHPORT_HAVE_LZMA
#include <lzma.h>
#endif

namespace flashport {

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
#ifdef _WIN32
    if (!f) {
        // Paths longer than MAX_PATH need the \\?\ prefix on an absolute, backslashed path.
        std::error_code ec;
        auto abs = std::filesystem::absolute(std::filesystem::u8path(path), ec);
        if (!ec) {
            std::wstring w = abs.make_preferred().wstring();
            if (w.rfind(L"\\\\?\\", 0) != 0) w = L"\\\\?\\" + w;
            f.open(std::filesystem::path(w), std::ios::binary);
        }
    }
#endif
    if (!f) throw std::runtime_error("cannot open file: " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

bool looksLikeSWF(const std::vector<std::uint8_t>& b) {
    if (b.size() < 8) return false;
    return (b[0] == 'F' || b[0] == 'C' || b[0] == 'Z') && b[1] == 'W' && b[2] == 'S';
}

namespace {

// ZWS layout: "ZWS" version, u32 uncompressed length (with the 8-byte header),
// u32 compressed length, 5 LZMA properties bytes, raw LZMA stream.
std::vector<std::uint8_t> decompressZWS(const std::vector<std::uint8_t>& input) {
#ifdef FLASHPORT_HAVE_LZMA
    if (input.size() < 17) throw std::runtime_error("ZWS file is too small");
    ByteReader h(input, 4);
    const auto declared = h.u32();
    if (declared < 8) throw std::runtime_error("invalid SWF declared length");

    // Rebuild a classic .lzma ("LZMA alone") header: props + 64-bit size.
    std::vector<std::uint8_t> alone(input.begin() + 12, input.begin() + 17);
    const std::uint64_t size = declared - 8;
    for (int i = 0; i < 8; ++i) alone.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    alone.insert(alone.end(), input.begin() + 17, input.end());

    std::vector<std::uint8_t> out(input.begin(), input.begin() + 8);
    out[0] = 'F';
    out.resize(declared);
    lzma_stream zs = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&zs, UINT64_MAX) != LZMA_OK) throw std::runtime_error("lzma init failed");
    zs.next_in = alone.data();
    zs.avail_in = alone.size();
    zs.next_out = out.data() + 8;
    zs.avail_out = out.size() - 8;
    lzma_ret rc = LZMA_OK;
    while (rc == LZMA_OK && zs.avail_out > 0) rc = lzma_code(&zs, LZMA_FINISH);
    const std::size_t written = out.size() - zs.avail_out;
    lzma_end(&zs);
    if (rc != LZMA_OK && rc != LZMA_STREAM_END && written <= 8)
        throw std::runtime_error("failed to decompress ZWS with LZMA");
    out.resize(written);
    return out;
#else
    (void)input;
    throw std::runtime_error("ZWS/LZMA needs liblzma (rebuild FlashPortBuilder with xz installed)");
#endif
}

} // namespace

std::vector<std::uint8_t> decompressSWF(const std::vector<std::uint8_t>& input) {
    if (input.size() < 8) throw std::runtime_error("file is too small to be SWF");
    const std::string sig(reinterpret_cast<const char*>(input.data()), 3);
    if (sig == "FWS") return input;
    if (sig == "ZWS") return decompressZWS(input);
    if (sig != "CWS") throw std::runtime_error("not a SWF file (expected FWS/CWS/ZWS)");

    ByteReader h(input, 4);
    const auto declared = h.u32();
    if (declared < 8) throw std::runtime_error("invalid SWF declared length");

    // Streaming inflate: tolerates declared lengths that are wrong and
    // trailing garbage after the zlib stream, both common in packed games.
    std::vector<std::uint8_t> out(input.begin(), input.begin() + 8);
    out[0] = 'F';
    out.resize(std::max<std::size_t>(declared, 8 + 1024));

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw std::runtime_error("inflateInit failed");
    zs.next_in = const_cast<Bytef*>(input.data() + 8);
    zs.avail_in = static_cast<uInt>(input.size() - 8);
    std::size_t written = 8;
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (written == out.size()) out.resize(out.size() * 2);
        zs.next_out = out.data() + written;
        zs.avail_out = static_cast<uInt>(out.size() - written);
        rc = inflate(&zs, Z_NO_FLUSH);
        written = out.size() - zs.avail_out;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
        if (rc == Z_BUF_ERROR) rc = Z_OK;
    }
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        throw std::runtime_error("failed to decompress CWS with zlib");
    }
    out.resize(written);
    return out;
}

namespace {

void collectTags(const std::vector<std::uint8_t>& data, std::size_t start, std::size_t end,
                 std::uint16_t spriteId, unsigned depth, std::vector<TagRecord>& tags) {
    if (depth > 64) throw std::runtime_error("DefineSprite nesting is too deep");
    ByteReader r(data, start);
    std::uint32_t frame = 0;
    while (r.pos() + 2 <= end) {
        const auto header = r.u16();
        TagRecord t;
        t.code = header >> 6;
        t.length = header & 0x3f;
        if (t.length == 0x3f) t.length = r.u32();
        t.offset = r.pos();
        t.spriteId = spriteId;
        t.frame = frame;
        if (t.offset + t.length > end) {
            // Truncated final tag: keep what fits so partially damaged files remain usable.
            t.length = static_cast<std::uint32_t>(end - t.offset);
        }
        tags.push_back(t);
        if (t.code == 1) ++frame;
        if (t.code == 39 && t.length >= 4) {
            const std::uint16_t id = static_cast<std::uint16_t>(data[t.offset] | (data[t.offset + 1] << 8));
            collectTags(data, t.offset + 4, t.offset + t.length, id, depth + 1, tags);
        }
        r.seek(t.offset + t.length);
        if (t.code == 0) break;
    }
}

} // namespace

SWFDocument loadSWFDocument(const std::vector<std::uint8_t>& bytes, const std::string& sourceName) {
    SWFDocument doc;
    doc.sourceName = sourceName;
    doc.data = decompressSWF(bytes);
    doc.version = doc.data[3];

    BitReader bits(doc.data, 8);
    const auto nbits = bits.bits(5);
    doc.frameRect.xmin = bits.signedBits(nbits);
    doc.frameRect.xmax = bits.signedBits(nbits);
    doc.frameRect.ymin = bits.signedBits(nbits);
    doc.frameRect.ymax = bits.signedBits(nbits);
    ByteReader r(doc.data, bits.nextByte());
    doc.fps = static_cast<double>(r.u16()) / 256.0;
    doc.frameCount = r.u16();

    collectTags(doc.data, r.pos(), doc.data.size(), 0, 0, doc.tags);
    return doc;
}

SWFDocument loadSWFDocumentFile(const std::string& path) {
    return loadSWFDocument(readFileBytes(path), path);
}

std::vector<EmbeddedBinary> binaryDataPayloads(const SWFDocument& doc, bool swfOnly) {
    std::vector<EmbeddedBinary> out;
    for (const auto& t : doc.tags) {
        if (t.code != 87 || t.length < 6) continue;
        EmbeddedBinary b;
        b.characterId = static_cast<std::uint16_t>(doc.data[t.offset] | (doc.data[t.offset + 1] << 8));
        b.bytes.assign(doc.data.begin() + static_cast<std::ptrdiff_t>(t.offset + 6),
                       doc.data.begin() + static_cast<std::ptrdiff_t>(t.offset + t.length));
        if (swfOnly && !looksLikeSWF(b.bytes)) continue;
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace flashport

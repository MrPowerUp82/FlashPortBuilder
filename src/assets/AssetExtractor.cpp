#include "flashport/AssetExtractor.hpp"
#include "flashport/Audio.hpp"
#include "flashport/ByteReader.hpp"
#include "flashport/Image.hpp"
#include "flashport/Shape.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <zlib.h>

namespace flashport {
namespace fs = std::filesystem;
namespace {

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("cannot write " + path.string());
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    if (!f) throw std::runtime_error("cannot write " + path.string());
}

std::vector<std::uint8_t> inflateAll(const std::uint8_t* data, std::size_t size, std::size_t expected) {
    std::vector<std::uint8_t> out(std::max<std::size_t>(expected, 1024));
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) throw std::runtime_error("inflateInit failed");
    zs.next_in = const_cast<Bytef*>(data);
    zs.avail_in = static_cast<uInt>(size);
    std::size_t written = 0;
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
    if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) throw std::runtime_error("zlib data is corrupt");
    out.resize(written);
    return out;
}

// SWF JPEGs may contain stray EOI+SOI pairs (0xFFD9FFD8), e.g. the "erroneous header"
// written by old tools or the seam between JPEGTables and DefineBits data.
std::vector<std::uint8_t> cleanJPEG(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out;
    out.reserve(size);
    // Old SWF encoders emit stray markers: EOI+SOI pairs between tables and image, or a
    // doubled SOI at the start. SOI never occurs inside entropy-coded data (0xff is stuffed),
    // so every SOI after the first and every EOI before the end can be dropped.
    for (std::size_t i = 0; i < size;) {
        if (i + 1 < size && data[i] == 0xff && data[i + 1] == 0xd8 && !out.empty()) { i += 2; continue; }
        if (i + 1 < size && data[i] == 0xff && data[i + 1] == 0xd9 && i + 2 < size) {
            i += 2;
            if (out.empty()) { out.push_back(0xff); out.push_back(0xd8); } // keep one SOI at the start
            continue;
        }
        out.push_back(data[i++]);
    }
    return out;
}

enum class ImageKind { JPEG, PNG, GIF };

ImageKind detectImage(const std::uint8_t* d, std::size_t n) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return ImageKind::PNG;
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') return ImageKind::GIF;
    return ImageKind::JPEG;
}

// Read width/height from a PNG IHDR or GIF logical screen descriptor.
bool rawImageSize(ImageKind k, const std::uint8_t* d, std::size_t n, int& w, int& h) {
    if (k == ImageKind::PNG && n >= 24) {
        w = static_cast<int>((d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19]);
        h = static_cast<int>((d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23]);
        return true;
    }
    if (k == ImageKind::GIF && n >= 10) {
        w = le16(d + 6);
        h = le16(d + 8);
        return true;
    }
    return false;
}

void unpremultiply(ImageRGBA& img) {
    for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        const unsigned a = img.pixels[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            img.pixels[i + c] = static_cast<std::uint8_t>(std::min(255u, (img.pixels[i + c] * 255u + a / 2) / a));
        }
    }
}

// DefineBitsLossless (20) / DefineBitsLossless2 (36).
ImageRGBA decodeLossless(const std::uint8_t* p, std::size_t len, bool alpha) {
    if (len < 7) throw std::runtime_error("lossless bitmap too short");
    const auto format = p[2];
    ImageRGBA img;
    img.width = le16(p + 3);
    img.height = le16(p + 5);
    const auto w = static_cast<std::size_t>(img.width), h = static_cast<std::size_t>(img.height);
    img.pixels.assign(w * h * 4, 0);
    if (format == 3) {
        if (len < 8) throw std::runtime_error("colormapped bitmap too short");
        const std::size_t colors = static_cast<std::size_t>(p[7]) + 1;
        const std::size_t entry = alpha ? 4 : 3;
        const std::size_t stride = (w + 3) & ~static_cast<std::size_t>(3);
        const auto raw = inflateAll(p + 8, len - 8, colors * entry + stride * h);
        if (raw.size() < colors * entry + stride * h) throw std::runtime_error("colormapped bitmap data truncated");
        const auto* pixels = raw.data() + colors * entry;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t idx = pixels[y * stride + x];
                auto* out = &img.pixels[(y * w + x) * 4];
                if (idx >= colors) continue; // out-of-range index = transparent
                const auto* c = raw.data() + idx * entry;
                out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
                out[3] = alpha ? c[3] : 255;
            }
        }
    } else if (format == 4) { // PIX15, only valid in DefineBitsLossless
        const std::size_t stride = (w * 2 + 3) & ~static_cast<std::size_t>(3);
        const auto raw = inflateAll(p + 7, len - 7, stride * h);
        if (raw.size() < stride * h) throw std::runtime_error("15-bit bitmap data truncated");
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const std::uint16_t v = static_cast<std::uint16_t>((raw[y * stride + x * 2] << 8) | raw[y * stride + x * 2 + 1]);
                auto* out = &img.pixels[(y * w + x) * 4];
                out[0] = static_cast<std::uint8_t>(((v >> 10) & 0x1f) * 255 / 31);
                out[1] = static_cast<std::uint8_t>(((v >> 5) & 0x1f) * 255 / 31);
                out[2] = static_cast<std::uint8_t>((v & 0x1f) * 255 / 31);
                out[3] = 255;
            }
        }
    } else if (format == 5) { // (A|X)RGB
        const auto raw = inflateAll(p + 7, len - 7, w * h * 4);
        if (raw.size() < w * h * 4) throw std::runtime_error("32-bit bitmap data truncated");
        for (std::size_t i = 0; i < w * h; ++i) {
            img.pixels[i * 4 + 0] = raw[i * 4 + 1];
            img.pixels[i * 4 + 1] = raw[i * 4 + 2];
            img.pixels[i * 4 + 2] = raw[i * 4 + 3];
            img.pixels[i * 4 + 3] = alpha ? raw[i * 4] : 255;
        }
    } else {
        throw std::runtime_error("unknown lossless bitmap format " + std::to_string(format));
    }
    if (alpha) unpremultiply(img); // Flash stores DefineBitsLossless2 premultiplied
    return img;
}

// ---- sound ----

void wav(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& pcm, unsigned rate, unsigned channels,
         unsigned bits) {
    auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    auto u16 = [&](std::uint16_t v) { out.push_back(static_cast<std::uint8_t>(v)); out.push_back(static_cast<std::uint8_t>(v >> 8)); };
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    u32(static_cast<std::uint32_t>(36 + pcm.size()));
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(channels));
    u32(rate);
    u32(rate * channels * bits / 8);
    u16(static_cast<std::uint16_t>(channels * bits / 8));
    u16(static_cast<std::uint16_t>(bits));
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    u32(static_cast<std::uint32_t>(pcm.size()));
    out.insert(out.end(), pcm.begin(), pcm.end());
}

const unsigned kRates[4] = {5512, 11025, 22050, 44100};

// Converts one sound payload (format + raw data) to a file. Returns the extension or "" if unsupported.
std::string convertSound(unsigned format, unsigned rate, bool is16, bool stereo, const std::uint8_t* data,
                         std::size_t size, std::vector<std::uint8_t>& out) {
    const unsigned channels = stereo ? 2 : 1;
    switch (format) {
        case 2: // MP3
            out.assign(data, data + size);
            return ".mp3";
        case 0: case 3: { // PCM (format 0 is platform-endian; every real SWF is little-endian)
            std::vector<std::uint8_t> pcm(data, data + size);
            wav(out, pcm, rate, channels, is16 ? 16 : 8);
            return ".wav";
        }
        case 1:
            wav(out, decodeAdpcm(data, size, channels), rate, channels, 16);
            return ".wav";
        default:
            out.assign(data, data + size);
            return "";
    }
}

std::string soundFormatName(unsigned f) {
    switch (f) {
        case 0: case 3: return "pcm";
        case 1: return "adpcm";
        case 2: return "mp3";
        case 4: case 5: case 6: return "nellymoser";
        case 11: return "speex";
        default: return "format" + std::to_string(f);
    }
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

struct Character {
    std::string kind;
    std::string file;
    int width = 0, height = 0;
    Rect bounds;
    bool hasBounds = false;
    std::uint32_t frames = 0;
    std::vector<std::string> names;
    std::string note;
};

std::string timelineName(std::uint16_t spriteId) {
    return spriteId == 0 ? "main" : "sprite_" + std::to_string(spriteId);
}

} // namespace

DecodedBitmap decodeBitmapTag(const SWFDocument& doc, const TagRecord& t, const std::uint8_t* jpegTables,
                              std::size_t jpegTablesLen) {
    DecodedBitmap out;
    const auto* p = doc.payload(t);
    const std::size_t n = t.length;
    if (n < 2) throw std::runtime_error("bitmap tag too short");
    if (t.code == 20 || t.code == 36) {
        out.image = decodeLossless(p, n, t.code == 36);
        out.decoded = true;
        out.width = out.image.width;
        out.height = out.image.height;
        return out;
    }
    std::size_t imgStart = 2, imgLen = n - 2, alphaStart = 0;
    if (t.code == 35 || t.code == 90) {
        if (n < 6) throw std::runtime_error("JPEG3 header truncated");
        imgLen = le32(p + 2);
        imgStart = t.code == 90 ? 8 : 6;
        alphaStart = imgStart + imgLen;
        if (alphaStart > n) throw std::runtime_error("alpha offset outside tag");
    }
    std::vector<std::uint8_t> img;
    if (t.code == 6) {
        if (jpegTables && jpegTablesLen > 4) img = cleanJPEG(jpegTables, jpegTablesLen);
        // Drop the tables' EOI and the image's SOI so the two halves form one stream.
        if (img.size() >= 2 && img[img.size() - 2] == 0xff && img.back() == 0xd9) img.resize(img.size() - 2);
        auto body = cleanJPEG(p + imgStart, imgLen);
        std::size_t skip = img.empty() ? 0 : (body.size() >= 2 && body[0] == 0xff && body[1] == 0xd8 ? 2 : 0);
        img.insert(img.end(), body.begin() + static_cast<std::ptrdiff_t>(skip), body.end());
    } else {
        img.assign(p + imgStart, p + imgStart + imgLen);
    }
    const auto kind = detectImage(img.data(), img.size());
    if (kind != ImageKind::JPEG) {
        const bool ok = kind == ImageKind::PNG ? decodePNG(img.data(), img.size(), out.image, &out.error)
                                               : decodeGIF(img.data(), img.size(), out.image, &out.error);
        if (ok) {
            out.decoded = true;
            out.width = out.image.width;
            out.height = out.image.height;
            return out;
        }
        out.originalExtension = kind == ImageKind::PNG ? ".png" : ".gif";
        rawImageSize(kind, img.data(), img.size(), out.width, out.height);
        out.original = std::move(img);
        return out;
    }
    img = cleanJPEG(img.data(), img.size());
    if (!decodeJPEG(img, out.image, &out.error)) {
        out.error = "jpeg not decoded: " + out.error;
        out.originalExtension = ".jpg";
        out.original = std::move(img);
        return out;
    }
    if (alphaStart && alphaStart < n) {
        const auto alpha = inflateAll(p + alphaStart, n - alphaStart, out.image.pixels.size() / 4);
        const auto count = std::min(alpha.size(), out.image.pixels.size() / 4);
        for (std::size_t i = 0; i < count; ++i) out.image.pixels[i * 4 + 3] = alpha[i];
        unpremultiply(out.image); // Flash multiplies the JPEG colour by the alpha plane
    }
    out.decoded = true;
    out.width = out.image.width;
    out.height = out.image.height;
    return out;
}

void extractAssets(const SWFDocument& doc, const std::string& outDir, AssetReport& r) {
    const fs::path root(outDir);
    for (const auto* sub : {"bitmaps", "sounds", "streams", "shapes"}) fs::create_directories(root / sub);

    std::map<std::uint16_t, Character> chars;
    std::map<std::uint16_t, BitmapInfo> bitmaps;
    const std::uint8_t* jpegTables = nullptr;
    std::size_t jpegTablesLen = 0;

    // Pass 1: bitmaps (shapes reference them), sounds, character inventory.
    struct Stream {
        unsigned format{}, rate{};
        bool is16{}, stereo{};
        std::vector<std::uint8_t> data;
    };
    std::map<std::uint16_t, Stream> streams;

    for (const auto& t : doc.tags) {
        const auto* p = doc.payload(t);
        const std::size_t n = t.length;
        try {
            switch (t.code) {
                case 8: // JPEGTables
                    jpegTables = p;
                    jpegTablesLen = n;
                    break;
                case 6: case 21: case 35: case 90: // DefineBits / JPEG2 / JPEG3 / JPEG4
                case 20: case 36: {                  // DefineBitsLossless(2)
                    if (n < 2) break;
                    const auto id = le16(p);
                    auto& ch = chars[id];
                    ch.kind = "bitmap";
                    const auto bm = decodeBitmapTag(doc, t, jpegTables, jpegTablesLen);
                    const std::string base = std::to_string(id);
                    if (bm.decoded) {
                        ch.file = base + ".png";
                        writeFile(root / "bitmaps" / ch.file, encodePNG(bm.image));
                    } else {
                        // PNG/GIF payloads (and JPEGs without a decoder) are stored verbatim.
                        ch.file = base + bm.originalExtension;
                        writeFile(root / "bitmaps" / ch.file, bm.original);
                        ++r.bitmapsRaw;
                        if (!bm.error.empty()) {
                            ch.note = bm.error;
                            if (haveJPEGDecoder()) { ++r.bitmapsFailed; r.problem("bitmap " + base + ": " + bm.error); }
                        }
                    }
                    ch.width = bm.width;
                    ch.height = bm.height;
                    bitmaps[id] = {ch.width, ch.height, ch.file};
                    ++r.bitmaps;
                    break;
                }
                case 14: { // DefineSound
                    if (n < 7) break;
                    const auto id = le16(p);
                    const auto flags = p[2];
                    const unsigned format = flags >> 4;
                    auto& ch = chars[id];
                    ch.kind = "sound";
                    std::size_t start = 7;
                    if (format == 2) start += 2; // MP3 seekSamples
                    std::vector<std::uint8_t> bytes;
                    const auto ext = convertSound(format, kRates[(flags >> 2) & 3], (flags & 2) != 0, (flags & 1) != 0,
                                                  p + std::min(start, n), n - std::min(start, n), bytes);
                    ch.file = std::to_string(id) + (ext.empty() ? "." + soundFormatName(format) : ext);
                    if (ext.empty()) { ++r.soundsUnsupported; ch.note = soundFormatName(format) + " kept raw"; }
                    writeFile(root / "sounds" / ch.file, bytes);
                    ++r.sounds;
                    break;
                }
                case 18: case 45: { // SoundStreamHead(2)
                    if (n < 4) break;
                    auto& s = streams[t.spriteId];
                    s = Stream{};
                    s.format = p[1] >> 4;
                    s.rate = kRates[(p[1] >> 2) & 3];
                    s.is16 = (p[1] & 2) != 0;
                    s.stereo = (p[1] & 1) != 0;
                    break;
                }
                case 19: { // SoundStreamBlock
                    auto it = streams.find(t.spriteId);
                    if (it == streams.end()) break;
                    auto& s = it->second;
                    const std::size_t skip = s.format == 2 ? 4 : 0; // MP3: sampleCount + seekSamples
                    if (n > skip) s.data.insert(s.data.end(), p + skip, p + n);
                    break;
                }
                case 2: case 22: case 32: case 83:
                    chars[le16(p)].kind = "shape";
                    break;
                case 39: {
                    auto& ch = chars[le16(p)];
                    ch.kind = "sprite";
                    ch.frames = le16(p + 2);
                    ++r.sprites;
                    break;
                }
                case 46: case 84: chars[le16(p)].kind = "morphshape"; break;
                case 7: case 34: chars[le16(p)].kind = "button"; break;
                case 10: case 48: case 75: case 91: chars[le16(p)].kind = "font"; break;
                case 11: case 33: chars[le16(p)].kind = "text"; break;
                case 37: chars[le16(p)].kind = "edittext"; break;
                case 60: chars[le16(p)].kind = "video"; break;
                case 87: chars[le16(p)].kind = "binarydata"; break;
                case 56: case 76: { // ExportAssets / SymbolClass
                    ByteReader br(doc.data, t.offset);
                    const auto count = br.u16();
                    for (std::uint16_t i = 0; i < count && br.pos() < t.offset + t.length; ++i) {
                        const auto id = br.u16();
                        chars[id].names.push_back(br.cstring());
                        ++r.symbols;
                    }
                    break;
                }
                default:
                    break;
            }
        } catch (const std::exception& e) {
            if (t.code == 20 || t.code == 36 || t.code == 6 || t.code == 21 || t.code == 35 || t.code == 90) ++r.bitmapsFailed;
            r.problem("tag " + std::to_string(t.code) + " at " + std::to_string(t.offset) + ": " + e.what());
        }
    }

    for (auto& [sprite, s] : streams) {
        if (s.data.empty()) continue;
        std::vector<std::uint8_t> bytes;
        const auto ext = convertSound(s.format, s.rate, s.is16, s.stereo, s.data.data(), s.data.size(), bytes);
        writeFile(root / "streams" / (timelineName(sprite) + (ext.empty() ? "." + soundFormatName(s.format) : ext)), bytes);
        ++r.streams;
    }

    // Pass 2: shapes, now that bitmap sizes are known.
    for (const auto& t : doc.tags) {
        if (t.code != 2 && t.code != 22 && t.code != 32 && t.code != 83) continue;
        const auto id = le16(doc.payload(t));
        try {
            const auto shape = parseShape(doc, t);
            auto& ch = chars[id];
            ch.file = std::to_string(id) + ".svg";
            ch.bounds = shape.bounds;
            ch.hasBounds = true;
            writeText(root / "shapes" / ch.file, shapeToSVG(shape, "../bitmaps", bitmaps));
            ++r.shapes;
        } catch (const std::exception& e) {
            ++r.shapesFailed;
            r.problem("shape " + std::to_string(id) + ": " + e.what());
        }
    }

    for (const auto& [id, ch] : chars) {
        if (ch.kind != "bitmap" && ch.kind != "sound" && ch.kind != "shape" && ch.kind != "sprite" && !ch.kind.empty()) {
            ++r.skipped[ch.kind];
        }
    }

    // Manifest consumed by the generated runtime and by later pipeline stages.
    std::ostringstream m;
    m << "{\n  \"source\": \"" << jsonEscape(doc.sourceName) << "\",\n";
    m << "  \"version\": " << static_cast<unsigned>(doc.version) << ",\n";
    m << "  \"stage\": {\"width\": " << (doc.frameRect.xmax - doc.frameRect.xmin) / 20.0
      << ", \"height\": " << (doc.frameRect.ymax - doc.frameRect.ymin) / 20.0 << ", \"fps\": " << doc.fps
      << ", \"frames\": " << doc.frameCount << "},\n";
    m << "  \"characters\": [\n";
    bool first = true;
    for (const auto& [id, ch] : chars) {
        if (ch.kind.empty()) continue; // referenced by SymbolClass only (e.g. id 0 = document class)
        m << (first ? "" : ",\n") << "    {\"id\": " << id << ", \"kind\": \"" << ch.kind << "\"";
        first = false;
        if (!ch.file.empty()) {
            const char* dir = ch.kind == "bitmap" ? "bitmaps/" : ch.kind == "sound" ? "sounds/" : "shapes/";
            m << ", \"file\": \"" << dir << jsonEscape(ch.file) << "\"";
        }
        if (ch.width) m << ", \"width\": " << ch.width << ", \"height\": " << ch.height;
        if (ch.hasBounds) {
            m << ", \"bounds\": [" << ch.bounds.xmin / 20.0 << ", " << ch.bounds.ymin / 20.0 << ", "
              << ch.bounds.xmax / 20.0 << ", " << ch.bounds.ymax / 20.0 << "]";
        }
        if (ch.frames) m << ", \"frames\": " << ch.frames;
        if (!ch.names.empty()) {
            m << ", \"names\": [";
            for (std::size_t i = 0; i < ch.names.size(); ++i) m << (i ? ", " : "") << "\"" << jsonEscape(ch.names[i]) << "\"";
            m << "]";
        }
        if (!ch.note.empty()) m << ", \"note\": \"" << jsonEscape(ch.note) << "\"";
        m << "}";
    }
    m << "\n  ],\n  \"documentClass\": \"";
    if (auto it = chars.find(0); it != chars.end() && !it->second.names.empty()) m << jsonEscape(it->second.names.front());
    m << "\"\n}\n";
    writeText(root / "manifest.json", m.str());
}

void printAssetReport(const AssetReport& r, std::ostream& out) {
    out << "Assets: " << r.bitmaps << " bitmaps (" << r.bitmapsRaw << " kept in original format, "
        << r.bitmapsFailed << " failed), " << r.shapes << " shapes -> SVG (" << r.shapesFailed << " failed), "
        << r.sounds << " sounds (" << r.soundsUnsupported << " unsupported codec), " << r.streams
        << " stream sounds, " << r.sprites << " sprites, " << r.symbols << " symbol names\n";
    if (!r.skipped.empty()) {
        out << "        not converted yet:";
        for (const auto& [kind, n] : r.skipped) out << " " << kind << "=" << n;
        out << "\n";
    }
    if (!r.problems.empty()) {
        out << "Asset problems (first " << r.problems.size() << "):\n";
        for (const auto& p : r.problems) out << "  - " << p << "\n";
    }
}

} // namespace flashport

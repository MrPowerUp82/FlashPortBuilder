#include "flashport/SWFStructures.hpp"
#include "flashport/ByteReader.hpp"
#include <stdexcept>

namespace flashport {
namespace {

std::string readCString(const SWFDocument& doc, std::size_t& pos, std::size_t end) {
    std::string s;
    while (pos < end && doc.data[pos] != 0) s.push_back(static_cast<char>(doc.data[pos++]));
    if (pos >= end) throw std::runtime_error("unterminated string");
    ++pos;
    return s;
}

} // namespace

std::size_t skipFilterList(const SWFDocument& doc, std::size_t pos, std::size_t end, std::uint8_t& count) {
    if (pos >= end) throw std::runtime_error("missing filter list");
    count = doc.data[pos++];
    for (unsigned i = 0; i < count; ++i) {
        if (pos >= end) throw std::runtime_error("truncated filter list");
        const auto id = doc.data[pos++];
        switch (id) {
            case 0: pos += 23; break;                       // DropShadow
            case 1: pos += 9; break;                        // Blur
            case 2: pos += 15; break;                       // Glow
            case 3: pos += 27; break;                       // Bevel
            case 4: case 7: {                               // GradientGlow / GradientBevel
                if (pos >= end) throw std::runtime_error("truncated gradient filter");
                const auto n = doc.data[pos++];
                pos += 5u * n + 19;
                break;
            }
            case 5: {                                       // Convolution
                if (pos + 2 > end) throw std::runtime_error("truncated convolution filter");
                const auto x = doc.data[pos], y = doc.data[pos + 1];
                pos += 2 + 8 + 4u * x * y + 5;
                break;
            }
            case 6: pos += 80; break;                       // ColorMatrix
            default: throw std::runtime_error("unknown filter id " + std::to_string(id));
        }
    }
    if (pos > end) throw std::runtime_error("filter list overruns tag");
    return pos;
}

Rect readRect(BitReader& bits) {
    Rect r;
    const auto n = bits.bits(5);
    r.xmin = bits.signedBits(n);
    r.xmax = bits.signedBits(n);
    r.ymin = bits.signedBits(n);
    r.ymax = bits.signedBits(n);
    return r;
}

Matrix readMatrix(BitReader& bits) {
    bits.align();
    Matrix m;
    if (bits.flag()) {
        const auto n = bits.bits(5);
        m.a = bits.fixedBits(n);
        m.d = bits.fixedBits(n);
    }
    if (bits.flag()) {
        const auto n = bits.bits(5);
        m.b = bits.fixedBits(n);
        m.c = bits.fixedBits(n);
    }
    const auto n = bits.bits(5);
    m.tx = bits.signedBits(n);
    m.ty = bits.signedBits(n);
    bits.align();
    return m;
}

ColorTransform readColorTransform(BitReader& bits, bool withAlpha) {
    bits.align();
    ColorTransform c;
    const bool hasAdd = bits.flag();
    const bool hasMul = bits.flag();
    const auto n = bits.bits(4);
    if (hasMul) {
        c.rMul = bits.signedBits(n) / 256.0;
        c.gMul = bits.signedBits(n) / 256.0;
        c.bMul = bits.signedBits(n) / 256.0;
        if (withAlpha) c.aMul = bits.signedBits(n) / 256.0;
    }
    if (hasAdd) {
        c.rAdd = bits.signedBits(n);
        c.gAdd = bits.signedBits(n);
        c.bAdd = bits.signedBits(n);
        if (withAlpha) c.aAdd = bits.signedBits(n);
    }
    bits.align();
    return c;
}

PlaceObject parsePlaceObject(const SWFDocument& doc, const TagRecord& tag) {
    PlaceObject p;
    const std::size_t end = tag.offset + tag.length;
    std::size_t pos = tag.offset;
    auto u8 = [&]() -> std::uint8_t {
        if (pos >= end) throw std::runtime_error("PlaceObject truncated");
        return doc.data[pos++];
    };
    auto u16 = [&]() -> std::uint16_t { const auto lo = u8(); return static_cast<std::uint16_t>(lo | (u8() << 8)); };
    auto u32 = [&]() -> std::uint32_t { const std::uint32_t lo = u16(); return lo | (static_cast<std::uint32_t>(u16()) << 16); };
    auto matrix = [&]() {
        BitReader bits(doc.data.data(), end, pos);
        auto m = readMatrix(bits);
        pos = bits.nextByte();
        return m;
    };
    auto cxform = [&](bool alpha) {
        BitReader bits(doc.data.data(), end, pos);
        auto c = readColorTransform(bits, alpha);
        pos = bits.nextByte();
        return c;
    };

    if (tag.code == 4) { // PlaceObject (v1)
        p.version = 1;
        p.characterId = u16();
        p.depth = u16();
        p.matrix = matrix();
        if (pos < end) p.colorTransform = cxform(false);
        return p;
    }

    p.version = tag.code == 70 ? 3 : 2;
    const auto flags = u8();
    const std::uint8_t flags2 = p.version == 3 ? u8() : 0;
    p.depth = u16();
    p.move = (flags & 0x01) != 0;
    if (p.version == 3 && ((flags2 & 0x08) || ((flags2 & 0x10) && (flags & 0x02)))) {
        p.className = readCString(doc, pos, end);
    }
    if (flags & 0x02) p.characterId = u16();
    if (flags & 0x04) p.matrix = matrix();
    if (flags & 0x08) p.colorTransform = cxform(true);
    if (flags & 0x10) p.ratio = u16();
    if (flags & 0x20) p.name = readCString(doc, pos, end);
    if (flags & 0x40) p.clipDepth = u16();
    if (p.version == 3) {
        if (flags2 & 0x01) pos = skipFilterList(doc, pos, end, p.filterCount);
        if (flags2 & 0x02) p.blendMode = u8();
        if (flags2 & 0x04) p.cacheAsBitmap = u8() != 0;
        if (flags2 & 0x20) { p.visible = u8() != 0; }
        if (flags2 & 0x40) { (void)u32(); } // opaque background RGBA
    }
    if (flags & 0x80) {
        const bool wide = doc.version >= 6;
        (void)u16(); // reserved
        if (wide) (void)u32(); else (void)u16(); // all event flags
        while (pos < end) {
            const std::uint32_t events = wide ? u32() : u16();
            if (events == 0) break;
            ClipAction ca;
            ca.events = events;
            std::uint32_t size = u32();
            if (wide && (events & 0x00020000u)) { // ClipEventKeyPress
                ca.keyCode = u8();
                if (size > 0) --size;
            }
            if (pos + size > end) throw std::runtime_error("clip action overruns PlaceObject");
            ca.actionOffset = pos;
            ca.actionLength = size;
            pos += size;
            p.clipActions.push_back(ca);
        }
    }
    return p;
}

std::vector<AVM1Source> findAVM1Sources(const SWFDocument& doc) {
    std::vector<AVM1Source> out;
    auto where = [](const TagRecord& t) {
        return t.spriteId == 0 ? "frame " + std::to_string(t.frame) + " of main timeline"
                               : "frame " + std::to_string(t.frame) + " of sprite " + std::to_string(t.spriteId);
    };
    for (const auto& t : doc.tags) {
        const std::size_t end = t.offset + t.length;
        switch (t.code) {
            case 12: // DoAction
                out.push_back({"DoAction, " + where(t), t.offset, t.length, t.spriteId, t.frame});
                break;
            case 59: // DoInitAction
                if (t.length >= 2) {
                    const auto id = static_cast<std::uint16_t>(doc.data[t.offset] | (doc.data[t.offset + 1] << 8));
                    out.push_back({"DoInitAction for sprite " + std::to_string(id), t.offset + 2, t.length - 2u,
                                   t.spriteId, t.frame});
                }
                break;
            case 7: { // DefineButton: records until 0 flag byte, then a single action list
                if (t.length < 3) break;
                const auto id = static_cast<std::uint16_t>(doc.data[t.offset] | (doc.data[t.offset + 1] << 8));
                std::size_t pos = t.offset + 2;
                while (pos < end && doc.data[pos] != 0) {
                    pos += 5; // flags, character id, depth
                    BitReader bits(doc.data.data(), end, pos);
                    (void)readMatrix(bits);
                    pos = bits.nextByte();
                }
                ++pos;
                if (pos < end) out.push_back({"DefineButton " + std::to_string(id), pos, end - pos, t.spriteId, t.frame});
                break;
            }
            case 34: { // DefineButton2: actionOffset points at the BUTTONCONDACTION list
                if (t.length < 5) break;
                const auto id = static_cast<std::uint16_t>(doc.data[t.offset] | (doc.data[t.offset + 1] << 8));
                const std::size_t offField = t.offset + 3;
                const auto actionOffset = static_cast<std::uint16_t>(doc.data[offField] | (doc.data[offField + 1] << 8));
                if (actionOffset == 0) break;
                std::size_t pos = offField + actionOffset;
                while (pos + 4 <= end) {
                    const auto size = static_cast<std::uint16_t>(doc.data[pos] | (doc.data[pos + 1] << 8));
                    const auto cond = static_cast<std::uint16_t>(doc.data[pos + 2] | (doc.data[pos + 3] << 8));
                    const std::size_t codeStart = pos + 4;
                    const std::size_t codeEnd = size == 0 ? end : std::min(end, pos + size);
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "0x%04x", cond);
                    if (codeEnd > codeStart) {
                        out.push_back({"DefineButton2 " + std::to_string(id) + " cond " + buf, codeStart,
                                       codeEnd - codeStart, t.spriteId, t.frame});
                    }
                    if (size == 0) break;
                    pos += size;
                }
                break;
            }
            case 26: case 70: { // PlaceObject2/3 clip actions
                if (t.length == 0 || (doc.data[t.offset] & 0x80) == 0) break;
                const auto p = parsePlaceObject(doc, t);
                for (const auto& ca : p.clipActions) {
                    out.push_back({"onClipEvent(" + clipEventNames(ca.events) + ") depth " + std::to_string(p.depth) +
                                       " " + where(t),
                                   ca.actionOffset, ca.actionLength, t.spriteId, t.frame});
                }
                break;
            }
            default:
                break;
        }
    }
    return out;
}

std::string clipEventNames(std::uint32_t e) {
    static const char* names[] = {
        "keyUp", "keyDown", "mouseUp", "mouseDown", "mouseMove", "unload", "enterFrame", "load",
        "dragOver", "rollOut", "rollOver", "releaseOutside", "release", "press", "initialize", "data",
        "?", "?", "?", "?", "?", "construct", "keyPress", "dragOut"};
    std::string s;
    // Byte-wise MSB-first naming, matching the SWF spec field order.
    for (int byte = 0; byte < 3; ++byte) {
        for (int bit = 7; bit >= 0; --bit) {
            if (e & (1u << (byte * 8 + bit))) {
                const auto idx = static_cast<std::size_t>(byte * 8 + (7 - bit));
                if (!s.empty()) s += "|";
                s += names[idx];
            }
        }
    }
    return s.empty() ? "none" : s;
}

} // namespace flashport

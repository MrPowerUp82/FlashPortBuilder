#include "flashport/Shape.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace flashport {
namespace {

RGBA readColor(BitReader& b, bool alpha) {
    RGBA c;
    c.r = b.u8();
    c.g = b.u8();
    c.b = b.u8();
    c.a = alpha ? b.u8() : 255;
    return c;
}

std::uint16_t readCount(BitReader& b, std::uint8_t shapeVersion) {
    std::uint16_t n = b.u8();
    if (n == 0xff && shapeVersion >= 2) n = b.u16();
    return n;
}

FillStyle readFillStyle(BitReader& b, std::uint8_t v) {
    FillStyle f;
    const auto type = b.u8();
    const bool alpha = v >= 3;
    switch (type) {
        case 0x00:
            f.type = FillStyle::Type::Solid;
            f.color = readColor(b, alpha);
            break;
        case 0x10: case 0x12: case 0x13: {
            f.type = type == 0x10 ? FillStyle::Type::Linear : type == 0x12 ? FillStyle::Type::Radial : FillStyle::Type::Focal;
            f.matrix = readMatrix(b);
            const auto flags = b.u8();
            f.spread = (flags >> 6) & 3;
            f.interpolation = (flags >> 4) & 3;
            const auto n = flags & 0x0f;
            for (int i = 0; i < n; ++i) {
                GradientStop s;
                s.ratio = b.u8();
                s.color = readColor(b, alpha);
                f.stops.push_back(s);
            }
            if (type == 0x13) f.focal = static_cast<std::int16_t>(b.u16()) / 256.0;
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43:
            f.type = FillStyle::Type::Bitmap;
            f.bitmapId = b.u16();
            f.matrix = readMatrix(b);
            f.repeat = type == 0x40 || type == 0x42;
            f.smooth = type == 0x40 || type == 0x41;
            break;
        default:
            throw std::runtime_error("unknown fill style type " + std::to_string(type));
    }
    return f;
}

void readStyles(BitReader& b, std::uint8_t v, ShapeGroup& g) {
    const auto nf = readCount(b, v);
    for (std::uint16_t i = 0; i < nf; ++i) g.fills.push_back(readFillStyle(b, v));
    const auto nl = readCount(b, v);
    for (std::uint16_t i = 0; i < nl; ++i) {
        LineStyle l;
        l.width = b.u16();
        if (v >= 4) {
            const auto f1 = b.u8();
            const auto f2 = b.u8();
            l.startCap = (f1 >> 6) & 3;
            l.join = (f1 >> 4) & 3;
            l.hasFill = (f1 & 0x08) != 0;
            l.noClose = (f2 & 0x04) != 0;
            l.endCap = f2 & 3;
            if (l.join == 2) l.miterLimit = b.u16() / 256.0;
            if (l.hasFill) {
                l.fill = readFillStyle(b, v);
                l.color = l.fill.type == FillStyle::Type::Solid ? l.fill.color
                          : !l.fill.stops.empty() ? l.fill.stops.front().color : RGBA{};
            } else {
                l.color = readColor(b, true);
            }
        } else {
            l.color = readColor(b, v >= 3);
        }
        g.lines.push_back(l);
    }
}

struct PointHash {
    std::size_t operator()(const Point& p) const {
        return std::hash<std::int64_t>()((static_cast<std::int64_t>(p.x) << 32) ^ static_cast<std::uint32_t>(p.y));
    }
};

// Join an unordered set of directed edges into closed contours.
std::vector<std::vector<Edge>> buildContours(const std::vector<Edge>& edges) {
    std::unordered_multimap<Point, std::size_t, PointHash> byStart;
    for (std::size_t i = 0; i < edges.size(); ++i) byStart.emplace(edges[i].from, i);
    std::vector<bool> used(edges.size(), false);
    std::vector<std::vector<Edge>> contours;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        if (used[i]) continue;
        std::vector<Edge> c;
        std::size_t cur = i;
        while (true) {
            used[cur] = true;
            c.push_back(edges[cur]);
            if (c.back().to == c.front().from) break;
            std::size_t next = edges.size();
            auto range = byStart.equal_range(c.back().to);
            for (auto it = range.first; it != range.second; ++it) {
                if (!used[it->second]) { next = it->second; break; }
            }
            if (next == edges.size()) break; // open contour: SVG closes it implicitly
            cur = next;
        }
        contours.push_back(std::move(c));
    }
    return contours;
}

// Strokes keep drawing order; a discontinuity starts a new polyline.
std::vector<std::vector<Edge>> buildPolylines(const std::vector<Edge>& edges) {
    std::vector<std::vector<Edge>> out;
    for (const auto& e : edges) {
        if (out.empty() || !(out.back().back().to == e.from)) out.emplace_back();
        out.back().push_back(e);
    }
    return out;
}

void flushGroup(ShapeGroup& g, std::vector<std::vector<Edge>>& fillEdges, std::vector<std::vector<Edge>>& lineEdges) {
    for (std::size_t s = 1; s < fillEdges.size(); ++s) {
        if (fillEdges[s].empty()) continue;
        g.fillPaths.push_back({static_cast<std::uint32_t>(s - 1), buildContours(fillEdges[s])});
    }
    for (std::size_t s = 1; s < lineEdges.size(); ++s) {
        if (lineEdges[s].empty()) continue;
        g.linePaths.push_back({static_cast<std::uint32_t>(s - 1), buildPolylines(lineEdges[s])});
    }
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

std::string px(std::int32_t twips) { return num(twips / 20.0); }

std::string hexColor(const RGBA& c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string pathData(const std::vector<std::vector<Edge>>& contours, bool close) {
    std::ostringstream d;
    for (const auto& c : contours) {
        d << "M" << px(c.front().from.x) << " " << px(c.front().from.y);
        for (const auto& e : c) {
            if (e.curved) d << "Q" << px(e.control.x) << " " << px(e.control.y) << " " << px(e.to.x) << " " << px(e.to.y);
            else d << "L" << px(e.to.x) << " " << px(e.to.y);
        }
        if (close) d << "Z";
    }
    return d.str();
}

std::string matrixAttr(const Matrix& m, double scale) {
    return "matrix(" + num(m.a * scale) + " " + num(m.b * scale) + " " + num(m.c * scale) + " " +
           num(m.d * scale) + " " + num(m.tx / 20.0) + " " + num(m.ty / 20.0) + ")";
}

// Emits a <defs> entry for gradient/bitmap fills and returns the paint reference.
std::string paint(const FillStyle& f, const std::string& id, std::ostringstream& defs,
                  const std::string& bitmapDir, const std::map<std::uint16_t, BitmapInfo>& bitmaps,
                  double& opacity) {
    opacity = 1.0;
    switch (f.type) {
        case FillStyle::Type::Solid:
            opacity = f.color.a / 255.0;
            return hexColor(f.color);
        case FillStyle::Type::Linear:
        case FillStyle::Type::Radial:
        case FillStyle::Type::Focal: {
            // Gradient square is -16384..16384 twips = -819.2..819.2 px before the fill matrix.
            const char* spread = f.spread == 1 ? "reflect" : f.spread == 2 ? "repeat" : "pad";
            if (f.type == FillStyle::Type::Linear) {
                defs << "<linearGradient id=\"" << id << "\" gradientUnits=\"userSpaceOnUse\" x1=\"-819.2\" x2=\"819.2\" y1=\"0\" y2=\"0\"";
            } else {
                defs << "<radialGradient id=\"" << id << "\" gradientUnits=\"userSpaceOnUse\" cx=\"0\" cy=\"0\" r=\"819.2\"";
                if (f.type == FillStyle::Type::Focal) defs << " fx=\"" << num(f.focal * 819.2) << "\" fy=\"0\"";
            }
            defs << " spreadMethod=\"" << spread << "\" gradientTransform=\"" << matrixAttr(f.matrix, 1.0) << "\"";
            if (f.interpolation == 1) defs << " color-interpolation=\"linearRGB\"";
            defs << ">";
            for (const auto& s : f.stops) {
                defs << "<stop offset=\"" << num(s.ratio / 255.0) << "\" stop-color=\"" << hexColor(s.color) << "\"";
                if (s.color.a != 255) defs << " stop-opacity=\"" << num(s.color.a / 255.0) << "\"";
                defs << "/>";
            }
            defs << (f.type == FillStyle::Type::Linear ? "</linearGradient>" : "</radialGradient>") << "\n";
            return "url(#" + id + ")";
        }
        case FillStyle::Type::Bitmap: {
            const auto it = bitmaps.find(f.bitmapId);
            if (it == bitmaps.end()) return "none"; // 65535 = "no bitmap", or an unextracted image
            const auto& bm = it->second;
            // Bitmap matrices map image pixels to twips, so scale by 1/20 to reach SVG pixels.
            defs << "<pattern id=\"" << id << "\" patternUnits=\"userSpaceOnUse\" width=\"" << bm.width
                 << "\" height=\"" << bm.height << "\" patternTransform=\"" << matrixAttr(f.matrix, 1.0 / 20.0) << "\">"
                 << "<image href=\"" << bitmapDir << "/" << bm.file << "\" width=\"" << bm.width << "\" height=\""
                 << bm.height << "\"" << (f.smooth ? "" : " style=\"image-rendering:pixelated\"") << "/></pattern>\n";
            return "url(#" + id + ")";
        }
    }
    return "none";
}

} // namespace

Shape parseShape(const SWFDocument& doc, const TagRecord& tag) {
    Shape shape;
    switch (tag.code) {
        case 2: shape.version = 1; break;
        case 22: shape.version = 2; break;
        case 32: shape.version = 3; break;
        case 83: shape.version = 4; break;
        default: throw std::runtime_error("not a DefineShape tag");
    }
    const std::size_t end = tag.offset + tag.length;
    BitReader b(doc.data.data(), end, tag.offset);
    shape.id = b.u16();
    shape.bounds = readRect(b);
    if (shape.version == 4) {
        b.align();
        (void)readRect(b); // edge bounds
        const auto flags = b.u8();
        shape.evenOdd = (flags & 0x04) == 0; // UsesFillWindingRule -> nonzero
    }
    b.align();

    ShapeGroup group;
    readStyles(b, shape.version, group);
    readShapeRecords(b, shape, std::move(group));
    return shape;
}

// SHAPERECORDs following the (already read) style arrays of `group`; used by DefineShape
// and by font glyphs (which have one implicit fill style and no line styles).
void readShapeRecords(BitReader& b, Shape& shape, ShapeGroup group) {
    std::vector<std::vector<Edge>> fillEdges(group.fills.size() + 1), lineEdges(group.lines.size() + 1);
    unsigned fillBits = b.bits(4), lineBits = b.bits(4);
    Point pos;
    std::uint32_t fill0 = 0, fill1 = 0, line = 0;

    auto addEdge = [&](const Edge& e) {
        if (fill0 && fill0 < fillEdges.size()) fillEdges[fill0].push_back(e.reversed());
        if (fill1 && fill1 < fillEdges.size()) fillEdges[fill1].push_back(e);
        if (line && line < lineEdges.size()) lineEdges[line].push_back(e);
    };

    while (true) {
        if (b.flag()) { // edge record
            const bool straight = b.flag();
            const unsigned n = b.bits(4) + 2;
            Edge e;
            e.from = pos;
            if (straight) {
                std::int32_t dx = 0, dy = 0;
                if (b.flag()) { dx = b.signedBits(n); dy = b.signedBits(n); }
                else if (b.flag()) dy = b.signedBits(n);
                else dx = b.signedBits(n);
                e.to = {pos.x + dx, pos.y + dy};
            } else {
                const auto cdx = b.signedBits(n), cdy = b.signedBits(n);
                const auto adx = b.signedBits(n), ady = b.signedBits(n);
                e.curved = true;
                e.control = {pos.x + cdx, pos.y + cdy};
                e.to = {e.control.x + adx, e.control.y + ady};
            }
            addEdge(e);
            pos = e.to;
            continue;
        }
        const auto flags = b.bits(5);
        if (flags == 0) break; // EndShapeRecord
        if (flags & 0x01) { // MoveTo
            const auto n = b.bits(5);
            pos.x = b.signedBits(n);
            pos.y = b.signedBits(n);
        }
        if (flags & 0x02) fill0 = b.bits(fillBits);
        if (flags & 0x04) fill1 = b.bits(fillBits);
        if (flags & 0x08) line = b.bits(lineBits);
        if (flags & 0x10) { // NewStyles
            flushGroup(group, fillEdges, lineEdges);
            shape.groups.push_back(std::move(group));
            group = ShapeGroup{};
            readStyles(b, shape.version, group);
            fillEdges.assign(group.fills.size() + 1, {});
            lineEdges.assign(group.lines.size() + 1, {});
            fillBits = b.bits(4);
            lineBits = b.bits(4);
            fill0 = fill1 = line = 0;
        }
    }
    flushGroup(group, fillEdges, lineEdges);
    shape.groups.push_back(std::move(group));
}

std::string shapeToSVG(const Shape& shape, const std::string& bitmapDir,
                       const std::map<std::uint16_t, BitmapInfo>& bitmaps) {
    const auto& r = shape.bounds;
    std::ostringstream defs, body;
    int paintId = 0;
    const char* rule = shape.evenOdd ? "evenodd" : "nonzero";
    for (const auto& g : shape.groups) {
        for (const auto& p : g.fillPaths) {
            if (p.style >= g.fills.size()) continue;
            double opacity = 1;
            const auto fill = paint(g.fills[p.style], "p" + std::to_string(shape.id) + "_" + std::to_string(paintId++),
                                    defs, bitmapDir, bitmaps, opacity);
            body << "<path fill=\"" << fill << "\" fill-rule=\"" << rule << "\"";
            if (opacity < 1) body << " fill-opacity=\"" << num(opacity) << "\"";
            body << " d=\"" << pathData(p.contours, true) << "\"/>\n";
        }
        for (const auto& p : g.linePaths) {
            if (p.style >= g.lines.size()) continue;
            const auto& l = g.lines[p.style];
            double opacity = 1;
            std::string stroke;
            if (l.hasFill) {
                stroke = paint(l.fill, "p" + std::to_string(shape.id) + "_" + std::to_string(paintId++), defs, bitmapDir,
                               bitmaps, opacity);
            } else {
                stroke = hexColor(l.color);
                opacity = l.color.a / 255.0;
            }
            static const char* caps[] = {"round", "butt", "square", "round"};
            static const char* joins[] = {"round", "bevel", "miter", "round"};
            body << "<path fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\""
                 << (l.width == 0 ? "1" : num(l.width / 20.0)) << "\" stroke-linecap=\"" << caps[l.startCap & 3]
                 << "\" stroke-linejoin=\"" << joins[l.join & 3] << "\"";
            if (l.join == 2) body << " stroke-miterlimit=\"" << num(l.miterLimit) << "\"";
            if (l.width == 0) body << " vector-effect=\"non-scaling-stroke\"";
            if (opacity < 1) body << " stroke-opacity=\"" << num(opacity) << "\"";
            body << " d=\"" << pathData(p.contours, false) << "\"/>\n";
        }
    }
    std::ostringstream svg;
    const double w = std::max(1, r.xmax - r.xmin) / 20.0, h = std::max(1, r.ymax - r.ymin) / 20.0;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"" << px(r.xmin) << " " << px(r.ymin) << " "
        << num(w) << " " << num(h) << "\" width=\"" << num(w) << "\" height=\"" << num(h) << "\">\n";
    const auto d = defs.str();
    if (!d.empty()) svg << "<defs>\n" << d << "</defs>\n";
    svg << body.str() << "</svg>\n";
    return svg.str();
}

// ------------------------------------------------------------------ fonts and static text

namespace {

Shape readGlyph(const SWFDocument& doc, std::size_t pos, std::size_t end) {
    Shape glyph;
    glyph.version = 1;
    BitReader b(doc.data.data(), end, pos);
    ShapeGroup group;
    group.fills.push_back(FillStyle{}); // glyphs have one implicit fill style
    readShapeRecords(b, glyph, std::move(group));
    return glyph;
}

} // namespace

Font parseFont(const SWFDocument& doc, const TagRecord& tag) {
    Font font;
    const std::size_t end = tag.offset + tag.length;
    const auto* d = doc.data.data();
    auto u16at = [&](std::size_t p) -> std::uint16_t {
        if (p + 2 > end) throw std::runtime_error("font data truncated");
        return static_cast<std::uint16_t>(d[p] | (d[p + 1] << 8));
    };
    auto u32at = [&](std::size_t p) -> std::uint32_t { return u16at(p) | (static_cast<std::uint32_t>(u16at(p + 2)) << 16); };
    std::size_t pos = tag.offset;
    font.id = u16at(pos);
    pos += 2;

    if (tag.code == 10) { // DefineFont: offset table followed by glyph shapes
        const std::size_t table = pos;
        const std::size_t count = u16at(table) / 2;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t start = table + u16at(table + i * 2);
            const std::size_t stop = i + 1 < count ? table + u16at(table + (i + 1) * 2) : end;
            font.glyphs.push_back(readGlyph(doc, start, std::min(stop, end)));
        }
        return font;
    }
    if (tag.code != 48 && tag.code != 75) throw std::runtime_error("unsupported font tag");
    font.emSquare = tag.code == 75 ? 20480.0 : 1024.0;
    if (pos + 3 > end) throw std::runtime_error("font header truncated");
    const auto flags = d[pos];
    pos += 2; // flags, language
    font.hasLayout = (flags & 0x80) != 0;
    const bool wideOffsets = (flags & 0x08) != 0, wideCodes = (flags & 0x04) != 0;
    font.italic = (flags & 0x02) != 0;
    font.bold = (flags & 0x01) != 0;
    const std::size_t nameLen = d[pos++];
    font.name.assign(reinterpret_cast<const char*>(d + pos), std::min(nameLen, end - pos));
    pos += nameLen;
    const std::size_t count = u16at(pos);
    pos += 2;
    if (count == 0) return font; // device font: no outlines (the code table offset may be omitted)
    const std::size_t table = pos;
    const std::size_t entry = wideOffsets ? 4 : 2;
    auto offsetAt = [&](std::size_t i) -> std::size_t { return wideOffsets ? u32at(table + i * entry) : u16at(table + i * entry); };
    const std::size_t codeTable = table + (count == 0 ? (wideOffsets ? u32at(table) : u16at(table)) : offsetAt(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t start = table + offsetAt(i);
        const std::size_t stop = i + 1 < count ? table + offsetAt(i + 1) : codeTable;
        font.glyphs.push_back(readGlyph(doc, start, std::min(stop, end)));
    }
    pos = codeTable;
    for (std::size_t i = 0; i < count; ++i) {
        font.codes.push_back(wideCodes ? u16at(pos) : d[pos]);
        pos += wideCodes ? 2 : 1;
    }
    if (font.hasLayout && pos + 6 <= end) {
        font.ascent = static_cast<std::int16_t>(u16at(pos));
        font.descent = static_cast<std::int16_t>(u16at(pos + 2));
        font.leading = static_cast<std::int16_t>(u16at(pos + 4));
        pos += 6;
        for (std::size_t i = 0; i < count && pos + 2 <= end; ++i, pos += 2) font.advances.push_back(static_cast<std::int16_t>(u16at(pos)));
    }
    return font;
}

EditText parseEditText(const SWFDocument& doc, const TagRecord& tag) {
    const std::size_t end = tag.offset + tag.length;
    BitReader b(doc.data.data(), end, tag.offset);
    EditText t;
    t.id = b.u16();
    t.bounds = readRect(b);
    const auto f1 = b.u8(), f2 = b.u8();
    const bool hasText = f1 & 0x80, hasColor = f1 & 0x04, hasMaxLength = f1 & 0x02, hasFont = f1 & 0x01;
    t.wordWrap = f1 & 0x40;
    t.multiline = f1 & 0x20;
    t.password = f1 & 0x10;
    t.readOnly = f1 & 0x08;
    const bool hasFontClass = f2 & 0x80, hasLayout = f2 & 0x20;
    t.autoSize = f2 & 0x40;
    t.noSelect = f2 & 0x10;
    t.border = f2 & 0x08;
    t.html = f2 & 0x02;
    t.useOutlines = f2 & 0x01;
    auto cstr = [&] {
        std::string s;
        for (std::uint8_t c = b.u8(); c != 0; c = b.u8()) s.push_back(static_cast<char>(c));
        return s;
    };
    if (hasFont) t.fontId = b.u16();
    if (hasFontClass) (void)cstr();
    if (hasFont) t.height = b.u16();
    if (hasColor) { t.color.r = b.u8(); t.color.g = b.u8(); t.color.b = b.u8(); t.color.a = b.u8(); }
    if (hasMaxLength) t.maxLength = b.u16();
    if (hasLayout) {
        t.align = b.u8();
        t.leftMargin = b.u16();
        t.rightMargin = b.u16();
        t.indent = b.u16();
        t.leading = static_cast<std::int16_t>(b.u16());
    }
    t.variable = cstr();
    if (hasText) t.initialText = cstr();
    return t;
}

Shape parseText(const SWFDocument& doc, const TagRecord& tag, const std::map<std::uint16_t, Font>& fonts) {
    const std::size_t end = tag.offset + tag.length;
    BitReader b(doc.data.data(), end, tag.offset);
    Shape text;
    text.version = 3;
    text.id = b.u16();
    text.bounds = readRect(b);
    const Matrix m = readMatrix(b);
    const unsigned glyphBits = b.u8(), advanceBits = b.u8();
    const bool alpha = tag.code == 33;

    ShapeGroup group;
    std::map<std::uint32_t, std::size_t> styleForColor; // RGBA -> fill style / path index
    const Font* font = nullptr;
    RGBA color;
    double x = 0, y = 0, height = 0;

    auto transform = [&](const Point& p, double scale, double ox, double oy) {
        const double gx = ox + p.x * scale, gy = oy + p.y * scale;
        return Point{static_cast<std::int32_t>(std::lround(m.a * gx + m.c * gy + m.tx)),
                     static_cast<std::int32_t>(std::lround(m.b * gx + m.d * gy + m.ty))};
    };

    while (true) {
        const auto flags = b.u8();
        if (flags == 0) break;
        if (flags & 0x08) {
            const auto id = b.u16();
            const auto it = fonts.find(id);
            font = it == fonts.end() ? nullptr : &it->second;
        }
        if (flags & 0x04) {
            color.r = b.u8(); color.g = b.u8(); color.b = b.u8();
            color.a = alpha ? b.u8() : 255;
        }
        if (flags & 0x01) x = static_cast<std::int16_t>(b.u16());
        if (flags & 0x02) y = static_cast<std::int16_t>(b.u16());
        if (flags & 0x08) height = b.u16();
        const unsigned count = b.u8();
        const std::uint32_t key = (std::uint32_t(color.r) << 24) | (std::uint32_t(color.g) << 16) | (std::uint32_t(color.b) << 8) | color.a;
        auto style = styleForColor.find(key);
        if (style == styleForColor.end()) {
            FillStyle fs;
            fs.color = color;
            group.fills.push_back(fs);
            group.fillPaths.push_back({static_cast<std::uint32_t>(group.fills.size() - 1), {}});
            style = styleForColor.emplace(key, group.fillPaths.size() - 1).first;
        }
        auto& path = group.fillPaths[style->second];
        for (unsigned i = 0; i < count; ++i) {
            const auto index = b.bits(glyphBits);
            const auto advance = b.signedBits(advanceBits);
            if (font && index < font->glyphs.size() && height > 0) {
                const double scale = height / font->emSquare;
                for (const auto& g : font->glyphs[index].groups) {
                    for (const auto& fp : g.fillPaths) {
                        for (const auto& contour : fp.contours) {
                            std::vector<Edge> out;
                            out.reserve(contour.size());
                            for (const auto& e : contour) {
                                out.push_back({transform(e.from, scale, x, y), transform(e.control, scale, x, y),
                                               transform(e.to, scale, x, y), e.curved});
                            }
                            path.contours.push_back(std::move(out));
                        }
                    }
                }
            }
            x += advance;
        }
        b.align();
    }
    text.groups.push_back(std::move(group));
    return text;
}

// ---------------------------------------------------------------- morph shapes

namespace {

struct MorphFill {
    FillStyle start, end;
};

struct MorphLine {
    LineStyle start, end;
};

MorphFill readMorphFillStyle(BitReader& b) {
    MorphFill m;
    const auto type = b.u8();
    switch (type) {
        case 0x00:
            m.start.type = m.end.type = FillStyle::Type::Solid;
            m.start.color = readColor(b, true);
            m.end.color = readColor(b, true);
            break;
        case 0x10: case 0x12: case 0x13: {
            const auto t = type == 0x10 ? FillStyle::Type::Linear : type == 0x12 ? FillStyle::Type::Radial : FillStyle::Type::Focal;
            m.start.type = m.end.type = t;
            m.start.matrix = readMatrix(b);
            m.end.matrix = readMatrix(b);
            const auto flags = b.u8();
            m.start.spread = m.end.spread = (flags >> 6) & 3;
            m.start.interpolation = m.end.interpolation = (flags >> 4) & 3;
            for (int i = 0, n = flags & 0x0f; i < n; ++i) {
                GradientStop s, e;
                s.ratio = b.u8();
                s.color = readColor(b, true);
                e.ratio = b.u8();
                e.color = readColor(b, true);
                m.start.stops.push_back(s);
                m.end.stops.push_back(e);
            }
            if (type == 0x13) {
                m.start.focal = static_cast<std::int16_t>(b.u16()) / 256.0;
                m.end.focal = static_cast<std::int16_t>(b.u16()) / 256.0;
            }
            break;
        }
        case 0x40: case 0x41: case 0x42: case 0x43:
            m.start.type = m.end.type = FillStyle::Type::Bitmap;
            m.start.bitmapId = m.end.bitmapId = b.u16();
            m.start.matrix = readMatrix(b);
            m.end.matrix = readMatrix(b);
            m.start.repeat = m.end.repeat = type == 0x40 || type == 0x42;
            m.start.smooth = m.end.smooth = type == 0x40 || type == 0x41;
            break;
        default:
            throw std::runtime_error("unknown morph fill style type " + std::to_string(type));
    }
    return m;
}

// Shape records with absolute coordinates, as needed to pair start and end edges.
struct MorphRecord {
    bool edge = false;
    bool curved = false;
    Point control, to;           // edge
    bool hasMove = false;        // style change
    Point move;
    bool hasFill0 = false, hasFill1 = false, hasLine = false;
    std::uint32_t fill0{}, fill1{}, line{};
};

std::vector<MorphRecord> readMorphRecords(BitReader& b) {
    std::vector<MorphRecord> out;
    const unsigned fillBits = b.bits(4), lineBits = b.bits(4);
    Point pos;
    while (true) {
        MorphRecord r;
        if (b.flag()) {
            r.edge = true;
            const bool straight = b.flag();
            const unsigned n = b.bits(4) + 2;
            if (straight) {
                std::int32_t dx = 0, dy = 0;
                if (b.flag()) { dx = b.signedBits(n); dy = b.signedBits(n); }
                else if (b.flag()) dy = b.signedBits(n);
                else dx = b.signedBits(n);
                r.to = {pos.x + dx, pos.y + dy};
            } else {
                const auto cdx = b.signedBits(n), cdy = b.signedBits(n);
                const auto adx = b.signedBits(n), ady = b.signedBits(n);
                r.curved = true;
                r.control = {pos.x + cdx, pos.y + cdy};
                r.to = {r.control.x + adx, r.control.y + ady};
            }
            pos = r.to;
            out.push_back(r);
            continue;
        }
        const auto flags = b.bits(5);
        if (flags == 0) break;
        if (flags & 0x10) throw std::runtime_error("morph shape with NewStyles record");
        if (flags & 0x01) {
            const auto n = b.bits(5);
            r.hasMove = true;
            r.move.x = b.signedBits(n);
            r.move.y = b.signedBits(n);
            pos = r.move;
        }
        if (flags & 0x02) { r.hasFill0 = true; r.fill0 = b.bits(fillBits); }
        if (flags & 0x04) { r.hasFill1 = true; r.fill1 = b.bits(fillBits); }
        if (flags & 0x08) { r.hasLine = true; r.line = b.bits(lineBits); }
        out.push_back(r);
    }
    return out;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

std::int32_t lerpI(std::int32_t a, std::int32_t b, double t) {
    return static_cast<std::int32_t>(std::lround(lerp(a, b, t)));
}

Point lerpP(const Point& a, const Point& b, double t) { return {lerpI(a.x, b.x, t), lerpI(a.y, b.y, t)}; }

RGBA lerpC(const RGBA& a, const RGBA& b, double t) {
    auto ch = [&](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(lerp(x, y, t))); };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a)};
}

Matrix lerpM(const Matrix& a, const Matrix& b, double t) {
    return {lerp(a.a, b.a, t), lerp(a.b, b.b, t), lerp(a.c, b.c, t), lerp(a.d, b.d, t), lerp(a.tx, b.tx, t), lerp(a.ty, b.ty, t)};
}

FillStyle lerpFill(const FillStyle& s, const FillStyle& e, double t) {
    FillStyle f = s;
    f.color = lerpC(s.color, e.color, t);
    f.matrix = lerpM(s.matrix, e.matrix, t);
    f.focal = lerp(s.focal, e.focal, t);
    for (std::size_t i = 0; i < f.stops.size() && i < e.stops.size(); ++i) {
        f.stops[i].ratio = static_cast<std::uint8_t>(std::lround(lerp(s.stops[i].ratio, e.stops[i].ratio, t)));
        f.stops[i].color = lerpC(s.stops[i].color, e.stops[i].color, t);
    }
    return f;
}

} // namespace

Shape parseMorphShape(const SWFDocument& doc, const TagRecord& tag, std::uint16_t ratio) {
    if (tag.code != 46 && tag.code != 84) throw std::runtime_error("not a DefineMorphShape tag");
    const bool v2 = tag.code == 84;
    const double t = ratio / 65535.0;
    const std::size_t end = tag.offset + tag.length;
    BitReader b(doc.data.data(), end, tag.offset);
    Shape shape;
    shape.version = v2 ? 4 : 3;
    shape.id = b.u16();
    const Rect startBounds = readRect(b);
    b.align();
    const Rect endBounds = readRect(b);
    if (v2) {
        b.align();
        (void)readRect(b); // start edge bounds
        b.align();
        (void)readRect(b); // end edge bounds
        (void)b.u8();      // reserved + non-scaling/scaling stroke flags
    }
    const std::uint32_t offsetLo = b.u16(), offsetHi = b.u16();
    const std::size_t endEdgesPos = b.nextByte() + (offsetLo | (offsetHi << 16));
    shape.bounds = {lerpI(startBounds.xmin, endBounds.xmin, t), lerpI(startBounds.xmax, endBounds.xmax, t),
                    lerpI(startBounds.ymin, endBounds.ymin, t), lerpI(startBounds.ymax, endBounds.ymax, t)};

    ShapeGroup group;
    const auto nf = readCount(b, 2);
    for (std::uint16_t i = 0; i < nf; ++i) {
        const auto m = readMorphFillStyle(b);
        group.fills.push_back(lerpFill(m.start, m.end, t));
    }
    const auto nl = readCount(b, 2);
    for (std::uint16_t i = 0; i < nl; ++i) {
        LineStyle s, e;
        s.width = b.u16();
        e.width = b.u16();
        if (v2) {
            const auto f1 = b.u8();
            const auto f2 = b.u8();
            s.startCap = (f1 >> 6) & 3;
            s.join = (f1 >> 4) & 3;
            s.hasFill = (f1 & 0x08) != 0;
            s.noClose = (f2 & 0x04) != 0;
            s.endCap = f2 & 3;
            if (s.join == 2) s.miterLimit = b.u16() / 256.0;
            if (s.hasFill) {
                const auto m = readMorphFillStyle(b);
                s.fill = lerpFill(m.start, m.end, t);
                s.color = s.fill.type == FillStyle::Type::Solid ? s.fill.color
                          : !s.fill.stops.empty() ? s.fill.stops.front().color : RGBA{};
            } else {
                s.color = readColor(b, true);
                e.color = readColor(b, true);
                s.color = lerpC(s.color, e.color, t);
            }
        } else {
            s.color = readColor(b, true);
            e.color = readColor(b, true);
            s.color = lerpC(s.color, e.color, t);
        }
        s.width = static_cast<std::uint16_t>(std::lround(lerp(s.width, e.width, t)));
        group.lines.push_back(s);
    }

    b.align();
    const auto startRecs = readMorphRecords(b);
    if (endEdgesPos >= end) throw std::runtime_error("morph end edges outside tag");
    BitReader eb(doc.data.data(), end, endEdgesPos);
    const auto endRecs = readMorphRecords(eb);

    // Walk both record lists together. Style changes exist only in the start edges; the end
    // edges carry just their own moveTos. Straight/curved mismatches morph as quadratic curves.
    std::vector<std::vector<Edge>> fillEdges(group.fills.size() + 1), lineEdges(group.lines.size() + 1);
    std::uint32_t fill0 = 0, fill1 = 0, line = 0;
    Point sPos, ePos;
    std::size_t j = 0;
    for (std::size_t i = 0; i < startRecs.size();) {
        const auto& s = startRecs[i];
        const MorphRecord* e = j < endRecs.size() ? &endRecs[j] : nullptr;
        if (!s.edge) {
            if (s.hasMove) sPos = s.move;
            if (e && !e->edge) {
                if (e->hasMove) ePos = e->move;
                ++j;
            }
            if (s.hasFill0) fill0 = s.fill0;
            if (s.hasFill1) fill1 = s.fill1;
            if (s.hasLine) line = s.line;
            ++i;
            continue;
        }
        if (!e) break;
        if (!e->edge) { // an end-only moveTo
            if (e->hasMove) ePos = e->move;
            ++j;
            continue;
        }
        Edge out;
        out.from = lerpP(sPos, ePos, t);
        out.to = lerpP(s.to, e->to, t);
        out.curved = s.curved || e->curved;
        if (out.curved) {
            const Point sc = s.curved ? s.control : Point{(sPos.x + s.to.x) / 2, (sPos.y + s.to.y) / 2};
            const Point ec = e->curved ? e->control : Point{(ePos.x + e->to.x) / 2, (ePos.y + e->to.y) / 2};
            out.control = lerpP(sc, ec, t);
        }
        if (fill0 && fill0 < fillEdges.size()) fillEdges[fill0].push_back(out.reversed());
        if (fill1 && fill1 < fillEdges.size()) fillEdges[fill1].push_back(out);
        if (line && line < lineEdges.size()) lineEdges[line].push_back(out);
        sPos = s.to;
        ePos = e->to;
        ++i;
        ++j;
    }
    flushGroup(group, fillEdges, lineEdges);
    shape.groups.push_back(std::move(group));
    return shape;
}

} // namespace flashport

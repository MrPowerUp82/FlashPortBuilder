#pragma once
#include "flashport/SWFDocument.hpp"
#include "flashport/SWFStructures.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flashport {

struct GradientStop {
    std::uint8_t ratio{};
    RGBA color;
};

struct FillStyle {
    enum class Type : std::uint8_t { Solid, Linear, Radial, Focal, Bitmap };
    Type type = Type::Solid;
    RGBA color;
    Matrix matrix;                  // gradient/bitmap space -> shape space (twips)
    std::vector<GradientStop> stops;
    std::uint8_t spread{};          // 0 pad, 1 reflect, 2 repeat
    std::uint8_t interpolation{};   // 0 RGB, 1 linear RGB
    double focal{};
    std::uint16_t bitmapId{};
    bool repeat = true;
    bool smooth = true;
};

struct LineStyle {
    std::uint16_t width{};          // twips
    RGBA color;
    bool hasFill = false;
    FillStyle fill;
    std::uint8_t startCap{}, endCap{}, join{}; // 0 round, 1 none/bevel, 2 square/miter
    double miterLimit = 3.0;
    bool noClose = false;
};

struct Point { std::int32_t x{}, y{}; bool operator==(const Point& o) const { return x == o.x && y == o.y; } };

struct Edge {
    Point from;
    Point control;
    Point to;
    bool curved = false;
    Edge reversed() const { return {to, control, from, curved}; }
};

// A contour list for one style: closed contours for fills, open polylines for strokes.
struct StylePath {
    std::uint32_t style{};                 // index into the group's fill or line styles
    std::vector<std::vector<Edge>> contours;
};

// Styles are scoped: StateNewStyles records start a new group that replaces the arrays.
struct ShapeGroup {
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    std::vector<StylePath> fillPaths;      // drawn first, in style order
    std::vector<StylePath> linePaths;      // drawn over the fills
};

struct Shape {
    std::uint16_t id{};
    std::uint8_t version{};                // 1..4 (DefineShape .. DefineShape4)
    Rect bounds;
    bool evenOdd = true;                   // DefineShape4 can request the nonzero rule
    std::vector<ShapeGroup> groups;
};

// Parse DefineShape/2/3/4 and resolve the fill0/fill1 edge soup into closed contours.
Shape parseShape(const SWFDocument& doc, const TagRecord& tag);

// SHAPERECORDs after the style arrays already stored in `group` (DefineShape bodies, font glyphs).
void readShapeRecords(BitReader& b, Shape& shape, ShapeGroup group);

struct Font {
    std::uint16_t id{};
    std::string name;
    double emSquare = 1024;               // glyph units per em (20480 for DefineFont3)
    std::vector<Shape> glyphs;            // outlines, fill style 0
    std::vector<std::uint16_t> codes;     // character code per glyph (DefineFont2/3)
    std::vector<std::int16_t> advances;   // layout advances in glyph units (when present)
    std::int16_t ascent{}, descent{}, leading{};
    bool hasLayout = false, bold = false, italic = false;
};

// DefineFont / DefineFont2 / DefineFont3 glyph outlines and layout.
Font parseFont(const SWFDocument& doc, const TagRecord& tag);

struct EditText {
    std::uint16_t id{};
    Rect bounds;
    std::uint16_t fontId{};
    std::uint16_t height{}; // twips
    RGBA color{0, 0, 0, 255};
    std::uint8_t align{};   // 0 left, 1 right, 2 center, 3 justify
    std::uint16_t leftMargin{}, rightMargin{}, indent{};
    std::int16_t leading{};
    bool wordWrap = false, multiline = false, password = false, readOnly = true, html = false, border = false;
    bool useOutlines = false, autoSize = false, noSelect = false;
    std::uint16_t maxLength{};
    std::string variable;
    std::string initialText;
};

// DefineEditText (dynamic / input text field).
EditText parseEditText(const SWFDocument& doc, const TagRecord& tag);

// Static text (DefineText/DefineText2) laid out with its fonts' outlines, as one shape.
Shape parseText(const SWFDocument& doc, const TagRecord& tag, const std::map<std::uint16_t, Font>& fonts);

// DefineMorphShape/DefineMorphShape2 interpolated at `ratio` (0 = start shape, 65535 = end shape).
Shape parseMorphShape(const SWFDocument& doc, const TagRecord& tag, std::uint16_t ratio);

struct BitmapInfo {
    int width{};
    int height{};
    std::string file; // file name inside the bitmap directory
};

// SVG document for a shape. Bitmap fills reference `<bitmapDir>/<file>`.
std::string shapeToSVG(const Shape& shape, const std::string& bitmapDir,
                       const std::map<std::uint16_t, BitmapInfo>& bitmaps);

} // namespace flashport

#pragma once
#include "flashport/BitReader.hpp"
#include "flashport/SWFDocument.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flashport {

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1; // scaleX, rotateSkew0, rotateSkew1, scaleY
    double tx = 0, ty = 0;             // twips
};

struct ColorTransform {
    double rMul = 1, gMul = 1, bMul = 1, aMul = 1;
    std::int32_t rAdd = 0, gAdd = 0, bAdd = 0, aAdd = 0;
};

struct RGBA { std::uint8_t r{}, g{}, b{}, a = 255; };

Rect readRect(BitReader& bits);
Matrix readMatrix(BitReader& bits);
ColorTransform readColorTransform(BitReader& bits, bool withAlpha);

struct ClipAction {
    std::uint32_t events{};
    std::uint8_t keyCode{};
    std::size_t actionOffset{}; // absolute offset in SWFDocument::data
    std::size_t actionLength{};
};

struct PlaceObject {
    std::uint8_t version{}; // 1, 2 or 3
    std::uint16_t depth{};
    bool move = false;
    std::optional<std::uint16_t> characterId;
    std::optional<Matrix> matrix;
    std::optional<ColorTransform> colorTransform;
    std::optional<std::uint16_t> ratio;
    std::optional<std::string> name;
    std::optional<std::uint16_t> clipDepth;
    std::optional<std::string> className;
    std::optional<std::uint8_t> blendMode;
    std::optional<bool> cacheAsBitmap;
    std::optional<bool> visible;
    std::uint8_t filterCount{};
    std::vector<ClipAction> clipActions;
};

// Skip a FILTERLIST starting at `pos`; returns the position after it.
std::size_t skipFilterList(const SWFDocument& doc, std::size_t pos, std::size_t end, std::uint8_t& count);

PlaceObject parsePlaceObject(const SWFDocument& doc, const TagRecord& tag);

// A contiguous AVM1 action stream somewhere inside the SWF.
struct AVM1Source {
    std::string label;      // e.g. "frame 3 of sprite 12", "button 40 cond 0x0008"
    std::size_t offset{};   // absolute offset in SWFDocument::data
    std::size_t length{};
    std::uint16_t spriteId{};
    std::uint32_t frame{};
};

// Enumerate every AVM1 code location: DoAction, DoInitAction, DefineButton(2) actions
// and PlaceObject2/3 clip event handlers.
std::vector<AVM1Source> findAVM1Sources(const SWFDocument& doc);

// Human readable clip-event mask ("load|enterFrame").
std::string clipEventNames(std::uint32_t events);

} // namespace flashport

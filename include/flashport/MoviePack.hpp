#pragma once
#include "flashport/SWFDocument.hpp"
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace flashport {

struct MoviePackReport {
    std::uint64_t bitmaps{}, gradientTextures{};
    std::uint64_t shapes{}, meshes{}, triangles{}, texts{}, editTexts{}, morphShapes{}, morphFrames{}, sounds{};
    std::uint64_t timelines{}, frames{}, placeCommands{};
    std::uint64_t buttons{};
    std::uint64_t frameScripts{}, frameActions{};
    std::uint64_t actionBlocks{}; // AVM1 bytecode blocks stored for the interpreter
    std::uint64_t abcBlocks{};    // AVM2 DoABC blocks stored for the interpreter
    std::uint64_t packBytes{};
    struct HeavyShape { std::uint16_t id{}; std::uint64_t triangles{}; double ms{}; };
    std::vector<HeavyShape> heaviest; // slowest shapes to tessellate, for diagnostics
    std::vector<std::string> problems;
    void problem(const std::string& p) { if (problems.size() < 40) problems.push_back(p); }
};

// Writes the binary movie pack consumed by the FlashPort runtime (format documented in
// runtime/FlashRuntime.hpp): bitmaps, gradient textures, tessellated shapes, timelines
// with per-frame display-list commands, labels, recognised frame actions, buttons, symbols.
void writeMoviePack(const SWFDocument& doc, const std::string& path, MoviePackReport& report);

void printMoviePackReport(const MoviePackReport& r, std::ostream& out);

} // namespace flashport

#pragma once
#include "flashport/SWFDocument.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace flashport {

// Timeline control calls a frame script makes on its own clip. This is not general
// script execution: it recognises literal stop()/play()/goto*() calls in the
// unconditional first basic block, which is enough to drive most timelines.
struct FrameAction {
    enum class Kind : std::uint8_t { Stop = 1, Play, GotoAndStop, GotoAndPlay, NextFrame, PrevFrame };
    Kind kind{};
    std::int32_t frame = -1; // 0-based; -1 when a label is used
    std::string label;
};

struct FrameScriptInfo {
    std::map<std::pair<std::uint16_t, std::uint32_t>, std::vector<FrameAction>> actions; // (sprite id, frame)
    std::uint64_t avm1Scripts{}, avm2Scripts{};   // frame scripts seen
    std::uint64_t recognisedActions{};
};

FrameScriptInfo extractFrameScripts(const SWFDocument& doc);

} // namespace flashport

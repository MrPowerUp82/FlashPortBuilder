#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace flashport {

struct Rect {
    std::int32_t xmin{};
    std::int32_t xmax{};
    std::int32_t ymin{};
    std::int32_t ymax{};
};

struct ABCSummary {
    std::string name;
    std::uint32_t methods{};
    std::uint32_t classes{};
    std::uint32_t scripts{};
    std::uint32_t methodBodies{};
    std::uint64_t bytecodeBytes{};
};

struct SWFSummary;

struct BinaryDataSummary {
    std::uint16_t characterId{};
    std::size_t payloadBytes{};
    std::string magic;
    std::shared_ptr<SWFSummary> embeddedSwf;
};

struct SWFSummary {
    std::string sourceName;
    std::string signature;
    std::uint8_t version{};
    std::uint32_t declaredFileLength{};
    std::size_t actualFileLength{};
    Rect frameRect;
    double fps{};
    std::uint16_t frameCount{};
    bool actionScript3{};
    bool hasAVM1Actions{};
    std::string documentClass;
    std::map<std::uint16_t, std::uint64_t> tagCounts;
    std::uint64_t totalTags{};
    std::vector<ABCSummary> abcBlocks;
    std::vector<BinaryDataSummary> binaryData;
};

class SWFReader {
public:
    SWFSummary analyzeFile(const std::string& path) const;
    SWFSummary analyzeBytes(const std::vector<std::uint8_t>& bytes,
                            const std::string& sourceName = "<memory>") const;
};

std::string tagName(std::uint16_t code);
std::string actionScriptKind(const SWFSummary& summary);

} // namespace flashport

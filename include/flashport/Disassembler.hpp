#pragma once
#include "flashport/SWFDocument.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flashport {

struct DisasmReport {
    // AVM2
    std::uint64_t abcBlocks{};
    std::uint64_t avm2Classes{};
    std::uint64_t avm2Bodies{};
    std::uint64_t avm2Instructions{};
    std::uint64_t avm2Blocks{};
    std::uint64_t avm2BodiesWithErrors{};
    std::uint64_t avm2BodiesWithWarnings{};
    std::uint64_t avm2UnreachableBytes{};
    std::map<std::string, std::uint64_t> avm2OpcodeUse;
    // AVM1
    std::uint64_t avm1Sources{};
    std::uint64_t avm1Functions{};
    std::uint64_t avm1Actions{};
    std::uint64_t avm1Blocks{};
    std::uint64_t avm1SourcesWithErrors{};
    std::uint64_t avm1UnknownActions{};
    std::map<std::string, std::uint64_t> avm1ActionUse;

    std::vector<std::string> problems; // first N problems for the summary
    void problem(const std::string& p) { if (problems.size() < 40) problems.push_back(p); }
};

// Decode every AVM2 method body and AVM1 action stream in `doc`. When `outDir` is not
// empty, per-class (.abc.txt) and per-source (.avm1.txt) listings are written there.
void disassembleSWF(const SWFDocument& doc, const std::string& outDir, DisasmReport& report);

void printDisasmReport(const DisasmReport& r, std::ostream& out);

} // namespace flashport

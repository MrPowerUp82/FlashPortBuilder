#pragma once
#include "flashport/ABCFile.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flashport {

enum class AVM2Operands : std::uint8_t {
    None,
    U30,      // one u30
    U30U30,   // two u30
    U8,       // one byte (pushbyte, getscopeobject)
    S24,      // branch offset
    Switch,   // lookupswitch
    Debug,    // debug: u8 u30 u8 u30
};

struct AVM2OpInfo {
    const char* name = nullptr; // nullptr = invalid opcode
    AVM2Operands operands = AVM2Operands::None;
};

const AVM2OpInfo& avm2OpInfo(std::uint8_t op);

struct AVM2Instruction {
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint8_t op{};
    std::vector<std::int32_t> operands;  // raw operand values (branch operands are relative)
    std::vector<std::uint32_t> targets;  // absolute branch/switch targets
};

struct AVM2Block {
    std::uint32_t start{};
    std::uint32_t end{};                 // exclusive byte offset
    std::uint32_t firstInstr{};          // index into AVM2Method::instructions
    std::uint32_t instrCount{};
    std::vector<std::uint32_t> successors; // block start offsets
    bool handler = false;                // exception handler entry
    std::int32_t stackIn = -1;           // operand stack depth on entry
    std::int32_t scopeIn = -1;           // scope depth on entry (relative to init_scope_depth)
};

struct AVM2Method {
    std::uint32_t bodyIndex{};
    std::vector<AVM2Instruction> instructions; // sorted by offset
    std::vector<AVM2Block> blocks;             // sorted by start
    std::uint32_t unreachableBytes{};
    std::int32_t maxStackSeen{};
    std::vector<std::string> errors;           // decoding problems (invalid opcode, bad target...)
    std::vector<std::string> warnings;         // verifier findings (stack mismatch, ...)
};

// Decode one method body by reachability from offset 0 and all exception handlers,
// build basic blocks and run a stack/scope depth verifier.
AVM2Method decodeAVM2Body(const ABCFile& abc, std::uint32_t bodyIndex);

// Pretty-print a single instruction with constant-pool operands resolved.
std::string formatAVM2Instruction(const ABCFile& abc, const AVM2Instruction& ins);

struct AVM2Stats {
    std::uint64_t bodies{};
    std::uint64_t instructions{};
    std::uint64_t blocks{};
    std::uint64_t bodiesWithErrors{};
    std::uint64_t bodiesWithWarnings{};
    std::uint64_t unreachableBytes{};
    std::map<std::uint8_t, std::uint64_t> opcodeCounts;
};

} // namespace flashport

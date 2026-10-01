#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace flashport {

struct AVM1PushValue {
    enum class Type : std::uint8_t { String, Float, Null, Undefined, Register, Boolean, Double, Integer, Constant };
    Type type{};
    std::string str;
    double num{};
    std::uint32_t index{}; // register or constant pool index
};

struct AVM1Action {
    std::uint32_t offset{};  // offset inside the decoded byte range
    std::uint8_t code{};
    std::uint16_t length{};  // payload length (0 for short actions)
    std::vector<std::string> strings;   // ConstantPool entries, URL/target/label, function name + params
    std::vector<std::int32_t> ints;     // numeric operands (frame, register, flags, sizes...)
    std::vector<AVM1PushValue> push;
    std::vector<std::uint32_t> targets; // branch / try-block targets (offsets)
    std::int32_t function = -1;         // DefineFunction(2): index into AVM1Program::functions
};

struct AVM1Block {
    std::uint32_t start{};
    std::uint32_t end{};
    std::uint32_t firstAction{};
    std::uint32_t actionCount{};
    std::vector<std::uint32_t> successors;
};

struct AVM1Function {
    std::string name;             // empty for anonymous / top level
    std::vector<std::string> params;
    std::uint32_t start{}, end{}; // byte range of the body
    bool isV2 = false;
    std::uint8_t registerCount{};
    std::uint16_t flags{};
    std::vector<AVM1Action> actions;
    std::vector<AVM1Block> blocks;
};

struct AVM1Program {
    std::vector<AVM1Function> functions; // [0] = top-level code
    std::vector<std::string> errors;
};

// Decode an AVM1 action stream. `swfVersion` selects UTF-8 (>=6) vs Latin-1 strings.
AVM1Program decodeAVM1(const std::uint8_t* data, std::size_t size, std::uint8_t swfVersion);

const char* avm1ActionName(std::uint8_t code);
std::string formatAVM1Action(const AVM1Action& a, const std::vector<std::string>* constants);

} // namespace flashport

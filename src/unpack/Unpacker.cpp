#include "flashport/Unpacker.hpp"
#include "flashport/ABCFile.hpp"
#include "flashport/AVM2Code.hpp"
#include "flashport/ByteReader.hpp"
#include <map>
#include <regex>
#include <sstream>

namespace flashport {
namespace {

bool validSWF(const std::vector<std::uint8_t>& bytes) {
    if (!looksLikeSWF(bytes) || bytes[0] == 'Z') return false;
    try {
        const auto doc = loadSWFDocument(bytes, "<candidate>");
        return !doc.tags.empty() && doc.tags.back().code == 0 && doc.frameRect.xmax > doc.frameRect.xmin;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<ABCFile> parseAllABC(const SWFDocument& doc) {
    std::vector<ABCFile> out;
    for (const auto& t : doc.tags) {
        if (t.code != 82) continue;
        try {
            ByteReader p(doc.data, t.offset);
            (void)p.u32();
            const auto name = p.cstring();
            out.push_back(parseABC(p.bytes(t.offset + t.length - p.pos()), name));
        } catch (const std::exception&) {
        }
    }
    return out;
}

// Alchemy compiles C locals to instance slots i0..iN on an FSM class and passes
// call arguments through the emulated stack: `getproperty iK; ...esp; pushbyte OFF; add; si32`.
// Recover integer arrays passed to a call (e.g. AS3_Array("IntType,...", k0, k1, ...)).
std::vector<std::vector<std::int32_t>> alchemyConstantArrays(const ABCFile& abc) {
    static const std::regex regName("^i([0-9]+)$");
    std::vector<std::vector<std::int32_t>> keys;
    for (std::size_t bi = 0; bi < abc.bodies.size(); ++bi) {
        const auto m = decodeAVM2Body(abc, static_cast<std::uint32_t>(bi));
        std::map<int, std::int32_t> regs;
        std::map<int, std::int32_t> args; // stack offset -> value
        auto regOf = [&](const AVM2Instruction& ins) -> int {
            if (ins.operands.empty()) return -1;
            std::smatch sm;
            const auto name = abc.multinameLocal(static_cast<std::uint32_t>(ins.operands[0]));
            return std::regex_match(name, sm, regName) ? std::stoi(sm[1]) : -1;
        };
        int pending = -1;
        for (std::size_t k = 0; k < m.instructions.size(); ++k) {
            const auto& ins = m.instructions[k];
            if (k + 1 < m.instructions.size() && m.instructions[k + 1].op == 0x68) { // -> initproperty
                std::optional<std::int32_t> v;
                if (ins.op == 0x24) v = static_cast<std::int8_t>(ins.operands[0]);
                else if (ins.op == 0x25) v = static_cast<std::int16_t>(ins.operands[0]);
                else if (ins.op == 0x2d && static_cast<std::size_t>(ins.operands[0]) < abc.ints.size()) v = abc.ints[static_cast<std::size_t>(ins.operands[0])];
                const int r = regOf(m.instructions[k + 1]);
                if (v && r >= 0) regs[r] = *v;
            }
            if (ins.op == 0x66) { // getproperty
                const int r = regOf(ins);
                if (r >= 0) pending = r;
            }
            if (ins.op == 0x3c && pending >= 0 && k >= 2 && m.instructions[k - 1].op == 0xa0 &&
                m.instructions[k - 2].op == 0x24 && regs.count(pending)) { // pushbyte OFF; add; si32
                args[static_cast<std::int8_t>(m.instructions[k - 2].operands[0])] = regs[pending];
                pending = -1;
            } else if (ins.op == 0x3c) {
                pending = -1;
            }
        }
        if (args.size() >= 4) {
            std::vector<std::int32_t> key;
            for (const auto& [off, v] : args) key.push_back(v);
            keys.push_back(std::move(key));
        }
    }
    return keys;
}

// Reproduces the AS3 loop: out.writeByte(in.readByte() ^ key[j]); j = (j >= key.length) ? 0 : j + 1.
// key[key.length] is `undefined`, which ToInt32 converts to 0.
std::vector<std::uint8_t> xorCycle(const std::vector<std::uint8_t>& in, const std::vector<std::int32_t>& key,
                                   std::size_t limit) {
    std::vector<std::uint8_t> out(std::min(limit, in.size()));
    std::size_t j = 0;
    const std::size_t n = key.size();
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::int32_t k = j < n ? key[j] : 0;
        out[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(in[i]) ^ k);
        j = j >= n ? 0 : j + 1;
    }
    return out;
}

std::string describeKey(const std::vector<std::int32_t>& key) {
    std::ostringstream o;
    for (std::size_t i = 0; i < key.size(); ++i) o << (i ? "," : "") << key[i];
    return o.str();
}

} // namespace

std::optional<UnpackResult> unpackLoader(const SWFDocument& doc) {
    const auto bins = binaryDataPayloads(doc, false);
    for (const auto& b : bins) {
        if (validSWF(b.bytes)) {
            return UnpackResult{"plain SWF in DefineBinaryData #" + std::to_string(b.characterId), b.bytes};
        }
    }
    if (bins.empty()) return std::nullopt;

    for (const auto& abc : parseAllABC(doc)) {
        for (const auto& key : alchemyConstantArrays(abc)) {
            for (const auto& a : bins) {
                // Cheap signature check on the first bytes before building the full candidate.
                const auto head = xorCycle(a.bytes, key, 8);
                if (!looksLikeSWF(head)) continue;
                std::vector<const EmbeddedBinary*> tails{nullptr};
                for (const auto& b : bins) if (&b != &a) tails.push_back(&b);
                for (const auto* tail : tails) {
                    auto candidate = xorCycle(a.bytes, key, a.bytes.size());
                    if (tail) candidate.insert(candidate.end(), tail->bytes.begin(), tail->bytes.end());
                    if (!validSWF(candidate)) continue;
                    std::string method = "Alchemy XOR loader: BinaryData #" + std::to_string(a.characterId) +
                                         " xor [" + describeKey(key) + "]";
                    if (tail) method += " ++ BinaryData #" + std::to_string(tail->characterId);
                    return UnpackResult{method, std::move(candidate)};
                }
            }
        }
    }

    // Simple repeating-XOR loaders (`bytes[i] ^= key[i % n]` before loadBytes). A key of up
    // to 3 bytes is fully determined by the "FWS"/"CWS"/"ZWS" signature it must produce.
    for (const auto& a : bins) {
        if (a.bytes.size() < 8) continue;
        for (const char* sig : {"FWS", "CWS", "ZWS"}) {
            for (std::size_t len = 1; len <= 3; ++len) {
                std::vector<std::uint8_t> key(len);
                for (std::size_t i = 0; i < len; ++i) key[i] = a.bytes[i] ^ static_cast<std::uint8_t>(sig[i]);
                bool consistent = true;
                for (std::size_t i = len; i < 3; ++i)
                    consistent = consistent && (a.bytes[i] ^ key[i % len]) == static_cast<std::uint8_t>(sig[i]);
                if (!consistent) continue;
                auto candidate = a.bytes;
                for (std::size_t i = 0; i < candidate.size(); ++i) candidate[i] ^= key[i % len];
                if (!validSWF(candidate)) continue;
                std::ostringstream method;
                method << "XOR loader: BinaryData #" << a.characterId << " xor [";
                for (std::size_t i = 0; i < len; ++i) method << (i ? "," : "") << int(key[i]);
                method << "]";
                return UnpackResult{method.str(), std::move(candidate)};
            }
        }
    }
    return std::nullopt;
}

} // namespace flashport

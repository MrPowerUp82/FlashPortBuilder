#include "flashport/AVM1Code.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flashport {
namespace {

class Cursor {
public:
    Cursor(const std::uint8_t* d, std::size_t begin, std::size_t end, bool utf8)
        : d_(d), pos_(begin), end_(end), utf8_(utf8) {}
    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return end_ - pos_; }
    std::uint8_t u8() { need(1); return d_[pos_++]; }
    std::uint16_t u16() { need(2); std::uint16_t v = static_cast<std::uint16_t>(d_[pos_] | (d_[pos_ + 1] << 8)); pos_ += 2; return v; }
    std::int16_t s16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = static_cast<std::uint32_t>(d_[pos_]) | (static_cast<std::uint32_t>(d_[pos_ + 1]) << 8) |
                          (static_cast<std::uint32_t>(d_[pos_ + 2]) << 16) | (static_cast<std::uint32_t>(d_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    float f32() { const auto raw = u32(); float f; std::memcpy(&f, &raw, 4); return f; }
    // AVM1 doubles store the high 32-bit word first.
    double f64() {
        const std::uint64_t hi = u32(), lo = u32();
        const std::uint64_t raw = (hi << 32) | lo;
        double d; std::memcpy(&d, &raw, 8); return d;
    }
    std::string str() {
        std::string s;
        while (true) {
            const auto c = u8();
            if (c == 0) break;
            if (!utf8_ && c >= 0x80) { // Latin-1 -> UTF-8 for SWF <= 5
                s.push_back(static_cast<char>(0xc0 | (c >> 6)));
                s.push_back(static_cast<char>(0x80 | (c & 0x3f)));
            } else {
                s.push_back(static_cast<char>(c));
            }
        }
        return s;
    }
private:
    void need(std::size_t n) const { if (pos_ + n > end_) throw std::runtime_error("action payload truncated"); }
    const std::uint8_t* d_;
    std::size_t pos_, end_;
    bool utf8_;
};

bool isTerminator(std::uint8_t c) { return c == 0x00 || c == 0x99 || c == 0x3e || c == 0x2a; }

class Decoder {
public:
    Decoder(const std::uint8_t* d, std::size_t size, std::uint8_t version)
        : d_(d), size_(size), utf8_(version >= 6) {}

    AVM1Program run() {
        prog_.functions.emplace_back();
        prog_.functions[0].start = 0;
        prog_.functions[0].end = static_cast<std::uint32_t>(size_);
        decodeFunction(0, 0);
        return std::move(prog_);
    }

private:
    void decodeFunction(std::size_t fi, unsigned depth) {
        if (depth > 64) { prog_.errors.push_back("function nesting too deep"); return; }
        const auto start = prog_.functions[fi].start;
        const auto end = prog_.functions[fi].end;
        std::vector<AVM1Action> actions;
        std::size_t pc = start;
        while (pc < end) {
            AVM1Action a;
            a.offset = static_cast<std::uint32_t>(pc);
            a.code = d_[pc];
            std::size_t payload = pc + 1;
            if (a.code >= 0x80) {
                if (pc + 3 > end) { prog_.errors.push_back("truncated action header at " + std::to_string(pc)); break; }
                a.length = static_cast<std::uint16_t>(d_[pc + 1] | (d_[pc + 2] << 8));
                payload = pc + 3;
                if (payload + a.length > end) {
                    prog_.errors.push_back("action 0x" + hex(a.code) + " at " + std::to_string(pc) + " overruns its block");
                    break;
                }
            }
            std::size_t next = payload + a.length;
            try {
                decodePayload(a, payload, next, depth, next);
            } catch (const std::exception& e) {
                prog_.errors.push_back(std::string("action 0x") + hex(a.code) + " at " + std::to_string(pc) + ": " + e.what());
            }
            const bool stop = a.code == 0x00 && fi == 0;
            actions.push_back(std::move(a));
            if (stop) break;
            pc = next;
        }
        prog_.functions[fi].actions = std::move(actions);
        buildBlocks(prog_.functions[fi]);
    }

    // `next` may be advanced past inline function bodies.
    void decodePayload(AVM1Action& a, std::size_t p, std::size_t end, unsigned depth, std::size_t& next) {
        Cursor c(d_, p, end, utf8_);
        switch (a.code) {
            case 0x81: a.ints.push_back(c.u16()); break;                            // GotoFrame
            case 0x83: a.strings.push_back(c.str()); a.strings.push_back(c.str()); break; // GetURL
            case 0x87: a.ints.push_back(c.u8()); break;                             // StoreRegister
            case 0x88: {                                                            // ConstantPool
                const auto n = c.u16();
                for (std::uint16_t i = 0; i < n && c.remaining() > 0; ++i) a.strings.push_back(c.str());
                break;
            }
            case 0x8a: a.ints.push_back(c.u16()); a.ints.push_back(c.u8()); break;   // WaitForFrame
            case 0x8b: case 0x8c: a.strings.push_back(c.str()); break;              // SetTarget / GoToLabel
            case 0x8d: a.ints.push_back(c.u8()); break;                             // WaitForFrame2
            case 0x9a: a.ints.push_back(c.u8()); break;                             // GetURL2
            case 0x9f: {                                                            // GotoFrame2
                const auto flags = c.u8();
                a.ints.push_back(flags);
                if (flags & 0x02) a.ints.push_back(c.u16());
                break;
            }
            case 0x99: case 0x9d: {                                                 // Jump / If
                const auto off = c.s16();
                a.ints.push_back(off);
                a.targets.push_back(static_cast<std::uint32_t>(static_cast<std::int64_t>(end) + off));
                break;
            }
            case 0x94: a.ints.push_back(c.u16()); break;                            // With (body is inline)
            case 0x96: decodePush(a, c); break;
            case 0x8f: {                                                            // Try
                const auto flags = c.u8();
                const auto trySize = c.u16(), catchSize = c.u16(), finallySize = c.u16();
                a.ints = {flags, trySize, catchSize, finallySize};
                if (flags & 0x04) a.ints.push_back(c.u8()); else a.strings.push_back(c.str());
                const auto base = static_cast<std::uint32_t>(end);
                if (flags & 0x01) a.targets.push_back(base + trySize);
                if (flags & 0x02) a.targets.push_back(base + trySize + catchSize);
                a.targets.push_back(base + trySize + catchSize + finallySize);
                break;
            }
            case 0x9b: case 0x8e: {                                                 // DefineFunction(2)
                AVM1Function f;
                f.isV2 = a.code == 0x8e;
                f.name = c.str();
                const auto numParams = c.u16();
                if (f.isV2) {
                    f.registerCount = c.u8();
                    f.flags = c.u16();
                    for (std::uint16_t i = 0; i < numParams; ++i) {
                        const auto reg = c.u8();
                        auto name = c.str();
                        f.params.push_back(reg ? "r" + std::to_string(reg) + ":" + name : name);
                    }
                } else {
                    for (std::uint16_t i = 0; i < numParams; ++i) f.params.push_back(c.str());
                }
                const auto codeSize = c.u16();
                a.strings.push_back(f.name);
                for (const auto& p : f.params) a.strings.push_back(p);
                a.ints.push_back(codeSize);
                f.start = static_cast<std::uint32_t>(end);
                f.end = static_cast<std::uint32_t>(std::min<std::size_t>(end + codeSize, size_));
                if (end + codeSize > size_) throw std::runtime_error("function body exceeds action block");
                const auto fi = prog_.functions.size();
                a.function = static_cast<std::int32_t>(fi);
                prog_.functions.push_back(std::move(f));
                decodeFunction(fi, depth + 1);
                next = end + codeSize;
                break;
            }
            default:
                break; // short actions and unknown long actions carry no decoded operands
        }
    }

    void decodePush(AVM1Action& a, Cursor& c) {
        using T = AVM1PushValue::Type;
        while (c.remaining() > 0) {
            AVM1PushValue v;
            const auto type = c.u8();
            switch (type) {
                case 0: v.type = T::String; v.str = c.str(); break;
                case 1: v.type = T::Float; v.num = c.f32(); break;
                case 2: v.type = T::Null; break;
                case 3: v.type = T::Undefined; break;
                case 4: v.type = T::Register; v.index = c.u8(); break;
                case 5: v.type = T::Boolean; v.num = c.u8() ? 1 : 0; break;
                case 6: v.type = T::Double; v.num = c.f64(); break;
                case 7: v.type = T::Integer; v.num = static_cast<std::int32_t>(c.u32()); break;
                case 8: v.type = T::Constant; v.index = c.u8(); break;
                case 9: v.type = T::Constant; v.index = c.u16(); break;
                default: throw std::runtime_error("unknown push type " + std::to_string(type));
            }
            a.push.push_back(std::move(v));
        }
    }

    void buildBlocks(AVM1Function& f) {
        std::set<std::uint32_t> boundaries;
        for (const auto& a : f.actions) boundaries.insert(a.offset);
        std::set<std::uint32_t> leaders;
        if (!f.actions.empty()) leaders.insert(f.actions.front().offset);
        for (std::size_t i = 0; i < f.actions.size(); ++i) {
            const auto& a = f.actions[i];
            for (auto t : a.targets) {
                if (t == f.end) continue; // jump to function end = exit
                if (t < f.start || t > f.end || !boundaries.count(t)) {
                    prog_.errors.push_back("action 0x" + hex(a.code) + " at " + std::to_string(a.offset) +
                                           " targets " + std::to_string(t) + " which is not an action boundary");
                    continue;
                }
                leaders.insert(t);
            }
            if ((isTerminator(a.code) || a.code == 0x9d || a.code == 0x8f) && i + 1 < f.actions.size()) {
                leaders.insert(f.actions[i + 1].offset);
            }
        }
        for (std::size_t i = 0; i < f.actions.size();) {
            AVM1Block b;
            b.start = f.actions[i].offset;
            b.firstAction = static_cast<std::uint32_t>(i);
            std::size_t j = i + 1;
            while (j < f.actions.size() && !leaders.count(f.actions[j].offset)) ++j;
            const auto& last = f.actions[j - 1];
            b.end = j < f.actions.size() ? f.actions[j].offset : f.end;
            b.actionCount = static_cast<std::uint32_t>(j - i);
            for (auto t : last.targets) if (boundaries.count(t)) b.successors.push_back(t);
            if (!isTerminator(last.code) && j < f.actions.size()) b.successors.push_back(f.actions[j].offset);
            std::sort(b.successors.begin(), b.successors.end());
            b.successors.erase(std::unique(b.successors.begin(), b.successors.end()), b.successors.end());
            f.blocks.push_back(std::move(b));
            i = j;
        }
    }

    static std::string hex(std::uint8_t v) {
        std::ostringstream o; o << std::hex << std::setw(2) << std::setfill('0') << int(v); return o.str();
    }

    const std::uint8_t* d_;
    std::size_t size_;
    bool utf8_;
    AVM1Program prog_;
};

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(ch)); }
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\x%02x", ch); out += buf; }
        else out.push_back(static_cast<char>(ch));
    }
    return out + "\"";
}

} // namespace

AVM1Program decodeAVM1(const std::uint8_t* data, std::size_t size, std::uint8_t swfVersion) {
    return Decoder(data, size, swfVersion).run();
}

const char* avm1ActionName(std::uint8_t code) {
    switch (code) {
        case 0x00: return "End"; case 0x04: return "NextFrame"; case 0x05: return "PrevFrame";
        case 0x06: return "Play"; case 0x07: return "Stop"; case 0x08: return "ToggleQuality";
        case 0x09: return "StopSounds"; case 0x0a: return "Add"; case 0x0b: return "Subtract";
        case 0x0c: return "Multiply"; case 0x0d: return "Divide"; case 0x0e: return "Equals";
        case 0x0f: return "Less"; case 0x10: return "And"; case 0x11: return "Or"; case 0x12: return "Not";
        case 0x13: return "StringEquals"; case 0x14: return "StringLength"; case 0x15: return "StringExtract";
        case 0x17: return "Pop"; case 0x18: return "ToInteger"; case 0x1c: return "GetVariable";
        case 0x1d: return "SetVariable"; case 0x20: return "SetTarget2"; case 0x21: return "StringAdd";
        case 0x22: return "GetProperty"; case 0x23: return "SetProperty"; case 0x24: return "CloneSprite";
        case 0x25: return "RemoveSprite"; case 0x26: return "Trace"; case 0x27: return "StartDrag";
        case 0x28: return "EndDrag"; case 0x29: return "StringLess"; case 0x2a: return "Throw";
        case 0x2b: return "CastOp"; case 0x2c: return "ImplementsOp"; case 0x2d: return "FSCommand2";
        case 0x30: return "RandomNumber"; case 0x31: return "MBStringLength"; case 0x32: return "CharToAscii";
        case 0x33: return "AsciiToChar"; case 0x34: return "GetTime"; case 0x35: return "MBStringExtract";
        case 0x36: return "MBCharToAscii"; case 0x37: return "MBAsciiToChar"; case 0x3a: return "Delete";
        case 0x3b: return "Delete2"; case 0x3c: return "DefineLocal"; case 0x3d: return "CallFunction";
        case 0x3e: return "Return"; case 0x3f: return "Modulo"; case 0x40: return "NewObject";
        case 0x41: return "DefineLocal2"; case 0x42: return "InitArray"; case 0x43: return "InitObject";
        case 0x44: return "TypeOf"; case 0x45: return "TargetPath"; case 0x46: return "Enumerate";
        case 0x47: return "Add2"; case 0x48: return "Less2"; case 0x49: return "Equals2";
        case 0x4a: return "ToNumber"; case 0x4b: return "ToString"; case 0x4c: return "PushDuplicate";
        case 0x4d: return "StackSwap"; case 0x4e: return "GetMember"; case 0x4f: return "SetMember";
        case 0x50: return "Increment"; case 0x51: return "Decrement"; case 0x52: return "CallMethod";
        case 0x53: return "NewMethod"; case 0x54: return "InstanceOf"; case 0x55: return "Enumerate2";
        case 0x60: return "BitAnd"; case 0x61: return "BitOr"; case 0x62: return "BitXor";
        case 0x63: return "BitLShift"; case 0x64: return "BitRShift"; case 0x65: return "BitURShift";
        case 0x66: return "StrictEquals"; case 0x67: return "Greater"; case 0x68: return "StringGreater";
        case 0x69: return "Extends";
        case 0x81: return "GotoFrame"; case 0x83: return "GetURL"; case 0x87: return "StoreRegister";
        case 0x88: return "ConstantPool"; case 0x8a: return "WaitForFrame"; case 0x8b: return "SetTarget";
        case 0x8c: return "GoToLabel"; case 0x8d: return "WaitForFrame2"; case 0x8e: return "DefineFunction2";
        case 0x8f: return "Try"; case 0x94: return "With"; case 0x96: return "Push"; case 0x99: return "Jump";
        case 0x9a: return "GetURL2"; case 0x9b: return "DefineFunction"; case 0x9d: return "If";
        case 0x9e: return "Call"; case 0x9f: return "GotoFrame2";
        default: return nullptr;
    }
}

std::string formatAVM1Action(const AVM1Action& a, const std::vector<std::string>* constants) {
    std::ostringstream o;
    o << std::setw(6) << std::setfill('0') << a.offset << std::setfill(' ') << "  ";
    const char* name = avm1ActionName(a.code);
    if (name) o << std::left << std::setw(16) << name;
    else o << "Unknown_0x" << std::hex << int(a.code) << std::dec << "  ";
    using T = AVM1PushValue::Type;
    switch (a.code) {
        case 0x96:
            for (std::size_t i = 0; i < a.push.size(); ++i) {
                const auto& v = a.push[i];
                if (i) o << ", ";
                switch (v.type) {
                    case T::String: o << quote(v.str); break;
                    case T::Float: case T::Double: o << std::setprecision(17) << v.num; break;
                    case T::Integer: o << static_cast<std::int64_t>(v.num); break;
                    case T::Null: o << "null"; break;
                    case T::Undefined: o << "undefined"; break;
                    case T::Register: o << "r" << v.index; break;
                    case T::Boolean: o << (v.num != 0 ? "true" : "false"); break;
                    case T::Constant:
                        if (constants && v.index < constants->size()) o << quote((*constants)[v.index]);
                        else o << "c" << v.index;
                        break;
                }
            }
            break;
        case 0x88: o << a.strings.size() << " constants"; break;
        case 0x9b: case 0x8e: {
            o << (a.strings.empty() || a.strings[0].empty() ? "<anonymous>" : a.strings[0]) << "(";
            for (std::size_t i = 1; i < a.strings.size(); ++i) o << (i > 1 ? ", " : "") << a.strings[i];
            o << ") -> fn#" << a.function;
            break;
        }
        default:
            for (const auto& s : a.strings) o << quote(s) << " ";
            if (!a.targets.empty()) {
                o << "->";
                for (auto t : a.targets) o << " " << t;
            } else {
                for (auto v : a.ints) o << v << " ";
            }
    }
    return o.str();
}

} // namespace flashport

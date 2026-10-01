#include "flashport/AVM2Code.hpp"
#include "flashport/ByteReader.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iomanip>
#include <set>
#include <sstream>

namespace flashport {
namespace {

using O = AVM2Operands;

std::array<AVM2OpInfo, 256> buildOpTable() {
    std::array<AVM2OpInfo, 256> t{};
    auto set = [&](int op, const char* name, O o = O::None) { t[static_cast<std::size_t>(op)] = {name, o}; };
    set(0x01, "bkpt"); set(0x02, "nop"); set(0x03, "throw");
    set(0x04, "getsuper", O::U30); set(0x05, "setsuper", O::U30);
    set(0x06, "dxns", O::U30); set(0x07, "dxnslate"); set(0x08, "kill", O::U30);
    set(0x09, "label");
    set(0x0c, "ifnlt", O::S24); set(0x0d, "ifnle", O::S24); set(0x0e, "ifngt", O::S24);
    set(0x0f, "ifnge", O::S24); set(0x10, "jump", O::S24); set(0x11, "iftrue", O::S24);
    set(0x12, "iffalse", O::S24); set(0x13, "ifeq", O::S24); set(0x14, "ifne", O::S24);
    set(0x15, "iflt", O::S24); set(0x16, "ifle", O::S24); set(0x17, "ifgt", O::S24);
    set(0x18, "ifge", O::S24); set(0x19, "ifstricteq", O::S24); set(0x1a, "ifstrictne", O::S24);
    set(0x1b, "lookupswitch", O::Switch);
    set(0x1c, "pushwith"); set(0x1d, "popscope"); set(0x1e, "nextname"); set(0x1f, "hasnext");
    set(0x20, "pushnull"); set(0x21, "pushundefined"); set(0x22, "pushconstant", O::U30);
    set(0x23, "nextvalue"); set(0x24, "pushbyte", O::U8); set(0x25, "pushshort", O::U30);
    set(0x26, "pushtrue"); set(0x27, "pushfalse"); set(0x28, "pushnan"); set(0x29, "pop");
    set(0x2a, "dup"); set(0x2b, "swap"); set(0x2c, "pushstring", O::U30);
    set(0x2d, "pushint", O::U30); set(0x2e, "pushuint", O::U30); set(0x2f, "pushdouble", O::U30);
    set(0x30, "pushscope"); set(0x31, "pushnamespace", O::U30); set(0x32, "hasnext2", O::U30U30);
    set(0x35, "li8"); set(0x36, "li16"); set(0x37, "li32"); set(0x38, "lf32"); set(0x39, "lf64");
    set(0x3a, "si8"); set(0x3b, "si16"); set(0x3c, "si32"); set(0x3d, "sf32"); set(0x3e, "sf64");
    set(0x40, "newfunction", O::U30); set(0x41, "call", O::U30); set(0x42, "construct", O::U30);
    set(0x43, "callmethod", O::U30U30); set(0x44, "callstatic", O::U30U30);
    set(0x45, "callsuper", O::U30U30); set(0x46, "callproperty", O::U30U30);
    set(0x47, "returnvoid"); set(0x48, "returnvalue"); set(0x49, "constructsuper", O::U30);
    set(0x4a, "constructprop", O::U30U30); set(0x4c, "callproplex", O::U30U30);
    set(0x4e, "callsupervoid", O::U30U30); set(0x4f, "callpropvoid", O::U30U30);
    set(0x50, "sxi1"); set(0x51, "sxi8"); set(0x52, "sxi16"); set(0x53, "applytype", O::U30);
    set(0x55, "newobject", O::U30); set(0x56, "newarray", O::U30); set(0x57, "newactivation");
    set(0x58, "newclass", O::U30); set(0x59, "getdescendants", O::U30); set(0x5a, "newcatch", O::U30);
    set(0x5b, "findpropglobalstrict", O::U30); set(0x5c, "findpropglobal", O::U30);
    set(0x5d, "findpropstrict", O::U30); set(0x5e, "findproperty", O::U30);
    set(0x5f, "finddef", O::U30); set(0x60, "getlex", O::U30); set(0x61, "setproperty", O::U30);
    set(0x62, "getlocal", O::U30); set(0x63, "setlocal", O::U30); set(0x64, "getglobalscope");
    set(0x65, "getscopeobject", O::U8); set(0x66, "getproperty", O::U30);
    set(0x67, "getouterscope", O::U30); set(0x68, "initproperty", O::U30);
    set(0x6a, "deleteproperty", O::U30); set(0x6c, "getslot", O::U30); set(0x6d, "setslot", O::U30);
    set(0x6e, "getglobalslot", O::U30); set(0x6f, "setglobalslot", O::U30);
    set(0x70, "convert_s"); set(0x71, "esc_xelem"); set(0x72, "esc_xattr"); set(0x73, "convert_i");
    set(0x74, "convert_u"); set(0x75, "convert_d"); set(0x76, "convert_b"); set(0x77, "convert_o");
    set(0x78, "checkfilter"); set(0x79, "convert_f"); set(0x7a, "unplus");
    set(0x80, "coerce", O::U30); set(0x81, "coerce_b"); set(0x82, "coerce_a"); set(0x83, "coerce_i");
    set(0x84, "coerce_d"); set(0x85, "coerce_s"); set(0x86, "astype", O::U30); set(0x87, "astypelate");
    set(0x88, "coerce_u"); set(0x89, "coerce_o");
    set(0x90, "negate"); set(0x91, "increment"); set(0x92, "inclocal", O::U30); set(0x93, "decrement");
    set(0x94, "declocal", O::U30); set(0x95, "typeof"); set(0x96, "not"); set(0x97, "bitnot");
    set(0xa0, "add"); set(0xa1, "subtract"); set(0xa2, "multiply"); set(0xa3, "divide");
    set(0xa4, "modulo"); set(0xa5, "lshift"); set(0xa6, "rshift"); set(0xa7, "urshift");
    set(0xa8, "bitand"); set(0xa9, "bitor"); set(0xaa, "bitxor"); set(0xab, "equals");
    set(0xac, "strictequals"); set(0xad, "lessthan"); set(0xae, "lessequals");
    set(0xaf, "greaterthan"); set(0xb0, "greaterequals"); set(0xb1, "instanceof");
    set(0xb2, "istype", O::U30); set(0xb3, "istypelate"); set(0xb4, "in");
    set(0xc0, "increment_i"); set(0xc1, "decrement_i"); set(0xc2, "inclocal_i", O::U30);
    set(0xc3, "declocal_i", O::U30); set(0xc4, "negate_i"); set(0xc5, "add_i");
    set(0xc6, "subtract_i"); set(0xc7, "multiply_i");
    set(0xd0, "getlocal0"); set(0xd1, "getlocal1"); set(0xd2, "getlocal2"); set(0xd3, "getlocal3");
    set(0xd4, "setlocal0"); set(0xd5, "setlocal1"); set(0xd6, "setlocal2"); set(0xd7, "setlocal3");
    set(0xef, "debug", O::Debug); set(0xf0, "debugline", O::U30); set(0xf1, "debugfile", O::U30);
    set(0xf2, "bkptline", O::U30); set(0xf3, "timestamp");
    return t;
}

const std::array<AVM2OpInfo, 256>& opTable() {
    static const auto table = buildOpTable();
    return table;
}

bool isTerminator(std::uint8_t op) {
    return op == 0x10 || op == 0x1b || op == 0x47 || op == 0x48 || op == 0x03;
}

bool isConditionalBranch(std::uint8_t op) {
    return (op >= 0x0c && op <= 0x0f) || (op >= 0x11 && op <= 0x1a);
}

std::int32_t readS24(ByteReader& r) {
    const std::uint32_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
    std::uint32_t v = b0 | (b1 << 8) | (b2 << 16);
    if (v & 0x800000u) v |= 0xff000000u;
    return static_cast<std::int32_t>(v);
}

// Extra stack slots consumed by a runtime multiname (namespace and/or name on the stack).
int runtimeMultinameArgs(const ABCFile& abc, std::uint32_t mn) {
    if (mn >= abc.multinames.size()) return 0;
    switch (abc.multinames[mn].kind) {
        case 0x0f: case 0x10: return 1; // RTQName
        case 0x11: case 0x12: return 2; // RTQNameL
        case 0x1b: case 0x1c: return 1; // MultinameL
        default: return 0;
    }
}

struct Effect { int pop = 0; int push = 0; int scope = 0; };

Effect stackEffect(const ABCFile& abc, const AVM2Instruction& ins) {
    const auto a0 = ins.operands.empty() ? 0 : ins.operands[0];
    const auto a1 = ins.operands.size() > 1 ? ins.operands[1] : 0;
    const auto rt = [&](std::int32_t mn) { return runtimeMultinameArgs(abc, static_cast<std::uint32_t>(mn)); };
    switch (ins.op) {
        case 0x04: return {1 + rt(a0), 1};                 // getsuper
        case 0x05: return {2 + rt(a0), 0};                 // setsuper
        case 0x07: return {1, 0};                          // dxnslate
        case 0x0c: case 0x0d: case 0x0e: case 0x0f:
        case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a:
            return {2, 0};
        case 0x11: case 0x12: case 0x1b: return {1, 0};
        case 0x1c: return {1, 0, +1};                      // pushwith
        case 0x1d: return {0, 0, -1};                      // popscope
        case 0x1e: case 0x1f: case 0x23: return {2, 1};    // nextname / hasnext / nextvalue
        case 0x20: case 0x21: case 0x22: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
        case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x31: case 0x32:
            return {0, 1};
        case 0x29: return {1, 0};
        case 0x2a: return {1, 2};
        case 0x2b: return {2, 2};
        case 0x30: return {1, 0, +1};                      // pushscope
        case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: return {1, 1};
        case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: return {2, 0};
        case 0x40: return {0, 1};                          // newfunction
        case 0x41: return {a0 + 2, 1};                     // call
        case 0x42: return {a0 + 1, 1};                     // construct
        case 0x43: case 0x44: return {a1 + 1, 1};          // callmethod / callstatic
        case 0x45: case 0x46: case 0x4c: case 0x4a: return {a1 + 1 + rt(a0), 1};
        case 0x4e: case 0x4f: return {a1 + 1 + rt(a0), 0};
        case 0x47: return {0, 0};
        case 0x48: return {1, 0};
        case 0x49: return {a0 + 1, 0};                     // constructsuper
        case 0x50: case 0x51: case 0x52: return {1, 1};
        case 0x53: return {a0 + 1, 1};                     // applytype
        case 0x55: return {a0 * 2, 1};                     // newobject
        case 0x56: return {a0, 1};                         // newarray
        case 0x57: return {0, 1};
        case 0x58: return {1, 1};                          // newclass
        case 0x59: return {1 + rt(a0), 1};                 // getdescendants
        case 0x5a: return {0, 1};                          // newcatch
        case 0x5b: case 0x5c: case 0x5d: case 0x5e: return {rt(a0), 1};
        case 0x5f: case 0x60: return {0, 1};
        case 0x61: case 0x68: return {2 + rt(a0), 0};      // setproperty / initproperty
        case 0x62: case 0x64: case 0x65: case 0x67: case 0x6e: return {0, 1};
        case 0x63: case 0x6f: return {1, 0};
        case 0x66: case 0x6a: return {1 + rt(a0), 1};      // getproperty / deleteproperty
        case 0x6c: return {1, 1};
        case 0x6d: return {2, 0};
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86:
        case 0x88: case 0x89: case 0x90: case 0x91: case 0x93: case 0x95: case 0x96: case 0x97:
        case 0xb2: case 0xc0: case 0xc1: case 0xc4:
            return {1, 1};
        case 0x87: case 0xb3: return {2, 1};
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
        case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        case 0xb0: case 0xb1: case 0xb4: case 0xc5: case 0xc6: case 0xc7:
            return {2, 1};
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: return {0, 1};
        case 0xd4: case 0xd5: case 0xd6: case 0xd7: return {1, 0};
        default: return {0, 0};
    }
}

std::string hexOffset(std::uint32_t off) {
    std::ostringstream o;
    o << std::setw(5) << std::setfill('0') << off;
    return o.str();
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\x%02x", c); out += buf; }
        else out.push_back(static_cast<char>(c));
    }
    return out + "\"";
}

} // namespace

const AVM2OpInfo& avm2OpInfo(std::uint8_t op) { return opTable()[op]; }

AVM2Method decodeAVM2Body(const ABCFile& abc, std::uint32_t bodyIndex) {
    AVM2Method m;
    m.bodyIndex = bodyIndex;
    const auto& body = abc.bodies.at(bodyIndex);
    const auto& code = body.code;
    const auto codeSize = static_cast<std::uint32_t>(code.size());

    // 1) Reachability-driven decoding.
    std::map<std::uint32_t, AVM2Instruction> decoded;
    std::set<std::uint32_t> leaders{0};
    std::set<std::uint32_t> handlerStarts;
    std::deque<std::uint32_t> work{0};
    for (const auto& ex : body.exceptions) {
        if (ex.target < codeSize) { work.push_back(ex.target); leaders.insert(ex.target); handlerStarts.insert(ex.target); }
        else m.errors.push_back("exception target out of range: " + std::to_string(ex.target));
    }
    std::vector<std::uint8_t> covered(codeSize, 0);

    auto addTarget = [&](AVM2Instruction& ins, std::int64_t target) {
        if (target < 0 || target >= codeSize) {
            m.errors.push_back("branch at " + std::to_string(ins.offset) + " targets " +
                               std::to_string(target) + " outside code");
            return;
        }
        ins.targets.push_back(static_cast<std::uint32_t>(target));
        leaders.insert(static_cast<std::uint32_t>(target));
        work.push_back(static_cast<std::uint32_t>(target));
    };

    if (codeSize == 0) m.errors.push_back("empty method body");
    while (!work.empty()) {
        std::uint32_t pc = work.front();
        work.pop_front();
        while (pc < codeSize && decoded.find(pc) == decoded.end()) {
            if (covered[pc]) {
                m.errors.push_back("jump into the middle of an instruction at " + std::to_string(pc));
                break;
            }
            AVM2Instruction ins;
            ins.offset = pc;
            ins.op = code[pc];
            const auto& info = opTable()[ins.op];
            if (!info.name) {
                m.errors.push_back("invalid opcode 0x" + [&] { std::ostringstream o; o << std::hex << int(ins.op); return o.str(); }() +
                                   " at " + std::to_string(pc));
                break;
            }
            ByteReader r(code, pc + 1);
            try {
                switch (info.operands) {
                    case O::None: break;
                    case O::U30: ins.operands.push_back(static_cast<std::int32_t>(r.u30())); break;
                    case O::U30U30:
                        ins.operands.push_back(static_cast<std::int32_t>(r.u30()));
                        ins.operands.push_back(static_cast<std::int32_t>(r.u30()));
                        break;
                    case O::U8: ins.operands.push_back(r.u8()); break;
                    case O::S24: ins.operands.push_back(readS24(r)); break;
                    case O::Switch: {
                        ins.operands.push_back(readS24(r));
                        const auto n = r.u30();
                        if (n > codeSize) throw std::runtime_error("lookupswitch case count too large");
                        ins.operands.push_back(static_cast<std::int32_t>(n));
                        for (std::uint32_t i = 0; i <= n; ++i) ins.operands.push_back(readS24(r));
                        break;
                    }
                    case O::Debug:
                        ins.operands.push_back(r.u8());
                        ins.operands.push_back(static_cast<std::int32_t>(r.u30()));
                        ins.operands.push_back(r.u8());
                        ins.operands.push_back(static_cast<std::int32_t>(r.u30()));
                        break;
                }
            } catch (const std::exception&) {
                m.errors.push_back("truncated operands at " + std::to_string(pc));
                break;
            }
            ins.size = static_cast<std::uint32_t>(r.pos()) - pc;
            if (std::any_of(covered.begin() + pc + 1, covered.begin() + pc + ins.size, [](auto c) { return c != 0; })) {
                m.errors.push_back("overlapping instructions at " + std::to_string(pc));
                break;
            }
            for (std::uint32_t i = pc; i < pc + ins.size; ++i) covered[i] = 1;

            const std::uint32_t next = pc + ins.size;
            if (info.operands == O::S24) {
                addTarget(ins, static_cast<std::int64_t>(next) + ins.operands[0]);
            } else if (info.operands == O::Switch) {
                // Switch offsets are relative to the lookupswitch opcode itself.
                addTarget(ins, static_cast<std::int64_t>(pc) + ins.operands[0]);
                for (std::size_t i = 2; i < ins.operands.size(); ++i) {
                    addTarget(ins, static_cast<std::int64_t>(pc) + ins.operands[i]);
                }
            }
            const auto op = ins.op;
            decoded.emplace(pc, std::move(ins));
            if (isConditionalBranch(op)) leaders.insert(next);
            if (isTerminator(op)) {
                if (next < codeSize) leaders.insert(next);
                break;
            }
            if (next >= codeSize) {
                m.errors.push_back("execution falls off the end of the method at " + std::to_string(pc));
            }
            pc = next;
        }
    }

    for (auto& [off, ins] : decoded) m.instructions.push_back(std::move(ins));
    m.unreachableBytes = static_cast<std::uint32_t>(std::count(covered.begin(), covered.end(), 0));

    // 2) Basic blocks. A block starts at every reachable leader.
    std::map<std::uint32_t, std::size_t> instrIndex;
    for (std::size_t i = 0; i < m.instructions.size(); ++i) instrIndex[m.instructions[i].offset] = i;
    for (std::size_t i = 0; i < m.instructions.size();) {
        AVM2Block b;
        b.start = m.instructions[i].offset;
        b.firstInstr = static_cast<std::uint32_t>(i);
        b.handler = handlerStarts.count(b.start) != 0;
        std::size_t j = i;
        while (true) {
            const auto& ins = m.instructions[j];
            const auto next = ins.offset + ins.size;
            ++j;
            const bool endHere = isTerminator(ins.op) || isConditionalBranch(ins.op) ||
                                 j >= m.instructions.size() || m.instructions[j].offset != next ||
                                 leaders.count(next) != 0;
            if (!endHere) continue;
            b.end = next;
            for (auto t : ins.targets) b.successors.push_back(t);
            if (!isTerminator(ins.op) && instrIndex.count(next)) b.successors.push_back(next);
            break;
        }
        b.instrCount = static_cast<std::uint32_t>(j - i);
        std::sort(b.successors.begin(), b.successors.end());
        b.successors.erase(std::unique(b.successors.begin(), b.successors.end()), b.successors.end());
        m.blocks.push_back(std::move(b));
        i = j;
    }

    // 3) Stack / scope depth verifier (dataflow over the CFG).
    std::map<std::uint32_t, std::size_t> blockAt;
    for (std::size_t i = 0; i < m.blocks.size(); ++i) blockAt[m.blocks[i].start] = i;
    std::deque<std::size_t> q;
    auto seed = [&](std::uint32_t start, int stack, int scope) {
        auto it = blockAt.find(start);
        if (it == blockAt.end()) return;
        auto& b = m.blocks[it->second];
        if (b.stackIn < 0) {
            b.stackIn = stack; b.scopeIn = scope;
            q.push_back(it->second);
        } else if (b.stackIn != stack || b.scopeIn != scope) {
            m.warnings.push_back("inconsistent depth at block " + std::to_string(start) + ": stack " +
                                 std::to_string(b.stackIn) + " vs " + std::to_string(stack) +
                                 ", scope " + std::to_string(b.scopeIn) + " vs " + std::to_string(scope));
        }
    };
    seed(0, 0, 0);
    for (auto h : handlerStarts) seed(h, 1, 0);
    while (!q.empty()) {
        const auto bi = q.front();
        q.pop_front();
        const auto& b = m.blocks[bi];
        int stack = b.stackIn, scope = b.scopeIn;
        bool broken = false;
        for (std::uint32_t k = 0; k < b.instrCount; ++k) {
            const auto& ins = m.instructions[b.firstInstr + k];
            const auto e = stackEffect(abc, ins);
            stack -= e.pop;
            if (stack < 0) {
                m.warnings.push_back("stack underflow at " + std::to_string(ins.offset) + " (" +
                                     opTable()[ins.op].name + ")");
                broken = true;
                break;
            }
            stack += e.push;
            scope += e.scope;
            if (scope < 0) {
                m.warnings.push_back("scope underflow at " + std::to_string(ins.offset));
                broken = true;
                break;
            }
            m.maxStackSeen = std::max(m.maxStackSeen, stack);
        }
        if (broken) continue;
        for (auto s : b.successors) seed(s, stack, scope);
    }
    if (static_cast<std::uint32_t>(m.maxStackSeen) > body.maxStack) {
        m.warnings.push_back("computed max stack " + std::to_string(m.maxStackSeen) +
                             " exceeds declared max_stack " + std::to_string(body.maxStack));
    }
    return m;
}

std::string formatAVM2Instruction(const ABCFile& abc, const AVM2Instruction& ins) {
    std::ostringstream o;
    const auto& info = opTable()[ins.op];
    o << hexOffset(ins.offset) << "  " << std::left << std::setw(18) << (info.name ? info.name : "???");
    const auto a0 = ins.operands.empty() ? 0 : ins.operands[0];
    const auto u0 = static_cast<std::uint32_t>(a0);
    switch (ins.op) {
        case 0x2c: case 0x06: case 0xf1: o << quote(abc.string(u0)); break;
        case 0x2d: o << (u0 < abc.ints.size() ? abc.ints[u0] : 0); break;
        case 0x2e: o << (u0 < abc.uints.size() ? abc.uints[u0] : 0u); break;
        case 0x2f: {
            const double d = u0 < abc.doubles.size() ? abc.doubles[u0] : 0.0;
            if (std::isnan(d)) o << "NaN"; else o << std::setprecision(17) << d;
            break;
        }
        case 0x24: o << static_cast<int>(static_cast<std::int8_t>(a0)); break;
        case 0x25: o << static_cast<std::int16_t>(a0); break;
        case 0x31: o << abc.namespaceName(u0); break;
        case 0x04: case 0x05: case 0x59: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        case 0x60: case 0x61: case 0x66: case 0x68: case 0x6a: case 0x80: case 0x86: case 0xb2:
            o << abc.multinameName(u0);
            break;
        case 0x45: case 0x46: case 0x4a: case 0x4c: case 0x4e: case 0x4f:
            o << abc.multinameName(u0) << " (" << ins.operands[1] << ")";
            break;
        case 0x43: case 0x44: o << "#" << a0 << " (" << ins.operands[1] << ")"; break;
        case 0x40: o << "method#" << a0; break;
        case 0x58: o << "class#" << a0; break;
        case 0x1b: {
            o << "default:" << ins.targets.at(0) << " [";
            for (std::size_t i = 1; i < ins.targets.size(); ++i) o << (i > 1 ? ", " : "") << ins.targets[i];
            o << "]";
            break;
        }
        default:
            if (!ins.targets.empty()) o << "-> " << ins.targets[0];
            else for (std::size_t i = 0; i < ins.operands.size(); ++i) o << (i ? " " : "") << ins.operands[i];
    }
    return o.str();
}

} // namespace flashport

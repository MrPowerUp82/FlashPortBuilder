#include "flashport/FrameScripts.hpp"
#include "flashport/ABCFile.hpp"
#include "flashport/AVM1Code.hpp"
#include "flashport/AVM2Code.hpp"
#include "flashport/ByteReader.hpp"
#include <optional>

namespace flashport {
namespace {

using Kind = FrameAction::Kind;

FrameAction act(Kind kind, std::int32_t frame = -1, std::string label = {}) {
    FrameAction a;
    a.kind = kind;
    a.frame = frame;
    a.label = std::move(label);
    return a;
}

// ---- AVM1 ----

std::vector<FrameAction> avm1Actions(const AVM1Program& prog) {
    std::vector<FrameAction> out;
    if (prog.functions.empty() || prog.functions[0].blocks.empty()) return out;
    const auto& f = prog.functions[0];
    const auto& block = f.blocks.front();
    std::vector<std::string> pool;
    std::optional<AVM1PushValue> top; // last literal pushed, if nothing consumed it since
    bool otherTarget = false;         // tellTarget / SetTarget in effect
    for (std::uint32_t k = 0; k < block.actionCount; ++k) {
        const auto& a = f.actions[block.firstAction + k];
        switch (a.code) {
            case 0x88: pool = a.strings; top.reset(); continue;
            case 0x96: top = a.push.empty() ? std::nullopt : std::optional<AVM1PushValue>(a.push.back()); continue;
            case 0x8b: otherTarget = !a.strings.empty() && !a.strings[0].empty(); top.reset(); continue;
            case 0x20: otherTarget = true; top.reset(); continue;
            default: break;
        }
        if (!otherTarget) {
            switch (a.code) {
                case 0x07: out.push_back(act(Kind::Stop)); break;
                case 0x06: out.push_back(act(Kind::Play)); break;
                case 0x04: out.push_back(act(Kind::NextFrame)); break;
                case 0x05: out.push_back(act(Kind::PrevFrame)); break;
                case 0x81: if (!a.ints.empty()) out.push_back(act(Kind::GotoAndStop, a.ints[0])); break;
                case 0x8c: if (!a.strings.empty()) out.push_back(act(Kind::GotoAndStop, -1, a.strings[0])); break;
                case 0x9f: { // GotoFrame2: frame (1-based number or label) popped from the stack
                    if (!top || a.ints.empty()) break;
                    const bool play = (a.ints[0] & 1) != 0;
                    const std::int32_t bias = a.ints.size() > 1 ? a.ints[1] : 0;
                    auto fa = act(play ? Kind::GotoAndPlay : Kind::GotoAndStop);
                    using T = AVM1PushValue::Type;
                    std::string label;
                    if (top->type == T::String) label = top->str;
                    else if (top->type == T::Constant && top->index < pool.size()) label = pool[top->index];
                    if (top->type == T::Integer || top->type == T::Double || top->type == T::Float) {
                        fa.frame = static_cast<std::int32_t>(top->num) - 1 + bias;
                    } else if (!label.empty()) {
                        char* end = nullptr;
                        const long v = std::strtol(label.c_str(), &end, 10);
                        if (end && *end == 0) fa.frame = static_cast<std::int32_t>(v) - 1 + bias;
                        else if (label.find(':') == std::string::npos && label.find('/') == std::string::npos) fa.label = label;
                        else break; // "path:frame" targets another clip
                    } else {
                        break;
                    }
                    out.push_back(fa);
                    break;
                }
                default: break;
            }
        }
        top.reset();
    }
    return out;
}

// ---- AVM2 ----

std::string dottedName(const ABCFile& abc, std::uint32_t mn) {
    auto s = abc.multinameName(mn);
    if (const auto pos = s.find("::"); pos != std::string::npos) s = s.substr(0, pos) + "." + s.substr(pos + 2);
    return s;
}

std::optional<Kind> timelineCall(const std::string& name, std::uint32_t argc) {
    if (argc == 0) {
        if (name == "stop") return Kind::Stop;
        if (name == "play") return Kind::Play;
        if (name == "nextFrame") return Kind::NextFrame;
        if (name == "prevFrame") return Kind::PrevFrame;
    } else if (argc == 1) {
        if (name == "gotoAndStop") return Kind::GotoAndStop;
        if (name == "gotoAndPlay") return Kind::GotoAndPlay;
    }
    return std::nullopt;
}

std::vector<FrameAction> avm2Actions(const ABCFile& abc, std::uint32_t methodIndex) {
    std::vector<FrameAction> out;
    if (methodIndex >= abc.methods.size() || abc.methods[methodIndex].body < 0) return out;
    const auto m = decodeAVM2Body(abc, static_cast<std::uint32_t>(abc.methods[methodIndex].body));
    if (m.blocks.empty()) return out;
    const auto& b = m.blocks.front();
    const auto at = [&](std::size_t k) -> const AVM2Instruction& { return m.instructions[b.firstInstr + k]; };
    for (std::size_t k = 0; k < b.instrCount; ++k) {
        const auto& ins = at(k);
        if (ins.op != 0x4f && ins.op != 0x46) continue; // callpropvoid / callproperty
        const auto name = abc.multinameLocal(static_cast<std::uint32_t>(ins.operands[0]));
        const auto argc = static_cast<std::uint32_t>(ins.operands[1]);
        const auto kind = timelineCall(name, argc);
        if (!kind || k < argc + 1) continue;
        // Receiver must be `this` (getlocal0) or the implicit scope lookup (findpropstrict <name>).
        const auto& recv = at(k - argc - 1);
        const bool thisCall = recv.op == 0xd0 ||
                              (recv.op == 0x5d && abc.multinameLocal(static_cast<std::uint32_t>(recv.operands[0])) == name);
        if (!thisCall) continue;
        auto fa = act(*kind);
        if (argc == 1) {
            const auto& arg = at(k - 1);
            const auto v = arg.operands.empty() ? 0 : arg.operands[0];
            if (arg.op == 0x24) fa.frame = static_cast<std::int8_t>(v) - 1;
            else if (arg.op == 0x25) fa.frame = static_cast<std::int16_t>(v) - 1;
            else if (arg.op == 0x2d && static_cast<std::size_t>(v) < abc.ints.size()) fa.frame = abc.ints[static_cast<std::size_t>(v)] - 1;
            else if (arg.op == 0x2c) fa.label = abc.string(static_cast<std::uint32_t>(v));
            else continue;
        }
        out.push_back(fa);
    }
    return out;
}

} // namespace

FrameScriptInfo extractFrameScripts(const SWFDocument& doc) {
    FrameScriptInfo info;

    // AVM1: DoAction tags attached to a frame.
    for (const auto& t : doc.tags) {
        if (t.code != 12) continue;
        ++info.avm1Scripts;
        const auto prog = decodeAVM1(doc.payload(t), t.length, doc.version);
        auto acts = avm1Actions(prog);
        if (acts.empty()) continue;
        info.recognisedActions += acts.size();
        auto& dst = info.actions[{t.spriteId, t.frame}];
        dst.insert(dst.end(), acts.begin(), acts.end());
    }

    // AVM2: classes linked to timeline symbols register frame scripts with addFrameScript(frame, method, ...).
    std::map<std::string, std::uint16_t> symbolIds;
    for (const auto& t : doc.tags) {
        if (t.code != 76) continue;
        ByteReader r(doc.data, t.offset);
        const auto count = r.u16();
        for (std::uint16_t i = 0; i < count && r.pos() < t.offset + t.length; ++i) {
            const auto id = r.u16();
            symbolIds[r.cstring()] = id;
        }
    }
    if (symbolIds.empty()) return info;
    for (const auto& t : doc.tags) {
        if (t.code != 82) continue;
        ABCFile abc;
        try {
            ByteReader p(doc.data, t.offset);
            (void)p.u32();
            const auto name = p.cstring();
            abc = parseABC(p.bytes(t.offset + t.length - p.pos()), name);
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& inst : abc.instances) {
            const auto sym = symbolIds.find(dottedName(abc, inst.name));
            if (sym == symbolIds.end()) continue;
            std::map<std::string, std::uint32_t> methodsByName;
            for (const auto& tr : inst.traits) {
                if (tr.kind == TraitKind::Method) methodsByName[abc.multinameLocal(tr.name)] = tr.index;
            }
            if (inst.iinit >= abc.methods.size() || abc.methods[inst.iinit].body < 0) continue;
            const auto ctor = decodeAVM2Body(abc, static_cast<std::uint32_t>(abc.methods[inst.iinit].body));
            std::optional<std::int32_t> frame;
            for (const auto& ins : ctor.instructions) {
                const auto v = ins.operands.empty() ? 0 : ins.operands[0];
                if (ins.op == 0x24) frame = static_cast<std::int8_t>(v);
                else if (ins.op == 0x25) frame = static_cast<std::int16_t>(v);
                else if (ins.op == 0x2d && static_cast<std::size_t>(v) < abc.ints.size()) frame = abc.ints[static_cast<std::size_t>(v)];
                else if ((ins.op == 0x66 || ins.op == 0x60) && frame) {
                    const auto it = methodsByName.find(abc.multinameLocal(static_cast<std::uint32_t>(v)));
                    if (it == methodsByName.end()) continue;
                    ++info.avm2Scripts;
                    auto acts = avm2Actions(abc, it->second);
                    info.recognisedActions += acts.size();
                    if (!acts.empty()) {
                        auto& dst = info.actions[{sym->second, static_cast<std::uint32_t>(*frame)}];
                        dst.insert(dst.end(), acts.begin(), acts.end());
                    }
                    frame.reset();
                }
            }
        }
    }
    return info;
}

} // namespace flashport

#include "flashport/Disassembler.hpp"
#include "flashport/ABCFile.hpp"
#include "flashport/AVM1Code.hpp"
#include "flashport/AVM2Code.hpp"
#include "flashport/ByteReader.hpp"
#include "flashport/SWFStructures.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <set>
#include <sstream>

namespace flashport {
namespace fs = std::filesystem;
namespace {

std::string sanitize(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') out.push_back(static_cast<char>(c));
        else if (c == ':' || c == '/' || c == '$') out.push_back('_');
        else { char buf[4]; std::snprintf(buf, sizeof buf, "%02x", c); out += buf; }
        if (out.size() > 80) break;
    }
    return out.empty() ? "_" : out;
}

std::string uniquePath(const fs::path& dir, const std::string& base, const std::string& ext,
                       std::set<std::string>& used) {
    std::string name = base;
    for (int i = 2; used.count(name); ++i) name = base + "_" + std::to_string(i);
    used.insert(name);
    return (dir / (name + ext)).string();
}

std::string methodSignature(const ABCFile& abc, std::uint32_t mi, const std::string& name) {
    if (mi >= abc.methods.size()) return name + "(<bad method>)";
    const auto& m = abc.methods[mi];
    std::ostringstream o;
    o << name << "(";
    for (std::size_t i = 0; i < m.paramTypes.size(); ++i) {
        if (i) o << ", ";
        if (i < m.paramNames.size()) o << abc.string(m.paramNames[i]) << ":";
        else o << "arg" << i << ":";
        o << abc.multinameLocal(m.paramTypes[i]);
    }
    if (m.flags & 0x04) o << (m.paramTypes.empty() ? "" : ", ") << "...rest";
    o << "):" << abc.multinameLocal(m.returnType);
    return o.str();
}

class AVM2Writer {
public:
    AVM2Writer(const ABCFile& abc, DisasmReport& r, const std::string& where)
        : abc_(abc), r_(r), where_(where), printed_(abc.methods.size(), false) {}

    // Decode (and optionally print) one method. Each method is decoded exactly once.
    void method(std::ostream* out, std::uint32_t mi, const std::string& title) {
        if (mi >= abc_.methods.size()) return;
        if (printed_[mi]) {
            if (out) *out << "\n  // " << title << " -> method#" << mi << " (listed above)\n";
            return;
        }
        printed_[mi] = true;
        const auto& m = abc_.methods[mi];
        if (out) *out << "\n  " << methodSignature(abc_, mi, title) << "   // method#" << mi << "\n";
        if (m.body < 0) {
            if (out) *out << "    <native or interface method: no body>\n";
            return;
        }
        const auto dm = decodeAVM2Body(abc_, static_cast<std::uint32_t>(m.body));
        ++r_.avm2Bodies;
        r_.avm2Instructions += dm.instructions.size();
        r_.avm2Blocks += dm.blocks.size();
        r_.avm2UnreachableBytes += dm.unreachableBytes;
        if (!dm.errors.empty()) {
            ++r_.avm2BodiesWithErrors;
            r_.problem(where_ + " method#" + std::to_string(mi) + ": " + dm.errors.front());
        }
        if (!dm.warnings.empty()) ++r_.avm2BodiesWithWarnings;
        for (const auto& ins : dm.instructions) {
            const auto* name = avm2OpInfo(ins.op).name;
            ++r_.avm2OpcodeUse[name ? name : "?"];
        }
        if (!out) return;
        const auto& body = abc_.bodies[static_cast<std::size_t>(m.body)];
        *out << "    // max_stack=" << body.maxStack << " locals=" << body.localCount
             << " scope=" << body.initScopeDepth << ".." << body.maxScopeDepth
             << " code=" << body.code.size() << "B blocks=" << dm.blocks.size();
        if (dm.unreachableBytes) *out << " unreachable=" << dm.unreachableBytes << "B";
        *out << "\n";
        for (const auto& ex : body.exceptions) {
            *out << "    // try " << ex.from << ".." << ex.to << " catch(" << abc_.multinameLocal(ex.excType)
                 << ") -> " << ex.target << "\n";
        }
        for (const auto& e : dm.errors) *out << "    // ERROR: " << e << "\n";
        for (const auto& w : dm.warnings) *out << "    // VERIFY: " << w << "\n";
        for (const auto& b : dm.blocks) {
            *out << "   block_" << b.start << ":";
            if (b.handler) *out << "  (exception handler)";
            if (b.stackIn >= 0) *out << "  [stack " << b.stackIn << ", scope " << b.scopeIn << "]";
            else *out << "  [unverified]";
            *out << "\n";
            for (std::uint32_t k = 0; k < b.instrCount; ++k) {
                *out << "      " << formatAVM2Instruction(abc_, dm.instructions[b.firstInstr + k]) << "\n";
            }
            if (!b.successors.empty()) {
                *out << "      ;; ->";
                for (auto s : b.successors) *out << " block_" << s;
                *out << "\n";
            }
        }
    }

    void traits(std::ostream* out, const std::vector<ABCTrait>& traits, const std::string& prefix) {
        for (const auto& t : traits) {
            const auto name = abc_.multinameName(t.name);
            switch (t.kind) {
                case TraitKind::Slot:
                case TraitKind::Const:
                    if (out) *out << "  " << prefix << traitKindName(t.kind) << " " << name << ":"
                                  << abc_.multinameLocal(t.typeName) << "\n";
                    break;
                case TraitKind::Method:
                case TraitKind::Getter:
                case TraitKind::Setter:
                case TraitKind::Function:
                    method(out, t.index, prefix + traitKindName(t.kind) + " " + name);
                    break;
                case TraitKind::Class:
                    if (out) *out << "  " << prefix << "class " << name << " (class#" << t.index << ")\n";
                    break;
            }
        }
    }

    const std::vector<bool>& printed() const { return printed_; }

private:
    const ABCFile& abc_;
    DisasmReport& r_;
    std::string where_;
    std::vector<bool> printed_;
};

void disassembleABC(const ABCFile& abc, const fs::path* dir, DisasmReport& r, const std::string& where) {
    ++r.abcBlocks;
    r.avm2Classes += abc.instances.size();
    AVM2Writer w(abc, r, where);
    std::set<std::string> used;

    for (std::size_t ci = 0; ci < abc.instances.size(); ++ci) {
        const auto& inst = abc.instances[ci];
        const auto& cls = abc.classes[ci];
        std::ofstream file;
        std::ostream* out = nullptr;
        if (dir) {
            file.open(uniquePath(*dir, sanitize(abc.multinameName(inst.name)), ".abc.txt", used));
            out = &file;
        }
        if (out) {
            *out << "class " << abc.multinameName(inst.name) << "   // class#" << ci << "\n";
            *out << "  extends " << abc.multinameName(inst.superName) << "\n";
            for (auto i : inst.interfaces) *out << "  implements " << abc.multinameName(i) << "\n";
            *out << "  flags:" << ((inst.flags & 1) ? " sealed" : "") << ((inst.flags & 2) ? " final" : "")
                 << ((inst.flags & 4) ? " interface" : "") << "\n";
        }
        w.method(out, cls.cinit, "static-init");
        w.traits(out, cls.traits, "static ");
        w.method(out, inst.iinit, "constructor");
        w.traits(out, inst.traits, "");
    }

    std::ofstream scripts;
    std::ostream* out = nullptr;
    if (dir) {
        scripts.open((*dir / "_scripts.abc.txt").string());
        out = &scripts;
    }
    for (std::size_t si = 0; si < abc.scripts.size(); ++si) {
        if (out) *out << "\nscript#" << si << "\n";
        w.method(out, abc.scripts[si].init, "script-init");
        w.traits(out, abc.scripts[si].traits, "");
    }
    // Closures (newfunction) and any other method not reachable from traits.
    if (out) *out << "\n// ---- closures and unreferenced methods ----\n";
    for (std::uint32_t mi = 0; mi < abc.methods.size(); ++mi) {
        if (!w.printed()[mi]) w.method(out, mi, "function#" + std::to_string(mi));
    }
}

void disassembleAVM1(const SWFDocument& doc, const fs::path* dir, DisasmReport& r) {
    const auto sources = findAVM1Sources(doc);
    // Buffered per output file: keeping hundreds of ofstreams open hits the CRT's
    // open-file limit on Windows (512) and later opens fail silently.
    std::map<std::string, std::ostringstream> files;
    for (const auto& src : sources) {
        ++r.avm1Sources;
        const auto prog = decodeAVM1(doc.data.data() + src.offset, src.length, doc.version);
        if (!prog.errors.empty()) {
            ++r.avm1SourcesWithErrors;
            r.problem(doc.sourceName + " [" + src.label + "]: " + prog.errors.front());
        }
        r.avm1Functions += prog.functions.size();
        for (const auto& f : prog.functions) {
            r.avm1Actions += f.actions.size();
            r.avm1Blocks += f.blocks.size();
            for (const auto& a : f.actions) {
                const char* n = avm1ActionName(a.code);
                if (!n) ++r.avm1UnknownActions;
                ++r.avm1ActionUse[n ? n : "Unknown"];
            }
        }
        if (!dir) continue;

        std::string key = src.spriteId == 0 ? "timeline_main" : "sprite_" + std::to_string(src.spriteId);
        if (src.label.rfind("DefineButton", 0) == 0) key = "buttons";
        if (src.label.rfind("DoInitAction", 0) == 0) key = "init_actions";
        auto& out = files[key];

        out << "\n==== " << src.label << " (" << src.length << " bytes) ====\n";
        for (const auto& e : prog.errors) out << "// ERROR: " << e << "\n";
        const std::vector<std::string>* pool = nullptr;
        for (std::size_t fi = 0; fi < prog.functions.size(); ++fi) {
            const auto& f = prog.functions[fi];
            if (fi > 0) {
                out << "\n  function fn#" << fi << " " << (f.name.empty() ? "<anonymous>" : f.name) << "(";
                for (std::size_t p = 0; p < f.params.size(); ++p) out << (p ? ", " : "") << f.params[p];
                out << ")" << (f.isV2 ? " [DefineFunction2 regs=" + std::to_string(f.registerCount) + "]" : "") << "\n";
            }
            for (const auto& b : f.blocks) {
                out << "   block_" << b.start << ":\n";
                for (std::uint32_t k = 0; k < b.actionCount; ++k) {
                    const auto& a = f.actions[b.firstAction + k];
                    if (a.code == 0x88) pool = &a.strings;
                    out << "      " << formatAVM1Action(a, pool) << "\n";
                }
                if (!b.successors.empty()) {
                    out << "      ;; ->";
                    for (auto s : b.successors) out << " block_" << s;
                    out << "\n";
                }
            }
        }
    }
    for (const auto& [key, text] : files) {
        std::ofstream f((*dir / (key + ".avm1.txt")).string(), std::ios::binary);
        f << text.str();
        if (!f) r.problem("cannot write " + key + ".avm1.txt");
    }
}

} // namespace

void disassembleSWF(const SWFDocument& doc, const std::string& outDir, DisasmReport& r) {
    fs::path root(outDir);
    std::size_t abcIndex = 0;
    for (const auto& t : doc.tags) {
        if (t.code != 82 && t.code != 72) continue;
        ByteReader p(doc.data, t.offset);
        std::string name = "DoABCDefine";
        if (t.code == 82) {
            (void)p.u32();
            name = p.cstring();
        }
        const auto bytes = p.bytes(t.offset + t.length - p.pos());
        const auto label = "abc" + std::to_string(abcIndex++) + (name.empty() ? "" : "_" + sanitize(name));
        try {
            const auto abc = parseABC(bytes, name);
            fs::path dir = root / "avm2" / label;
            if (!outDir.empty()) fs::create_directories(dir);
            disassembleABC(abc, outDir.empty() ? nullptr : &dir, r, doc.sourceName + " " + label);
        } catch (const std::exception& e) {
            r.problem(doc.sourceName + " " + label + ": ABC parse failed: " + e.what());
        }
    }

    fs::path avm1Dir = root / "avm1";
    if (!outDir.empty()) fs::create_directories(avm1Dir);
    disassembleAVM1(doc, outDir.empty() ? nullptr : &avm1Dir, r);
    if (!outDir.empty() && fs::is_empty(avm1Dir)) fs::remove(avm1Dir);
}

void printDisasmReport(const DisasmReport& r, std::ostream& out) {
    out << "AVM2: " << r.abcBlocks << " ABC blocks, " << r.avm2Classes << " classes, " << r.avm2Bodies
        << " bodies, " << r.avm2Instructions << " instructions, " << r.avm2Blocks << " basic blocks\n";
    out << "      bodies with decode errors: " << r.avm2BodiesWithErrors
        << ", with verifier warnings: " << r.avm2BodiesWithWarnings
        << ", unreachable bytes: " << r.avm2UnreachableBytes << "\n";
    out << "AVM1: " << r.avm1Sources << " action sources, " << r.avm1Functions << " functions, "
        << r.avm1Actions << " actions, " << r.avm1Blocks << " basic blocks\n";
    out << "      sources with errors: " << r.avm1SourcesWithErrors
        << ", unknown actions: " << r.avm1UnknownActions << "\n";
    if (!r.problems.empty()) {
        out << "Problems (first " << r.problems.size() << "):\n";
        for (const auto& p : r.problems) out << "  - " << p << "\n";
    }
}

} // namespace flashport

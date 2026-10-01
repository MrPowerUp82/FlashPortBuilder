#include "flashport/ABCFile.hpp"
#include "flashport/ByteReader.hpp"
#include <cstring>
#include <stdexcept>

namespace flashport {
namespace {

std::vector<ABCTrait> readTraits(ByteReader& r) {
    const auto count = r.u30();
    std::vector<ABCTrait> traits;
    traits.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ABCTrait t;
        t.name = r.u30();
        const std::uint8_t kindAttr = r.u8();
        const std::uint8_t kind = kindAttr & 0x0f;
        t.attrs = kindAttr >> 4;
        if (kind > 6) throw std::runtime_error("unsupported ABC trait kind: " + std::to_string(kind));
        t.kind = static_cast<TraitKind>(kind);
        switch (t.kind) {
            case TraitKind::Slot:
            case TraitKind::Const:
                t.slotId = r.u30();
                t.typeName = r.u30();
                t.vindex = r.u30();
                if (t.vindex != 0) t.vkind = r.u8();
                break;
            case TraitKind::Method:
            case TraitKind::Getter:
            case TraitKind::Setter:
            case TraitKind::Class:
            case TraitKind::Function:
                t.slotId = r.u30();
                t.index = r.u30();
                break;
        }
        if ((t.attrs & 0x04) != 0) {
            const auto n = r.u30();
            for (std::uint32_t j = 0; j < n; ++j) t.metadata.push_back(r.u30());
        }
        traits.push_back(std::move(t));
    }
    return traits;
}

} // namespace

ABCFile parseABC(const std::vector<std::uint8_t>& bytes, const std::string& name) {
    ByteReader r(bytes);
    ABCFile f;
    f.name = name;
    f.minor = r.u16();
    f.major = r.u16();

    auto count = r.u30();
    f.ints.assign(1, 0);
    for (std::uint32_t i = 1; i < count; ++i) f.ints.push_back(r.s32());

    count = r.u30();
    f.uints.assign(1, 0);
    for (std::uint32_t i = 1; i < count; ++i) f.uints.push_back(r.u30());

    count = r.u30();
    f.doubles.assign(1, 0.0);
    for (std::uint32_t i = 1; i < count; ++i) f.doubles.push_back(r.f64());

    if (f.minor >= 17) { // ES4 decimal pool: not used by Flash Player, skipped
        count = r.u30();
        for (std::uint32_t i = 1; i < count; ++i) r.skip(16);
    }
    f.floats.assign(1, 0.0);
    if (f.hasFloat()) {
        count = r.u30();
        for (std::uint32_t i = 1; i < count; ++i) {
            const auto bits = r.u32();
            float v;
            std::memcpy(&v, &bits, 4);
            f.floats.push_back(v);
        }
    }

    count = r.u30();
    f.strings.assign(1, "");
    for (std::uint32_t i = 1; i < count; ++i) {
        const auto len = r.u30();
        f.strings.push_back(r.bytesAsString(len));
    }

    count = r.u30();
    f.namespaces.assign(1, {});
    for (std::uint32_t i = 1; i < count; ++i) {
        ABCNamespace ns;
        ns.kind = r.u8();
        ns.name = r.u30();
        f.namespaces.push_back(ns);
    }

    count = r.u30();
    f.nsSets.assign(1, {});
    for (std::uint32_t i = 1; i < count; ++i) {
        const auto n = r.u30();
        std::vector<std::uint32_t> set;
        for (std::uint32_t j = 0; j < n; ++j) set.push_back(r.u30());
        f.nsSets.push_back(std::move(set));
    }

    count = r.u30();
    f.multinames.assign(1, {});
    for (std::uint32_t i = 1; i < count; ++i) {
        ABCMultiname m;
        m.kind = r.u8();
        switch (m.kind) {
            case 0x07: case 0x0d: m.ns = r.u30(); m.name = r.u30(); break;        // QName(A)
            case 0x0f: case 0x10: m.name = r.u30(); break;                        // RTQName(A)
            case 0x11: case 0x12: break;                                          // RTQNameL(A)
            case 0x09: case 0x0e: m.name = r.u30(); m.nsSet = r.u30(); break;     // Multiname(A)
            case 0x1b: case 0x1c: m.nsSet = r.u30(); break;                       // MultinameL(A)
            case 0x1d: {                                                          // TypeName
                m.typeBase = r.u30();
                const auto n = r.u30();
                for (std::uint32_t j = 0; j < n; ++j) m.typeParams.push_back(r.u30());
                break;
            }
            default:
                throw std::runtime_error("unsupported ABC multiname kind: " + std::to_string(m.kind));
        }
        f.multinames.push_back(std::move(m));
    }

    count = r.u30();
    f.methods.resize(count);
    for (auto& m : f.methods) {
        const auto paramCount = r.u30();
        m.returnType = r.u30();
        for (std::uint32_t p = 0; p < paramCount; ++p) m.paramTypes.push_back(r.u30());
        m.name = r.u30();
        m.flags = r.u8();
        if ((m.flags & 0x08) != 0) {
            const auto n = r.u30();
            for (std::uint32_t o = 0; o < n; ++o) {
                ABCOptional opt;
                opt.value = r.u30();
                opt.kind = r.u8();
                m.optionals.push_back(opt);
            }
        }
        if ((m.flags & 0x80) != 0) {
            for (std::uint32_t p = 0; p < paramCount; ++p) m.paramNames.push_back(r.u30());
        }
    }

    count = r.u30();
    f.metadata.resize(count);
    for (auto& md : f.metadata) {
        md.name = r.u30();
        const auto n = r.u30();
        md.items.resize(n);
        for (auto& it : md.items) it.first = r.u30();
        for (auto& it : md.items) it.second = r.u30();
    }

    count = r.u30();
    f.instances.resize(count);
    for (auto& inst : f.instances) {
        inst.name = r.u30();
        inst.superName = r.u30();
        inst.flags = r.u8();
        if ((inst.flags & 0x08) != 0) inst.protectedNs = r.u30();
        const auto n = r.u30();
        for (std::uint32_t j = 0; j < n; ++j) inst.interfaces.push_back(r.u30());
        inst.iinit = r.u30();
        inst.traits = readTraits(r);
    }
    f.classes.resize(count);
    for (auto& c : f.classes) {
        c.cinit = r.u30();
        c.traits = readTraits(r);
    }

    count = r.u30();
    f.scripts.resize(count);
    for (auto& s : f.scripts) {
        s.init = r.u30();
        s.traits = readTraits(r);
    }

    count = r.u30();
    f.bodies.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto& b = f.bodies[i];
        b.method = r.u30();
        b.maxStack = r.u30();
        b.localCount = r.u30();
        b.initScopeDepth = r.u30();
        b.maxScopeDepth = r.u30();
        b.code = r.bytes(r.u30());
        const auto ne = r.u30();
        for (std::uint32_t e = 0; e < ne; ++e) {
            ABCException ex;
            ex.from = r.u30(); ex.to = r.u30(); ex.target = r.u30();
            ex.excType = r.u30(); ex.varName = r.u30();
            b.exceptions.push_back(ex);
        }
        b.traits = readTraits(r);
        if (b.method < f.methods.size()) f.methods[b.method].body = static_cast<std::int32_t>(i);
    }
    return f;
}

std::string ABCFile::string(std::uint32_t i) const {
    return i < strings.size() ? strings[i] : "<bad-string#" + std::to_string(i) + ">";
}

std::string ABCFile::namespaceName(std::uint32_t i) const {
    if (i == 0) return "*";
    if (i >= namespaces.size()) return "<bad-ns#" + std::to_string(i) + ">";
    const auto& ns = namespaces[i];
    const auto n = string(ns.name);
    switch (ns.kind) {
        case 0x05: return n.empty() ? "private" : "private:" + n;
        case 0x18: return n.empty() ? "protected" : "protected:" + n;
        case 0x1a: return n.empty() ? "static-protected" : "static-protected:" + n;
        case 0x19: return n.empty() ? "internal" : "internal:" + n;
        default: return n;
    }
}

std::string ABCFile::multinameLocal(std::uint32_t i) const {
    if (i == 0) return "*";
    if (i >= multinames.size()) return "<bad-mn#" + std::to_string(i) + ">";
    const auto& m = multinames[i];
    switch (m.kind) {
        case 0x07: case 0x0d: case 0x0f: case 0x10: case 0x09: case 0x0e:
            return m.name == 0 ? "*" : string(m.name);
        case 0x1d: {
            std::string s = multinameLocal(m.typeBase) + ".<";
            for (std::size_t j = 0; j < m.typeParams.size(); ++j) {
                if (j) s += ",";
                s += multinameLocal(m.typeParams[j]);
            }
            return s + ">";
        }
        default:
            return "[runtime]";
    }
}

std::string ABCFile::multinameName(std::uint32_t i) const {
    if (i == 0) return "*";
    if (i >= multinames.size()) return "<bad-mn#" + std::to_string(i) + ">";
    const auto& m = multinames[i];
    switch (m.kind) {
        case 0x07: case 0x0d: {
            const auto ns = namespaceName(m.ns);
            const auto local = multinameLocal(i);
            return ns.empty() ? local : ns + "::" + local;
        }
        case 0x0f: case 0x10: return "[rt-ns]::" + multinameLocal(i);
        case 0x11: case 0x12: return "[rt-ns]::[rt-name]";
        case 0x09: case 0x0e: return multinameLocal(i);
        case 0x1b: case 0x1c: return "[rt-name]";
        case 0x1d: return multinameLocal(i);
        default: return "<mn-kind-" + std::to_string(m.kind) + ">";
    }
}

const char* traitKindName(TraitKind k) {
    switch (k) {
        case TraitKind::Slot: return "slot";
        case TraitKind::Method: return "method";
        case TraitKind::Getter: return "getter";
        case TraitKind::Setter: return "setter";
        case TraitKind::Class: return "class";
        case TraitKind::Function: return "function";
        case TraitKind::Const: return "const";
    }
    return "?";
}

} // namespace flashport

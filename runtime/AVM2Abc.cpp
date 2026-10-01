#include "AVM2.hpp"
#include <cstring>
#include <stdexcept>

namespace fp::avm2 {
namespace {

class In {
public:
    explicit In(const std::vector<std::uint8_t>& d) : d_(d) {}
    std::uint8_t u8() { need(1); return d_[p_++]; }
    std::uint16_t u16() { const std::uint16_t lo = u8(); return static_cast<std::uint16_t>(lo | (u8() << 8)); }
    std::uint32_t u30() {
        std::uint32_t r = 0;
        for (int i = 0; i < 5; ++i) {
            const auto b = u8();
            r |= static_cast<std::uint32_t>(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) break;
        }
        return r;
    }
    std::int32_t s32() { return static_cast<std::int32_t>(u30()); }
    double d64() {
        need(8);
        std::uint64_t raw = 0;
        for (int i = 0; i < 8; ++i) raw |= static_cast<std::uint64_t>(d_[p_ + i]) << (8 * i);
        p_ += 8;
        double v;
        std::memcpy(&v, &raw, 8);
        return v;
    }
    std::string str(std::size_t n) {
        need(n);
        std::string s(reinterpret_cast<const char*>(d_.data() + p_), n);
        p_ += n;
        return s;
    }
    std::vector<std::uint8_t> bytes(std::size_t n) {
        need(n);
        std::vector<std::uint8_t> v(d_.begin() + static_cast<std::ptrdiff_t>(p_), d_.begin() + static_cast<std::ptrdiff_t>(p_ + n));
        p_ += n;
        return v;
    }
private:
    void need(std::size_t n) const { if (p_ + n > d_.size()) throw std::runtime_error("ABC truncated"); }
    const std::vector<std::uint8_t>& d_;
    std::size_t p_ = 0;
};

std::vector<TraitInfo> readTraits(In& in) {
    std::vector<TraitInfo> out(in.u30());
    for (auto& t : out) {
        t.name = in.u30();
        const auto ka = in.u8();
        t.kind = ka & 0x0f;
        t.attrs = ka >> 4;
        if (t.kind == 0 || t.kind == 6) {
            t.slotId = in.u30();
            t.typeName = in.u30();
            t.vindex = in.u30();
            if (t.vindex) t.vkind = in.u8();
        } else {
            t.slotId = in.u30();
            t.index = in.u30();
        }
        if (t.attrs & 0x04) {
            const auto n = in.u30();
            for (std::uint32_t i = 0; i < n; ++i) in.u30();
        }
    }
    return out;
}

} // namespace

std::unique_ptr<AbcFile> parseAbc(const std::vector<std::uint8_t>& bytes, const std::string& name) {
    auto abc = std::make_unique<AbcFile>();
    auto& f = *abc;
    f.name = name;
    In in(bytes);
    const auto minor = in.u16();
    const auto major = in.u16();

    auto n = in.u30();
    f.ints.assign(1, 0);
    for (std::uint32_t i = 1; i < n; ++i) f.ints.push_back(in.s32());
    n = in.u30();
    f.uints.assign(1, 0);
    for (std::uint32_t i = 1; i < n; ++i) f.uints.push_back(in.u30());
    n = in.u30();
    f.doubles.assign(1, 0.0);
    for (std::uint32_t i = 1; i < n; ++i) f.doubles.push_back(in.d64());
    if (minor >= 17) { // ES4 decimals, unused by the player
        n = in.u30();
        for (std::uint32_t i = 1; i < n; ++i) in.str(16);
    }
    f.floats.assign(1, 0.0);
    if (major > 47 || (major == 47 && minor >= 16)) { // Harman AIR floats
        n = in.u30();
        for (std::uint32_t i = 1; i < n; ++i) {
            const auto raw = in.str(4);
            float v;
            std::memcpy(&v, raw.data(), 4);
            f.floats.push_back(v);
        }
    }
    n = in.u30();
    f.strings.assign(1, "");
    for (std::uint32_t i = 1; i < n; ++i) f.strings.push_back(in.str(in.u30()));

    n = in.u30();
    f.namespaces.assign(1, Namespace::pub());
    for (std::uint32_t i = 1; i < n; ++i) {
        const auto kind = in.u8();
        const auto nameIndex = in.u30();
        Namespace ns;
        ns.uri = nameIndex < f.strings.size() ? f.strings[nameIndex] : std::string();
        switch (kind) {
            case 0x08: case 0x16: ns.kind = NsKind::Public; break;
            case 0x17: ns.kind = NsKind::Internal; break;
            case 0x18: case 0x1a: ns.kind = NsKind::Protected; break;
            case 0x19: ns.kind = NsKind::Explicit; break;
            case 0x05: ns.kind = NsKind::Private; break;
            default: ns.kind = NsKind::Public; break;
        }
        // The AS3 namespace (`use namespace AS3` / explicit Array.AS3::push) names the same builtin
        // methods as the public ones.
        if (ns.uri == "http://adobe.com/AS3/2006/builtin") ns = Namespace::pub();
        // Private namespaces need a stable identity: use the address of this ABC plus the index.
        if (ns.kind == NsKind::Private) ns.owner = reinterpret_cast<const char*>(abc.get()) + i;
        f.namespaces.push_back(ns);
    }

    n = in.u30();
    f.nsSets.assign(1, {});
    for (std::uint32_t i = 1; i < n; ++i) {
        std::vector<Namespace> set;
        const auto c = in.u30();
        for (std::uint32_t j = 0; j < c; ++j) {
            const auto idx = in.u30();
            if (idx < f.namespaces.size()) set.push_back(f.namespaces[idx]);
        }
        f.nsSets.push_back(std::move(set));
    }

    n = in.u30();
    f.multinames.assign(1, Multiname{});
    f.multinames[0].anyName = true;
    // Multinames may reference later TypeName entries; resolve TypeName after the loop.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> typeNames;
    for (std::uint32_t i = 1; i < n; ++i) {
        Multiname m;
        const auto kind = in.u8();
        auto str = [&](std::uint32_t idx) { return idx < f.strings.size() ? f.strings[idx] : std::string(); };
        switch (kind) {
            case 0x07: case 0x0d: {
                const auto ns = in.u30(), nm = in.u30();
                if (ns < f.namespaces.size()) m.nss.push_back(f.namespaces[ns]);
                m.name = str(nm);
                m.anyName = nm == 0;
                m.attribute = kind == 0x0d;
                break;
            }
            case 0x0f: case 0x10: m.name = str(in.u30()); m.rtNs = true; m.attribute = kind == 0x10; break;
            case 0x11: case 0x12: m.rtNs = true; m.rtName = true; m.attribute = kind == 0x12; break;
            case 0x09: case 0x0e: {
                const auto nm = in.u30(), set = in.u30();
                m.name = str(nm);
                m.anyName = nm == 0;
                if (set < f.nsSets.size()) m.nss = f.nsSets[set];
                m.attribute = kind == 0x0e;
                break;
            }
            case 0x1b: case 0x1c: {
                const auto set = in.u30();
                if (set < f.nsSets.size()) m.nss = f.nsSets[set];
                m.rtName = true;
                m.attribute = kind == 0x1c;
                break;
            }
            case 0x1d: { // TypeName (generics): keep the base name
                const auto base = in.u30();
                const auto c = in.u30();
                for (std::uint32_t j = 0; j < c; ++j) in.u30();
                typeNames.emplace_back(i, base);
                break;
            }
            default: throw std::runtime_error("bad multiname kind");
        }
        f.multinames.push_back(std::move(m));
    }
    for (const auto& [i, base] : typeNames) if (base < f.multinames.size()) f.multinames[i] = f.multinames[base];

    n = in.u30();
    f.methods.resize(n);
    for (auto& m : f.methods) {
        m.paramCount = in.u30();
        m.returnType = in.u30();
        for (std::uint32_t p = 0; p < m.paramCount; ++p) m.paramTypes.push_back(in.u30());
        const auto nm = in.u30();
        m.name = nm < f.strings.size() ? f.strings[nm] : std::string();
        m.flags = in.u8();
        if (m.flags & 0x08) {
            const auto c = in.u30();
            for (std::uint32_t o = 0; o < c; ++o) {
                const auto v = in.u30();
                m.optionals.emplace_back(v, in.u8());
            }
        }
        if (m.flags & 0x80) for (std::uint32_t p = 0; p < m.paramCount; ++p) in.u30();
    }

    n = in.u30();
    for (std::uint32_t i = 0; i < n; ++i) {
        in.u30();
        const auto c = in.u30();
        for (std::uint32_t j = 0; j < c * 2; ++j) in.u30();
    }

    n = in.u30();
    f.instances.resize(n);
    for (auto& inst : f.instances) {
        inst.name = in.u30();
        inst.superName = in.u30();
        inst.flags = in.u8();
        if (inst.flags & 0x08) inst.protectedNs = in.u30();
        const auto c = in.u30();
        for (std::uint32_t j = 0; j < c; ++j) inst.interfaces.push_back(in.u30());
        inst.iinit = in.u30();
        inst.traits = readTraits(in);
    }
    f.classes.resize(n);
    for (auto& c : f.classes) {
        c.first = in.u30();
        c.second = readTraits(in);
    }
    f.classObjects.resize(n);

    n = in.u30();
    f.scripts.resize(n);
    for (auto& s : f.scripts) {
        s.first = in.u30();
        s.second = readTraits(in);
    }

    n = in.u30();
    f.bodies.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto& b = f.bodies[i];
        b.method = in.u30();
        b.maxStack = in.u30();
        b.localCount = in.u30();
        b.initScopeDepth = in.u30();
        b.maxScopeDepth = in.u30();
        b.code = in.bytes(in.u30());
        const auto ne = in.u30();
        for (std::uint32_t e = 0; e < ne; ++e) {
            ExceptionInfo ex;
            ex.from = in.u30();
            ex.to = in.u30();
            ex.target = in.u30();
            ex.typeName = in.u30();
            ex.varName = in.u30();
            b.exceptions.push_back(ex);
        }
        b.traits = readTraits(in);
        if (b.method < f.methods.size()) f.methods[b.method].body = static_cast<int>(i);
    }
    return abc;
}

} // namespace fp::avm2

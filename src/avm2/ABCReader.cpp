#include "flashport/ABCReader.hpp"
#include "flashport/ByteReader.hpp"
#include <stdexcept>

namespace flashport {
namespace {

void skipTraits(ByteReader& r, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        (void)r.u30(); // name
        const std::uint8_t kindAttr = r.u8();
        const std::uint8_t kind = kindAttr & 0x0f;
        const std::uint8_t attrs = kindAttr >> 4;
        switch (kind) {
            case 0: // Slot
            case 6: { // Const
                (void)r.u30(); // slot_id
                (void)r.u30(); // type_name
                const auto vindex = r.u30();
                if (vindex != 0) (void)r.u8(); // vkind
                break;
            }
            case 1: // Method
            case 2: // Getter
            case 3: // Setter
                (void)r.u30(); // disp_id
                (void)r.u30(); // method
                break;
            case 4: // Class
                (void)r.u30(); // slot_id
                (void)r.u30(); // classi
                break;
            case 5: // Function
                (void)r.u30(); // slot_id
                (void)r.u30(); // function
                break;
            default:
                throw std::runtime_error("unsupported ABC trait kind: " + std::to_string(kind));
        }
        if ((attrs & 0x04) != 0) { // ATTR_Metadata
            const auto n = r.u30();
            for (std::uint32_t j = 0; j < n; ++j) (void)r.u30();
        }
    }
}

void skipConstantPool(ByteReader& r, std::uint16_t minor, std::uint16_t major) {
    auto count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) (void)r.s32();

    count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) (void)r.u30();

    count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) (void)r.f64();

    // ES4 decimals (minor 17) and Harman AIR floats (ABC 47.16+) add extra pools.
    if (minor >= 17) {
        count = r.u30();
        for (std::uint32_t i = 1; i < count; ++i) r.skip(16);
    }
    if (major > 47 || (major == 47 && minor >= 16)) {
        count = r.u30();
        for (std::uint32_t i = 1; i < count; ++i) r.skip(4);
    }

    count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) {
        const auto len = r.u30();
        r.skip(len);
    }

    count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) {
        (void)r.u8();
        (void)r.u30();
    }

    count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) {
        const auto n = r.u30();
        for (std::uint32_t j = 0; j < n; ++j) (void)r.u30();
    }

    count = r.u30();
    for (std::uint32_t i = 1; i < count; ++i) {
        const auto kind = r.u8();
        switch (kind) {
            case 0x07: // QName
            case 0x0d: // QNameA
                (void)r.u30(); (void)r.u30();
                break;
            case 0x0f: // RTQName
            case 0x10: // RTQNameA
                (void)r.u30();
                break;
            case 0x11: // RTQNameL
            case 0x12: // RTQNameLA
                break;
            case 0x09: // Multiname
            case 0x0e: // MultinameA
                (void)r.u30(); (void)r.u30();
                break;
            case 0x1b: // MultinameL
            case 0x1c: // MultinameLA
                (void)r.u30();
                break;
            case 0x1d: { // TypeName
                (void)r.u30();
                const auto n = r.u30();
                for (std::uint32_t j = 0; j < n; ++j) (void)r.u30();
                break;
            }
            default:
                throw std::runtime_error("unsupported ABC multiname kind: " + std::to_string(kind));
        }
    }
}

} // namespace

ABCSummary ABCReader::analyze(const std::vector<std::uint8_t>& abc, const std::string& name) const {
    ByteReader r(abc);
    ABCSummary out;
    out.name = name;

    const auto minor = r.u16();
    const auto major = r.u16();
    skipConstantPool(r, minor, major);

    out.methods = r.u30();
    for (std::uint32_t i = 0; i < out.methods; ++i) {
        const auto paramCount = r.u30();
        (void)r.u30(); // return_type
        for (std::uint32_t p = 0; p < paramCount; ++p) (void)r.u30();
        (void)r.u30(); // name
        const auto flags = r.u8();
        if ((flags & 0x08) != 0) { // HAS_OPTIONAL
            const auto optionalCount = r.u30();
            for (std::uint32_t o = 0; o < optionalCount; ++o) {
                (void)r.u30();
                (void)r.u8();
            }
        }
        if ((flags & 0x80) != 0) { // HAS_PARAM_NAMES
            for (std::uint32_t p = 0; p < paramCount; ++p) (void)r.u30();
        }
    }

    const auto metadataCount = r.u30();
    for (std::uint32_t i = 0; i < metadataCount; ++i) {
        (void)r.u30();
        const auto itemCount = r.u30();
        for (std::uint32_t j = 0; j < itemCount; ++j) (void)r.u30();
        for (std::uint32_t j = 0; j < itemCount; ++j) (void)r.u30();
    }

    out.classes = r.u30();
    for (std::uint32_t i = 0; i < out.classes; ++i) {
        (void)r.u30(); // name
        (void)r.u30(); // super
        const auto flags = r.u8();
        if ((flags & 0x08) != 0) (void)r.u30(); // protected namespace
        const auto interfaceCount = r.u30();
        for (std::uint32_t j = 0; j < interfaceCount; ++j) (void)r.u30();
        (void)r.u30(); // iinit
        const auto traitCount = r.u30();
        skipTraits(r, traitCount);
    }

    for (std::uint32_t i = 0; i < out.classes; ++i) {
        (void)r.u30(); // cinit
        const auto traitCount = r.u30();
        skipTraits(r, traitCount);
    }

    out.scripts = r.u30();
    for (std::uint32_t i = 0; i < out.scripts; ++i) {
        (void)r.u30(); // init
        const auto traitCount = r.u30();
        skipTraits(r, traitCount);
    }

    out.methodBodies = r.u30();
    for (std::uint32_t i = 0; i < out.methodBodies; ++i) {
        (void)r.u30(); // method
        (void)r.u30(); // max_stack
        (void)r.u30(); // local_count
        (void)r.u30(); // init_scope_depth
        (void)r.u30(); // max_scope_depth
        const auto codeLength = r.u30();
        out.bytecodeBytes += codeLength;
        r.skip(codeLength);

        const auto exceptionCount = r.u30();
        for (std::uint32_t e = 0; e < exceptionCount; ++e) {
            (void)r.u30(); // from
            (void)r.u30(); // to
            (void)r.u30(); // target
            (void)r.u30(); // exc_type
            (void)r.u30(); // var_name
        }
        const auto traitCount = r.u30();
        skipTraits(r, traitCount);
    }

    return out;
}

} // namespace flashport

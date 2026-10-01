#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace flashport {

// Full in-memory model of an AVM2 ABC block (avm2overview.pdf, chapter 4).
// Index 0 of every constant pool is the implicit "any"/empty entry.

struct ABCNamespace {
    std::uint8_t kind{};
    std::uint32_t name{}; // string index
};

struct ABCMultiname {
    std::uint8_t kind{};
    std::uint32_t name{};       // string index (QName, RTQName, Multiname)
    std::uint32_t ns{};         // namespace index (QName)
    std::uint32_t nsSet{};      // ns_set index (Multiname, MultinameL)
    std::uint32_t typeBase{};   // TypeName: base multiname
    std::vector<std::uint32_t> typeParams;
};

struct ABCOptional {
    std::uint32_t value{};
    std::uint8_t kind{};
};

struct ABCMethod {
    std::vector<std::uint32_t> paramTypes;
    std::uint32_t returnType{};
    std::uint32_t name{};
    std::uint8_t flags{};
    std::vector<ABCOptional> optionals;
    std::vector<std::uint32_t> paramNames;
    std::int32_t body = -1; // index into ABCFile::bodies
};

enum class TraitKind : std::uint8_t { Slot = 0, Method = 1, Getter = 2, Setter = 3, Class = 4, Function = 5, Const = 6 };

struct ABCTrait {
    std::uint32_t name{}; // multiname
    TraitKind kind{};
    std::uint8_t attrs{};
    std::uint32_t slotId{};   // slot/const/class/function; disp_id for methods
    std::uint32_t typeName{}; // slot/const
    std::uint32_t vindex{};
    std::uint8_t vkind{};
    std::uint32_t index{};    // method / class / function index
    std::vector<std::uint32_t> metadata;
};

struct ABCInstance {
    std::uint32_t name{};
    std::uint32_t superName{};
    std::uint8_t flags{};
    std::uint32_t protectedNs{};
    std::vector<std::uint32_t> interfaces;
    std::uint32_t iinit{};
    std::vector<ABCTrait> traits;
};

struct ABCClass {
    std::uint32_t cinit{};
    std::vector<ABCTrait> traits;
};

struct ABCScript {
    std::uint32_t init{};
    std::vector<ABCTrait> traits;
};

struct ABCException {
    std::uint32_t from{}, to{}, target{}, excType{}, varName{};
};

struct ABCMethodBody {
    std::uint32_t method{};
    std::uint32_t maxStack{}, localCount{}, initScopeDepth{}, maxScopeDepth{};
    std::vector<std::uint8_t> code;
    std::vector<ABCException> exceptions;
    std::vector<ABCTrait> traits;
};

struct ABCMetadata {
    std::uint32_t name{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> items;
};

struct ABCFile {
    std::string name;
    std::uint16_t minor{}, major{};
    std::vector<std::int32_t> ints;
    std::vector<std::uint32_t> uints;
    std::vector<double> doubles;
    std::vector<double> floats;   // ABC 47.16+ (Harman AIR) float pool, widened
    bool hasFloat() const { return major > 47 || (major == 47 && minor >= 16); }
    std::vector<std::string> strings;
    std::vector<ABCNamespace> namespaces;
    std::vector<std::vector<std::uint32_t>> nsSets;
    std::vector<ABCMultiname> multinames;
    std::vector<ABCMethod> methods;
    std::vector<ABCMetadata> metadata;
    std::vector<ABCInstance> instances;
    std::vector<ABCClass> classes;
    std::vector<ABCScript> scripts;
    std::vector<ABCMethodBody> bodies;

    std::string string(std::uint32_t i) const;
    std::string namespaceName(std::uint32_t i) const;
    // Human readable multiname, e.g. "flash.display:Sprite" or "[ns...]::foo".
    std::string multinameName(std::uint32_t i) const;
    // Only the local name part of a multiname ("Sprite").
    std::string multinameLocal(std::uint32_t i) const;
};

ABCFile parseABC(const std::vector<std::uint8_t>& bytes, const std::string& name);

const char* traitKindName(TraitKind k);

} // namespace flashport

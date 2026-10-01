#pragma once
// AVM2 (ActionScript 3) interpreter for the FlashPort runtime.
#include "FlashRuntime.hpp"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fp::avm2 {

struct Object;
struct Class;
struct AbcFile;
struct MethodBody;
class VM;
using ObjectPtr = std::shared_ptr<Object>;
using ClassPtr = std::shared_ptr<Class>;

struct Value {
    enum class Type : std::uint8_t { Undefined, Null, Boolean, Number, String, Object };
    Type type = Type::Undefined;
    bool b = false;
    double n = 0;
    std::string s;
    ObjectPtr o;

    Value() = default;
    static Value null() { Value v; v.type = Type::Null; return v; }
    Value(bool v) : type(Type::Boolean), b(v) {}
    Value(double v) : type(Type::Number), n(v) {}
    Value(int v) : type(Type::Number), n(v) {}
    Value(unsigned v) : type(Type::Number), n(v) {}
    Value(std::string v) : type(Type::String), s(std::move(v)) {}
    Value(const char* v) : type(Type::String), s(v) {}
    Value(ObjectPtr v) : type(v ? Type::Object : Type::Null), o(std::move(v)) {}
    template <typename T, typename = std::enable_if_t<std::is_base_of_v<Object, T> && !std::is_same_v<T, Object>>>
    Value(std::shared_ptr<T> v) : Value(std::static_pointer_cast<Object>(std::move(v))) {}

    bool isUndefined() const { return type == Type::Undefined; }
    bool isNull() const { return type == Type::Null; }
    bool isNullish() const { return type == Type::Undefined || type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isBoolean() const { return type == Type::Boolean; }
};
using Args = std::vector<Value>;

// ---------------------------------------------------------------- names

enum class NsKind : std::uint8_t { Public, Internal, Protected, Private, Explicit };

struct Namespace {
    NsKind kind = NsKind::Public;
    std::string uri;
    const void* owner = nullptr; // private namespaces are unique per ABC entry
    // Protected namespaces are per class, but a subclass override of a protected member must
    // replace the base member (AS3 forbids unrelated protected members with the same name).
    bool operator==(const Namespace& o) const {
        if (kind != o.kind) return false;
        if (kind == NsKind::Protected) return true;
        return uri == o.uri && owner == o.owner;
    }
    bool operator!=(const Namespace& o) const { return !(*this == o); }
    static Namespace pub(std::string uri = {}) { return {NsKind::Public, std::move(uri), nullptr}; }
};

struct Multiname {
    std::string name;
    std::vector<Namespace> nss;
    bool rtName = false, rtNs = false, attribute = false;
    bool anyName = false; // "*"
    bool hasPublic() const {
        for (const auto& n : nss) if (n.kind == NsKind::Public) return true;
        return nss.empty();
    }
    static Multiname publicName(std::string n) {
        Multiname m;
        m.name = std::move(n);
        m.nss.push_back(Namespace::pub());
        return m;
    }
};

// ---------------------------------------------------------------- methods and traits

struct NativeData;
using NativeFn = std::function<Value(VM&, const Value& self, Args& args)>;

struct MethodRef {
    AbcFile* abc = nullptr;
    std::uint32_t index = 0;
    NativeFn native;
    bool valid() const { return abc || native; }
};

struct Trait {
    enum class Kind : std::uint8_t { Slot, Method, Accessor, Class };
    Namespace ns;
    std::string name;
    Kind kind = Kind::Slot;
    bool isConst = false;
    std::uint32_t slot = 0;        // slot index (Slot / Class)
    std::string typeName;          // declared slot type (local name) for coercion and defaults
    Value initial;
    bool hasInitial = false;
    MethodRef method, getter, setter;
    Class* declaring = nullptr;    // class that declared the method (for super calls)
    int classIndex = -1;           // Class traits: ABC class index
};

struct TraitTable {
    std::vector<Trait> traits;
    std::unordered_multimap<std::string, std::uint32_t> byName;
    std::uint32_t slotCount = 0;

    const Trait* find(const Multiname& mn) const;
    const Trait* findName(const std::string& name, const Namespace& ns) const;
    const Trait* findPublic(const std::string& name) const;
    Trait* findExact(const std::string& name, const Namespace& ns);
    // Adds or overrides (same name + namespace) a trait. Slots get a fresh index unless overriding.
    Trait& add(Trait t);
};

// ---------------------------------------------------------------- objects

struct DynamicProps {
    struct Entry { std::string key; Value value; bool alive = true; };
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::size_t> index;
    Value* find(const std::string& k) {
        auto it = index.find(k);
        return it == index.end() ? nullptr : &entries[it->second].value;
    }
    void set(const std::string& k, Value v) {
        if (auto* p = find(k)) { *p = std::move(v); return; }
        index[k] = entries.size();
        entries.push_back({k, std::move(v), true});
    }
    bool remove(const std::string& k) {
        auto it = index.find(k);
        if (it == index.end()) return false;
        entries[it->second].alive = false;
        entries[it->second].value = Value();
        index.erase(it);
        return true;
    }
};

struct Listener { Value fn; int priority = 0; bool capture = false; };

// Native state attached to instances of native classes (and their script subclasses).
struct NativeData {
    virtual ~NativeData() = default;
    std::map<std::string, std::vector<Listener>> listeners; // EventDispatcher
};

struct Object : std::enable_shared_from_this<Object> {
    ClassPtr cls;
    std::shared_ptr<const TraitTable> traits;
    std::vector<Value> slots;
    DynamicProps dynamic;
    ObjectPtr proto;
    std::shared_ptr<NativeData> native;
    bool isDynamic = true;
    virtual ~Object() = default;
    virtual bool isFunction() const { return false; }
};

struct ArrayObject : Object {
    std::vector<Value> items;
};

struct FunctionObject : Object {
    MethodRef method;
    std::vector<ObjectPtr> scope;   // captured scope chain (closures)
    Value boundThis;                // method closures
    bool isMethodClosure = false;
    Class* declaring = nullptr;
    bool isFunction() const override { return true; }
};

struct DictionaryObject : Object {
    std::vector<std::pair<Value, Value>> entries; // insertion ordered, identity keys for objects
    int indexOf(VM& vm, const Value& key) const;
};

struct Class : Object {
    std::string name;
    Namespace ns;
    ClassPtr super;
    std::shared_ptr<TraitTable> instanceTraits;
    std::shared_ptr<TraitTable> staticTraits;
    MethodRef iinit;
    AbcFile* abc = nullptr;
    int index = -1;
    ObjectPtr prototype;
    std::vector<ObjectPtr> scope;                          // scope chain for its methods
    bool sealed = false, isInterface = false;
    std::vector<Class*> interfaces;
    std::function<void(VM&, Object&)> nativeInit;          // allocates native state for instances
    std::function<Value(VM&, Args&)> callAsFunction;       // Class(value) conversions
    std::function<ObjectPtr(VM&)> allocator;               // special instance kinds (Array, Dictionary...)
    std::string qualifiedName() const { return ns.uri.empty() ? name : ns.uri + "::" + name; }
    bool isSubclassOf(const Class* other) const;
};

// ---------------------------------------------------------------- ABC

struct ExceptionInfo {
    std::uint32_t from = 0, to = 0, target = 0; // instruction indices after decoding
    std::uint32_t typeName = 0, varName = 0;    // multiname indices
};

struct Instr {
    std::uint8_t op = 0;
    std::int32_t a = 0, b = 0;
    std::int32_t target = -1;          // branch target instruction index
    std::vector<std::int32_t> targets; // lookupswitch: default then cases
};

struct MethodInfo {
    std::uint32_t paramCount = 0;
    std::vector<std::uint32_t> paramTypes;
    std::uint32_t returnType = 0;
    std::string name;
    std::uint8_t flags = 0;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> optionals;
    int body = -1;
};

struct TraitInfo {
    std::uint32_t name = 0;
    std::uint8_t kind = 0, attrs = 0;
    std::uint32_t slotId = 0, typeName = 0, vindex = 0, index = 0;
    std::uint8_t vkind = 0;
};

struct MethodBody {
    std::uint32_t method = 0, maxStack = 0, localCount = 0, initScopeDepth = 0, maxScopeDepth = 0;
    std::vector<std::uint8_t> code;
    std::vector<ExceptionInfo> exceptions;
    std::vector<TraitInfo> traits;
    std::vector<Instr> instrs;
    bool decoded = false;
    std::shared_ptr<TraitTable> activationTraits;
};

struct InstanceInfo {
    std::uint32_t name = 0, superName = 0;
    std::uint8_t flags = 0;
    std::uint32_t protectedNs = 0;
    std::vector<std::uint32_t> interfaces;
    std::uint32_t iinit = 0;
    std::vector<TraitInfo> traits;
};

struct AbcFile {
    std::string name;
    std::vector<std::int32_t> ints;
    std::vector<std::uint32_t> uints;
    std::vector<double> doubles;
    std::vector<double> floats;   // ABC 47.16+ float pool (index 0 unused)
    std::vector<std::string> strings;
    std::vector<Namespace> namespaces;
    std::vector<std::vector<Namespace>> nsSets;
    std::vector<Multiname> multinames;
    std::vector<MethodInfo> methods;
    std::vector<InstanceInfo> instances;
    std::vector<std::pair<std::uint32_t, std::vector<TraitInfo>>> classes; // cinit, static traits
    std::vector<std::pair<std::uint32_t, std::vector<TraitInfo>>> scripts; // init, traits
    std::vector<MethodBody> bodies;
    std::vector<ClassPtr> classObjects; // created by newclass
};

std::unique_ptr<AbcFile> parseAbc(const std::vector<std::uint8_t>& bytes, const std::string& name);

// ---------------------------------------------------------------- errors

struct ScriptException {
    Value value;
};

// ---------------------------------------------------------------- VM

class VM {
public:
    explicit VM(Player& player);
    ~VM();
    Player& player;

    // Loading
    void loadAbc(const Code& bytes, const std::string& name);
    ClassPtr findClass(const std::string& qualifiedName); // "flash.display::MovieClip", "IPFI"

    // Object model
    // `key` is the runtime name value (MultinameL), so Dictionary keys keep object identity.
    Value getProperty(const Value& obj, const Multiname& mn, const Value* key = nullptr);
    void setProperty(const Value& obj, const Multiname& mn, const Value& v, bool init = false, const Value* key = nullptr);
    bool hasProperty(const Value& obj, const Multiname& mn, const Value* key = nullptr);
    bool deleteProperty(const Value& obj, const Multiname& mn, const Value* key = nullptr);
    Value callProperty(const Value& obj, const Multiname& mn, Args& args, bool lex = false, const Value* key = nullptr);
    Value call(const Value& fn, const Value& thisv, Args args);
    Value construct(const Value& ctor, Args args);
    ObjectPtr createInstance(const ClassPtr& cls);
    void runConstructor(const ClassPtr& cls, const ObjectPtr& obj, Args& args);
    Value invoke(const MethodRef& m, const Value& thisv, Args& args, const std::vector<ObjectPtr>* scope, Class* declaring);
    const TraitTable* traitsOf(const Value& v, Object** holder, Value* receiver);

    // Convenience for natives
    Value getPublic(const Value& obj, const std::string& name) { return getProperty(obj, Multiname::publicName(name)); }
    void setPublic(const Value& obj, const std::string& name, const Value& v) { setProperty(obj, Multiname::publicName(name), v); }
    Value callPublic(const Value& obj, const std::string& name, Args args = {}) {
        return callProperty(obj, Multiname::publicName(name), args);
    }
    ObjectPtr newObject();
    std::shared_ptr<ArrayObject> newArray(std::vector<Value> items = {});
    ObjectPtr newFunction(NativeFn fn);
    Value newError(const std::string& cls, const std::string& message);
    [[noreturn]] void throwError(const std::string& cls, const std::string& message);
    ClassPtr classOf(const Value& v);
    bool isType(const Value& v, const ClassPtr& cls);
    Value coerceToType(const Value& v, const std::string& typeName);

    // Conversions
    std::string toString(const Value& v);
    double toNumber(const Value& v);
    bool toBoolean(const Value& v);
    std::int32_t toInt32(const Value& v);
    std::uint32_t toUint32(const Value& v);
    Value toPrimitive(const Value& v, bool preferString = false);
    bool equals(const Value& a, const Value& b);
    bool strictEquals(const Value& a, const Value& b);
    Value lessThan(const Value& a, const Value& b); // undefined when NaN is involved
    std::string typeOf(const Value& v);
    bool sameFunction(const Value& a, const Value& b);

    // Native class definition
    ClassPtr defineNativeClass(const std::string& pkg, const std::string& name, const ClassPtr& super);
    void defineGlobal(const std::string& pkg, const std::string& name, const Value& v);
    void defineGlobalFunction(const std::string& pkg, const std::string& name, NativeFn fn);

    // Events and timers
    bool dispatchEvent(const ObjectPtr& target, const ObjectPtr& event);
    void addListener(const ObjectPtr& target, const std::string& type, const Value& fn, int priority, bool capture);
    void removeListener(const ObjectPtr& target, const std::string& type, const Value& fn, bool capture);
    ObjectPtr makeEvent(const std::string& cls, const std::string& type, bool bubbles = false);
    std::vector<std::weak_ptr<Object>> enterFrameListeners;
    struct Timer { std::weak_ptr<Object> timer; Value fn; Args args; double next = 0, period = 0; bool once = false; int id = 0; };
    std::map<int, Timer> timers;
    int nextTimerId = 1;
    void runTimers(double nowMs);
    double nowMs() const;

    // Queued work (frame scripts), run after timeline processing
    struct Task { std::weak_ptr<Object> target; int frame = -1; };
    std::deque<Task> queue;
    void runQueue();
    void runFrameScript(const ObjectPtr& clip, int frame);
    void reportError(const char* where, const ScriptException& e);

    ObjectPtr toplevel;
    ClassPtr objectClass, functionClass, classClass, arrayClass, stringClass, numberClass, intClass, uintClass,
        booleanClass, errorClass, namespaceClass, dictionaryClass;
    ObjectPtr stageObject;

    std::uint64_t instructions = 0;
    std::uint64_t errors = 0;
    std::set<std::string> warned;
    void warnOnce(const std::string& msg);

    // Display bridge (AVM2Flash.cpp)
    std::shared_ptr<DisplayObject> pendingDisplay; // bound by the next display object allocation
    ObjectPtr objectFor(DisplayObject& d);          // AS3 object for a display object (created on demand)
    ClassPtr classForCharacter(std::uint16_t character, DisplayObject::Kind kind);

private:
    struct ScriptEntry {
        AbcFile* abc = nullptr;
        std::uint32_t index = 0;
        ObjectPtr global;
        bool initialized = false;
    };
    struct Definition {
        Namespace ns;
        int script = -1;    // index into scripts_
        Value value;        // natives
    };
    std::vector<std::unique_ptr<AbcFile>> abcs_;
    std::vector<ScriptEntry> scripts_;
    std::unordered_multimap<std::string, Definition> definitions_;
    std::shared_ptr<TraitTable> toplevelTraits_;

    friend struct Interpreter;
    Value execute(AbcFile& abc, MethodBody& body, const Value& thisv, Args& args, const std::vector<ObjectPtr>* scope,
                  Class* declaring, const MethodInfo& info);
    void decode(AbcFile& abc, MethodBody& body);
    ObjectPtr findDefinition(const Multiname& mn, bool strict);
    void initScript(std::size_t index);
    ClassPtr newClass(AbcFile& abc, std::uint32_t index, const ClassPtr& base, const std::vector<ObjectPtr>& scope);
    void buildTraits(AbcFile& abc, const std::vector<TraitInfo>& infos, TraitTable& table, Class* declaring);
    void initSlots(Object& o);
    Value defaultForType(const std::string& typeName);
    Value getFromTraits(Object* holder, const Trait& t, const Value& receiver);
    void installBuiltins();
    void installFlash();
    friend void installBuiltinsImpl(VM& vm);
    friend void installFlashImpl(VM& vm);
};

// Implemented in AVM2Builtins.cpp / AVM2Flash.cpp
void installBuiltinsImpl(VM& vm);
void installFlashImpl(VM& vm);

// Bridge used by the display runtime (AVM2Flash.cpp).
void as3Start(Player& player);                                 // document class + root
void as3Step(Player& player);                                  // one frame
void as3PreAllocate(Player& player, DisplayObject& obj);        // before timeline content is built
void as3ClipCreated(Player& player, DisplayObject& obj);        // placed by a timeline
void as3ClipRemoved(Player& player, DisplayObject& obj);        // removed from its parent
void as3FrameEntered(Player& player, Clip& clip, int frame);
// Mouse event of `type` on `target` (bubbling through the display list to the stage).
void as3MouseEvent(Player& player, DisplayObject* target, const char* type, bool bubbles, DisplayObject* related);
void as3KeyEvent(Player& player, int keyCode, bool down);

// Small builder for native classes.
struct ClassBuilder {
    VM& vm;
    ClassPtr c;
    ClassBuilder& method(const std::string& name, NativeFn fn);
    ClassBuilder& getter(const std::string& name, NativeFn fn);
    ClassBuilder& setter(const std::string& name, NativeFn fn);
    ClassBuilder& property(const std::string& name, NativeFn get, NativeFn set) { getter(name, std::move(get)); return setter(name, std::move(set)); }
    ClassBuilder& staticMethod(const std::string& name, NativeFn fn);
    ClassBuilder& staticGetter(const std::string& name, NativeFn fn);
    ClassBuilder& constant(const std::string& name, const Value& v);
    ClassBuilder& ctor(NativeFn fn) { c->iinit.native = std::move(fn); c->iinit.abc = nullptr; return *this; }
    ClassBuilder& init(std::function<void(VM&, Object&)> fn) { c->nativeInit = std::move(fn); return *this; }
};

} // namespace fp::avm2

#pragma once
// AVM1 (ActionScript 1/2) interpreter for the FlashPort runtime.
#include "FlashRuntime.hpp"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fp::avm1 {

struct Object;
using ObjectPtr = std::shared_ptr<Object>;
class VM;

// onClipEvent masks as stored in PlaceObject clip actions (little-endian u32).
namespace ClipEvent {
constexpr std::uint32_t Load = 0x01, EnterFrame = 0x02, Unload = 0x04, MouseMove = 0x08, MouseDown = 0x10,
                        MouseUp = 0x20, KeyDown = 0x40, KeyUp = 0x80, Data = 0x100, Initialize = 0x200,
                        Press = 0x400, Release = 0x800, ReleaseOutside = 0x1000, RollOver = 0x2000,
                        RollOut = 0x4000, DragOver = 0x8000, DragOut = 0x10000, KeyPress = 0x20000,
                        Construct = 0x40000;
}

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
    Value(std::string v) : type(Type::String), s(std::move(v)) {}
    Value(const char* v) : type(Type::String), s(v) {}
    Value(ObjectPtr v) : type(v ? Type::Object : Type::Undefined), o(std::move(v)) {}

    bool isUndefined() const { return type == Type::Undefined; }
    bool isNullish() const { return type == Type::Undefined || type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
};

struct Object : std::enable_shared_from_this<Object> {
    std::map<std::string, Value> props;
    ObjectPtr proto;
    virtual ~Object() = default;
    virtual const char* typeName() const { return "object"; }
    // Own property lookup (no prototype chain). Subclasses expose virtual properties here.
    virtual bool getOwn(VM& vm, const std::string& name, Value& out);
    virtual void setOwn(VM& vm, const std::string& name, const Value& v);
    virtual bool removeOwn(const std::string& name) { return props.erase(name) > 0; }
    virtual void keys(std::vector<std::string>& out) const { for (const auto& [k, v] : props) out.push_back(k); }
    virtual bool callable() const { return false; }
    virtual Value call(VM& vm, const Value& thisv, std::vector<Value>& args);
};

struct NativeFunction : Object {
    using Fn = std::function<Value(VM&, const Value& thisv, std::vector<Value>& args)>;
    Fn fn;
    explicit NativeFunction(Fn f) : fn(std::move(f)) {}
    const char* typeName() const override { return "function"; }
    bool callable() const override { return true; }
    Value call(VM& vm, const Value& thisv, std::vector<Value>& args) override { return fn(vm, thisv, args); }
};

struct ScriptFunction : Object {
    std::string name;
    Code code;
    std::size_t start = 0, end = 0;
    bool v2 = false;
    std::uint8_t registerCount = 0;
    std::uint16_t flags = 0;
    std::vector<std::pair<std::uint8_t, std::string>> params; // (register or 0, name)
    std::vector<ObjectPtr> scope;                              // enclosing activations, outermost first
    std::weak_ptr<Object> base;                                // timeline the function was defined on
    std::shared_ptr<const std::vector<std::string>> pool;      // constant pool at definition time
    const char* typeName() const override { return "function"; }
    bool callable() const override { return true; }
    Value call(VM& vm, const Value& thisv, std::vector<Value>& args) override;
};

// `super` inside a DefineFunction2 with PreloadSuper: property lookups continue on the home
// prototype's parent, method calls keep the caller's `this`, and calling it runs the superclass
// constructor (home.__constructor__) on that same `this`.
struct SuperObject : Object {
    Value thisv;
    Value ctor;
    const char* typeName() const override { return "object"; }
    bool callable() const override { return ctor.isObject(); }
    Value call(VM& vm, const Value& thisv, std::vector<Value>& args) override;
};

struct ArrayObject : Object {
    std::vector<Value> items;
    bool getOwn(VM& vm, const std::string& name, Value& out) override;
    void setOwn(VM& vm, const std::string& name, const Value& v) override;
    void keys(std::vector<std::string>& out) const override;
};

// The AVM1 face of a MovieClip. `clip` becomes null when the clip leaves the display list.
struct ClipObject : Object {
    Clip* clip = nullptr;
    const char* typeName() const override { return "movieclip"; }
    bool getOwn(VM& vm, const std::string& name, Value& out) override;
    void setOwn(VM& vm, const std::string& name, const Value& v) override;
    void keys(std::vector<std::string>& out) const override;
};

// The AVM1 face of a dynamic text field. `field` becomes null when it leaves the display list.
struct TextFieldObject : Object {
    DisplayObject* field = nullptr;
    const char* typeName() const override { return "object"; }
    bool getOwn(VM& vm, const std::string& name, Value& out) override;
    void setOwn(VM& vm, const std::string& name, const Value& v) override;
};

struct Context {
    std::shared_ptr<ClipObject> target;     // timeline for frame actions and unqualified variables
    std::shared_ptr<ClipObject> original;   // target before SetTarget
    Value thisv;
    ObjectPtr locals;                        // function activation (null in frame scripts)
    std::vector<ObjectPtr> scope;            // enclosing activations and `with` objects, outermost first
    std::vector<Value> registers;
    std::shared_ptr<const std::vector<std::string>> pool;
    int depth = 0;
};

class VM {
public:
    explicit VM(Player& player);
    Player& player;

    ObjectPtr global, objectProto, functionProto, arrayProto, stringProto, clipProto;
    // Object that owned the function about to be called (its prototype for constructors);
    // consumed by ScriptFunction::call to build `super`.
    ObjectPtr callHome;
    // Object.registerClass: character id -> constructor applied to new instances of that symbol.
    std::map<std::uint16_t, Value> registeredClasses;
    // Sound voices started by `Sound` objects, so onSoundComplete can reach the owner.
    struct SoundVoice { int handle; std::weak_ptr<Object> owner; };
    std::vector<SoundVoice> soundVoices;
    void pollSounds(); // calls onSoundComplete for voices that ended

    // Execution
    Value run(const Code& code, std::size_t start, std::size_t end, Context& ctx);
    Value call(const Value& fn, const Value& thisv, std::vector<Value> args);
    Value callMethod(const Value& obj, const std::string& name, std::vector<Value> args);

    // Action queue (frame scripts, clip events), processed in order.
    struct Task {
        Code code;                        // bytecode to run on `target`...
        std::weak_ptr<ClipObject> target;
        std::string handler;              // ...or a function property of `target` to call
        std::uint32_t clipEvent = 0;      // ...or its onClipEvent handlers for this event mask
        bool classInit = false;           // ...or apply the Object.registerClass class of its symbol
    };
    std::deque<Task> queue;
    void queueCode(const Code& code, Clip& target, std::size_t position);
    void queueHandler(Clip& target, const std::string& handler);
    void queueClipEvent(Clip& target, std::uint32_t eventMask);
    void queueClassInit(Clip& target);   // after the frame's DoInitAction blocks, like the Flash Player
    void applyRegisteredClass(Clip& clip);
    void runQueue();

    // Timers
    struct Interval { Value fn; Value thisv; std::string method; std::vector<Value> args; double period = 0, next = 0; bool once = false; };
    std::map<int, Interval> intervals;
    int nextInterval = 1;
    void runIntervals(double nowMs);
    double nowMs() const;

    // Listeners (Key/Mouse.addListener)
    std::vector<ObjectPtr> keyListeners, mouseListeners;

    // Conversions (SWF 7+ semantics)
    std::string toString(const Value& v);
    double toNumber(const Value& v);
    bool toBoolean(const Value& v);
    std::int32_t toInt32(const Value& v);
    Value toPrimitive(const Value& v);
    bool equals(const Value& a, const Value& b);       // ==
    bool strictEquals(const Value& a, const Value& b); // ===
    Value less(const Value& a, const Value& b);        // undefined when NaN is involved
    std::string typeOf(const Value& v);

    // Properties
    Value getMember(const Value& obj, const std::string& name);
    void setMember(const Value& obj, const std::string& name, const Value& v);
    Value getVariable(Context& ctx, const std::string& path);
    void setVariable(Context& ctx, const std::string& path, const Value& v);
    Value resolvePath(Context& ctx, const std::string& path); // "_root.a.b", "/a/b", "../x"

    // Helpers
    ObjectPtr newObject();
    std::shared_ptr<ArrayObject> newArray(std::vector<Value> items = {});
    ObjectPtr nativeFunction(NativeFunction::Fn fn);
    Value construct(const Value& ctor, std::vector<Value> args);
    std::shared_ptr<ClipObject> rootObject();
    std::string targetPath(Clip* clip);
    Clip* clipOf(const Value& v); // live clip behind a value (object or path string), else null
    Context frameContext(Clip& clip);

    std::uint64_t instructions = 0; // executed actions (diagnostics)
    std::uint64_t errors = 0;

private:
    void installGlobals();
    Value getInternal(const Value& obj, const std::string& name, int depth);
};

// Implemented in AVM1Builtins.cpp / AVM1Clip.cpp
void installBuiltins(VM& vm);
void installMovieClip(VM& vm);
Value clipGotoFrame(VM& vm, Clip& clip, const Value& frame, bool play);

// Flash key codes for SDL keys (0 = unmapped).
int flashKeyCode(SDL_Keycode key);

} // namespace fp::avm1

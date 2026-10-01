#include "AVM1.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace fp::avm1 {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint64_t kInstructionBudget = 20'000'000; // per queued task: guards against runaway loops

bool isIndex(const std::string& s, std::size_t& out) {
    if (s.empty() || s.size() > 9) return false;
    std::size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::size_t>(c - '0');
    }
    out = v;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Budget {
    std::uint64_t left = kInstructionBudget;
};
thread_local Budget* gBudget = nullptr;

} // namespace

// ------------------------------------------------------------------ objects

bool Object::getOwn(VM&, const std::string& name, Value& out) {
    auto it = props.find(name);
    if (it == props.end()) return false;
    out = it->second;
    return true;
}

void Object::setOwn(VM&, const std::string& name, const Value& v) { props[name] = v; }

Value Object::call(VM&, const Value&, std::vector<Value>&) { return {}; }

bool ArrayObject::getOwn(VM& vm, const std::string& name, Value& out) {
    std::size_t i;
    if (name == "length") { out = static_cast<double>(items.size()); return true; }
    if (isIndex(name, i)) {
        out = i < items.size() ? items[i] : Value();
        return i < items.size();
    }
    return Object::getOwn(vm, name, out);
}

void ArrayObject::setOwn(VM& vm, const std::string& name, const Value& v) {
    std::size_t i;
    if (name == "length") {
        const double n = vm.toNumber(v);
        if (n >= 0 && n < 1e7) items.resize(static_cast<std::size_t>(n));
        return;
    }
    if (isIndex(name, i) && i < 10'000'000) {
        if (i >= items.size()) items.resize(i + 1);
        items[i] = v;
        return;
    }
    Object::setOwn(vm, name, v);
}

void ArrayObject::keys(std::vector<std::string>& out) const {
    for (std::size_t i = 0; i < items.size(); ++i) out.push_back(std::to_string(i));
    Object::keys(out);
}

Value SuperObject::call(VM& vm, const Value&, std::vector<Value>& args) {
    if (!ctor.isObject() || !ctor.o->callable()) return {};
    Value proto = vm.getMember(ctor, "prototype");
    vm.callHome = proto.isObject() ? proto.o : nullptr;
    return ctor.o->call(vm, thisv, args);
}

Value ScriptFunction::call(VM& vm, const Value& thisv, std::vector<Value>& args) {
    ObjectPtr home = std::move(vm.callHome);
    vm.callHome = nullptr;
    Context ctx;
    auto baseObj = std::dynamic_pointer_cast<ClipObject>(base.lock());
    ctx.target = baseObj && baseObj->clip ? baseObj : vm.rootObject();
    ctx.original = ctx.target;
    ctx.thisv = thisv.isUndefined() ? Value(std::static_pointer_cast<Object>(ctx.target)) : thisv;
    ctx.locals = vm.newObject();
    ctx.locals->proto = nullptr;
    ctx.scope = scope;
    ctx.pool = pool;
    auto arguments = vm.newArray(args);
    arguments->props["callee"] = Value(shared_from_this());
    if (v2) {
        ctx.registers.assign(std::max<std::size_t>(registerCount, 1) + 1, Value());
        std::size_t r = 1;
        auto preload = [&](std::uint16_t bit, const Value& v) {
            if ((flags & bit) && r < ctx.registers.size()) ctx.registers[r++] = v;
        };
        preload(0x0001, ctx.thisv);
        preload(0x0004, Value(std::static_pointer_cast<Object>(arguments)));
        Value superv;
        if ((flags & 0x0010) && home && home->proto) {
            auto sup = std::make_shared<SuperObject>();
            sup->proto = home->proto;
            sup->thisv = ctx.thisv;
            if (auto it = home->props.find("__constructor__"); it != home->props.end()) sup->ctor = it->second;
            superv = Value(std::static_pointer_cast<Object>(sup));
        }
        preload(0x0010, superv);
        preload(0x0040, Value(std::static_pointer_cast<Object>(vm.rootObject())));
        if (flags & 0x0080) {
            Value parent;
            if (ctx.target && ctx.target->clip && ctx.target->clip->parent()) parent = Value(std::static_pointer_cast<Object>(ctx.target->clip->parent()->object()));
            preload(0x0080, parent);
        }
        preload(0x0100, Value(vm.global));
        if (!(flags & 0x0008)) ctx.locals->props["arguments"] = Value(std::static_pointer_cast<Object>(arguments));
        for (std::size_t i = 0; i < params.size(); ++i) {
            const Value v = i < args.size() ? args[i] : Value();
            if (params[i].first && params[i].first < ctx.registers.size()) ctx.registers[params[i].first] = v;
            else ctx.locals->props[params[i].second] = v;
        }
    } else {
        ctx.registers.assign(4, Value());
        ctx.locals->props["arguments"] = Value(std::static_pointer_cast<Object>(arguments));
        for (std::size_t i = 0; i < params.size(); ++i) ctx.locals->props[params[i].second] = i < args.size() ? args[i] : Value();
    }
    return vm.run(code, start, end, ctx);
}

// ------------------------------------------------------------------ VM basics

VM::VM(Player& p) : player(p) {
    objectProto = std::make_shared<Object>();
    functionProto = std::make_shared<Object>();
    functionProto->proto = objectProto;
    arrayProto = std::make_shared<Object>();
    arrayProto->proto = objectProto;
    stringProto = std::make_shared<Object>();
    stringProto->proto = objectProto;
    clipProto = std::make_shared<Object>();
    clipProto->proto = objectProto;
    global = std::make_shared<Object>();
    global->proto = objectProto;
    installBuiltins(*this);
    installMovieClip(*this);
}

double VM::nowMs() const { return player.timeMs(); }

ObjectPtr VM::newObject() {
    auto o = std::make_shared<Object>();
    o->proto = objectProto;
    return o;
}

std::shared_ptr<ArrayObject> VM::newArray(std::vector<Value> items) {
    auto a = std::make_shared<ArrayObject>();
    a->proto = arrayProto;
    a->items = std::move(items);
    return a;
}

ObjectPtr VM::nativeFunction(NativeFunction::Fn fn) {
    auto f = std::make_shared<NativeFunction>(std::move(fn));
    f->proto = functionProto;
    return f;
}

std::shared_ptr<ClipObject> VM::rootObject() { return player.root ? player.root->object() : nullptr; }

Clip* VM::clipOf(const Value& v) {
    if (v.isObject()) {
        if (auto c = std::dynamic_pointer_cast<ClipObject>(v.o)) return c->clip;
        return nullptr;
    }
    if (v.isString()) {
        Context ctx = frameContext(*player.root);
        return clipOf(resolvePath(ctx, v.s));
    }
    return nullptr;
}

std::string VM::targetPath(Clip* clip) {
    if (!clip || !clip->holder()) return "/";
    std::string path;
    for (Clip* c = clip; c && c->holder(); c = c->parent()) path = "/" + c->holder()->name + path;
    return path;
}

Context VM::frameContext(Clip& clip) {
    Context ctx;
    ctx.target = clip.object();
    ctx.original = ctx.target;
    ctx.thisv = Value(std::static_pointer_cast<Object>(ctx.target));
    ctx.registers.assign(4, Value());
    return ctx;
}

// ------------------------------------------------------------------ conversions

std::string VM::toString(const Value& v) {
    switch (v.type) {
        case Value::Type::Undefined: return "undefined";
        case Value::Type::Null: return "null";
        case Value::Type::Boolean: return v.b ? "true" : "false";
        case Value::Type::String: return v.s;
        case Value::Type::Number: {
            const double n = v.n;
            if (std::isnan(n)) return "NaN";
            if (std::isinf(n)) return n > 0 ? "Infinity" : "-Infinity";
            if (n == 0) return "0";
            if (std::fabs(n) < 1e15 && n == std::floor(n)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.0f", n);
                return buf;
            }
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.15g", n);
            std::string s(buf);
            // Flash writes exponents as e+NN / e-NN without zero padding.
            const auto e = s.find('e');
            if (e != std::string::npos) {
                std::string mant = s.substr(0, e), ex = s.substr(e + 1);
                const char sign = ex[0];
                ex = ex.substr(1);
                while (ex.size() > 1 && ex[0] == '0') ex.erase(0, 1);
                s = mant + "e" + (sign == '-' ? "-" : "+") + ex;
            }
            return s;
        }
        case Value::Type::Object: {
            if (auto c = std::dynamic_pointer_cast<ClipObject>(v.o)) return c->clip ? "_level0" + [&] {
                std::string p = targetPath(c->clip);
                for (auto& ch : p) if (ch == '/') ch = '.';
                return p == "." ? std::string() : p;
            }() : "";
            if (auto a = std::dynamic_pointer_cast<ArrayObject>(v.o)) {
                std::string out;
                for (std::size_t i = 0; i < a->items.size(); ++i) {
                    if (i) out += ",";
                    if (!a->items[i].isNullish()) out += toString(a->items[i]);
                }
                return out;
            }
            if (v.o->callable()) return "[type Function]";
            // Objects may define toString().
            Value ts = getMember(v, "toString");
            if (ts.isObject() && ts.o->callable() && ts.o.get() != getInternal(Value(objectProto), "toString", 0).o.get()) {
                return toString(call(ts, v, {}));
            }
            return "[object Object]";
        }
    }
    return "";
}

double VM::toNumber(const Value& v) {
    switch (v.type) {
        case Value::Type::Undefined: return kNaN;
        case Value::Type::Null: return kNaN;
        case Value::Type::Boolean: return v.b ? 1 : 0;
        case Value::Type::Number: return v.n;
        case Value::Type::String: {
            const char* s = v.s.c_str();
            while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
            if (!*s) return kNaN;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return static_cast<double>(std::strtoll(s + 2, nullptr, 16));
            char* end = nullptr;
            const double d = std::strtod(s, &end);
            while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
            if (end == s || (end && *end)) return kNaN;
            return d;
        }
        case Value::Type::Object: {
            if (std::dynamic_pointer_cast<ClipObject>(v.o) || v.o->callable()) return kNaN;
            Value p = toPrimitive(v);
            return p.isObject() ? kNaN : toNumber(p);
        }
    }
    return kNaN;
}

bool VM::toBoolean(const Value& v) {
    switch (v.type) {
        case Value::Type::Undefined: case Value::Type::Null: return false;
        case Value::Type::Boolean: return v.b;
        case Value::Type::Number: return v.n != 0 && !std::isnan(v.n);
        case Value::Type::String: return !v.s.empty();
        case Value::Type::Object: return true;
    }
    return false;
}

std::int32_t VM::toInt32(const Value& v) {
    const double d = toNumber(v);
    if (std::isnan(d) || std::isinf(d)) return 0;
    const double m = std::fmod(std::trunc(d), 4294967296.0);
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(m < 0 ? m + 4294967296.0 : m)));
}

Value VM::toPrimitive(const Value& v) {
    if (!v.isObject()) return v;
    if (std::dynamic_pointer_cast<ClipObject>(v.o)) return Value(toString(v));
    Value valueOf = getMember(v, "valueOf");
    if (valueOf.isObject() && valueOf.o->callable()) {
        Value r = call(valueOf, v, {});
        if (!r.isObject()) return r;
    }
    return Value(toString(v));
}

bool VM::strictEquals(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case Value::Type::Undefined: case Value::Type::Null: return true;
        case Value::Type::Boolean: return a.b == b.b;
        case Value::Type::Number: return a.n == b.n;
        case Value::Type::String: return a.s == b.s;
        case Value::Type::Object: {
            auto ca = std::dynamic_pointer_cast<ClipObject>(a.o), cb = std::dynamic_pointer_cast<ClipObject>(b.o);
            if (ca && cb) return ca->clip == cb->clip && (ca->clip || ca == cb);
            return a.o == b.o;
        }
    }
    return false;
}

bool VM::equals(const Value& a, const Value& b) {
    if (a.type == b.type) return strictEquals(a, b);
    if (a.isNullish() && b.isNullish()) return true;
    if (a.isNullish() || b.isNullish()) return false;
    if (a.isNumber() && b.isString()) return a.n == toNumber(b);
    if (a.isString() && b.isNumber()) return toNumber(a) == b.n;
    if (a.type == Value::Type::Boolean) return equals(Value(a.b ? 1.0 : 0.0), b);
    if (b.type == Value::Type::Boolean) return equals(a, Value(b.b ? 1.0 : 0.0));
    if (a.isObject() && !b.isObject()) return equals(toPrimitive(a), b);
    if (b.isObject() && !a.isObject()) return equals(a, toPrimitive(b));
    return false;
}

Value VM::less(const Value& a, const Value& b) {
    const Value pa = toPrimitive(a), pb = toPrimitive(b);
    if (pa.isString() && pb.isString()) return Value(pa.s < pb.s);
    const double x = toNumber(pa), y = toNumber(pb);
    if (std::isnan(x) || std::isnan(y)) return Value();
    return Value(x < y);
}

std::string VM::typeOf(const Value& v) {
    switch (v.type) {
        case Value::Type::Undefined: return "undefined";
        case Value::Type::Null: return "null";
        case Value::Type::Boolean: return "boolean";
        case Value::Type::Number: return "number";
        case Value::Type::String: return "string";
        case Value::Type::Object: return v.o->typeName();
    }
    return "undefined";
}

// ------------------------------------------------------------------ properties

Value VM::getInternal(const Value& obj, const std::string& name, int depth) {
    if (!obj.isObject()) {
        if (obj.isString()) {
            if (name == "length") return Value(static_cast<double>(obj.s.size()));
            return getInternal(Value(stringProto), name, depth);
        }
        if (obj.isNumber() || obj.type == Value::Type::Boolean) return getInternal(Value(objectProto), name, depth);
        return {};
    }
    Object* o = obj.o.get();
    for (int i = 0; o && i < 64; ++i) {
        Value out;
        if (o->getOwn(*this, name, out)) return out;
        if (name == "__proto__") return Value(o->proto);
        o = o->proto.get();
    }
    return {};
}

Value VM::getMember(const Value& obj, const std::string& name) { return getInternal(obj, name, 0); }

void VM::setMember(const Value& obj, const std::string& name, const Value& v) {
    if (!obj.isObject()) return;
    if (name == "__proto__") { obj.o->proto = v.isObject() ? v.o : nullptr; return; }
    obj.o->setOwn(*this, name, v);
}

// "a.b.c", "_root.x", "/a/b:var", "../x", "a:b"
Value VM::resolvePath(Context& ctx, const std::string& path) {
    if (path.empty()) return Value(std::static_pointer_cast<Object>(ctx.target));
    const bool slash = path.find('/') != std::string::npos || path.find(':') != std::string::npos;
    if (!slash) {
        std::size_t start = 0;
        Value cur;
        bool first = true;
        while (start <= path.size()) {
            const auto dot = path.find('.', start);
            const std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (first) {
                cur = getVariable(ctx, part);
                first = false;
            } else {
                cur = getMember(cur, part);
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return cur;
    }
    // Slash syntax: "/" is the root, ".." the parent, names are children.
    Clip* clip = ctx.target ? ctx.target->clip : nullptr;
    std::string p = path;
    std::size_t i = 0;
    if (!p.empty() && p[0] == '/') { clip = player.root.get(); i = 1; }
    while (clip && i < p.size()) {
        const auto sep = p.find_first_of("/:", i);
        const std::string part = p.substr(i, sep == std::string::npos ? std::string::npos : sep - i);
        if (sep != std::string::npos && p[sep] == ':') {
            Value cv(std::static_pointer_cast<Object>(clip->object()));
            if (part.empty()) return getMember(cv, p.substr(sep + 1));
            Value child = getMember(cv, part);
            return getMember(child, p.substr(sep + 1));
        }
        if (part == "..") clip = clip->parent();
        else if (!part.empty() && part != ".") {
            auto* obj = clip->childByName(part);
            clip = obj && obj->clip ? obj->clip.get() : nullptr;
        }
        if (sep == std::string::npos) break;
        i = sep + 1;
    }
    return clip ? Value(std::static_pointer_cast<Object>(clip->object())) : Value();
}

Value VM::getVariable(Context& ctx, const std::string& name) {
    if (name.find_first_of("./:") != std::string::npos && name != "." ) {
        if (name.find('.') != std::string::npos && name.find_first_of("/:") == std::string::npos) return resolvePath(ctx, name);
        return resolvePath(ctx, name);
    }
    if (name == "this") return ctx.thisv;
    if (name == "_global") return Value(global);
    if (name == "_root" || name == "_level0") return Value(std::static_pointer_cast<Object>(rootObject()));
    Value out;
    if (ctx.locals && ctx.locals->getOwn(*this, name, out)) return out;
    for (auto it = ctx.scope.rbegin(); it != ctx.scope.rend(); ++it) {
        if ((*it)->getOwn(*this, name, out)) return out;
    }
    if (ctx.target) {
        Value t(std::static_pointer_cast<Object>(ctx.target));
        Object* o = ctx.target.get();
        for (int i = 0; o && i < 64; ++i, o = o->proto.get()) {
            if (o->getOwn(*this, name, out)) return out;
        }
        (void)t;
    }
    Object* g = global.get();
    for (int i = 0; g && i < 64; ++i, g = g->proto.get()) {
        if (g->getOwn(*this, name, out)) return out;
    }
    return {};
}

void VM::setVariable(Context& ctx, const std::string& name, const Value& v) {
    const auto sep = name.find_last_of(".:");
    if (sep != std::string::npos && name.find('/') == std::string::npos) {
        Value owner = resolvePath(ctx, name.substr(0, sep));
        setMember(owner, name.substr(sep + 1), v);
        return;
    }
    if (name.find_first_of("/:") != std::string::npos) {
        const auto colon = name.rfind(':');
        if (colon != std::string::npos) {
            Value owner = resolvePath(ctx, name.substr(0, colon));
            setMember(owner, name.substr(colon + 1), v);
        }
        return;
    }
    if (ctx.locals && ctx.locals->props.count(name)) { ctx.locals->props[name] = v; return; }
    for (auto it = ctx.scope.rbegin(); it != ctx.scope.rend(); ++it) {
        if ((*it)->props.count(name)) { (*it)->props[name] = v; return; }
    }
    if (ctx.target) ctx.target->setOwn(*this, name, v);
}

// ------------------------------------------------------------------ calls

Value VM::call(const Value& fn, const Value& thisv, std::vector<Value> args) {
    if (!fn.isObject() || !fn.o->callable()) return {};
    return fn.o->call(*this, thisv, args);
}

Value VM::callMethod(const Value& obj, const std::string& name, std::vector<Value> args) {
    const Value fn = getMember(obj, name);
    Value thisv = obj;
    if (obj.isObject()) {
        if (auto* sup = dynamic_cast<SuperObject*>(obj.o.get())) thisv = sup->thisv;
        if (dynamic_cast<ScriptFunction*>(fn.o.get())) {
            // The function's home is the object on the prototype chain that owns it.
            for (Object* o = obj.o.get(); o; o = o->proto.get()) {
                if (o->props.count(name)) { callHome = o->shared_from_this(); break; }
            }
        }
    }
    const Value result = call(fn, thisv, std::move(args));
    callHome = nullptr;
    return result;
}

Value VM::construct(const Value& ctor, std::vector<Value> args) {
    if (!ctor.isObject() || !ctor.o->callable()) return {};
    auto obj = newObject();
    Value proto = getMember(ctor, "prototype");
    if (proto.isObject()) obj->proto = proto.o;
    obj->props["__constructor__"] = ctor;
    callHome = proto.isObject() ? proto.o : nullptr;
    Value result = ctor.o->call(*this, Value(obj), args);
    callHome = nullptr;
    return result.isObject() ? result : Value(obj);
}

// ------------------------------------------------------------------ queue

void VM::queueCode(const Code& code, Clip& target, std::size_t position) {
    Task t;
    t.code = code;
    t.target = target.object();
    position = std::min(position, queue.size());
    queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(position), std::move(t));
}

void VM::queueHandler(Clip& target, const std::string& handler) {
    Task t;
    t.target = target.object();
    t.handler = handler;
    queue.push_back(std::move(t));
}

void VM::queueClassInit(Clip& target) {
    Task t;
    t.target = target.object();
    t.classInit = true;
    queue.push_back(std::move(t));
}

void VM::applyRegisteredClass(Clip& clip) {
    auto* holder = clip.holder();
    if (!holder) return;
    const auto reg = registeredClasses.find(holder->character);
    if (reg == registeredClasses.end()) return;
    const Value ctor = reg->second;
    auto clipObj = clip.object();
    const Value proto = getMember(ctor, "prototype");
    if (proto.isObject()) clipObj->proto = proto.o;
    clipObj->props["__constructor__"] = ctor;
    callHome = proto.isObject() ? proto.o : nullptr;
    call(ctor, Value(std::static_pointer_cast<Object>(clipObj)), {});
    callHome = nullptr;
}

void VM::queueClipEvent(Clip& target, std::uint32_t eventMask) {
    Task t;
    t.target = target.object();
    t.clipEvent = eventMask;
    queue.push_back(std::move(t));
}

void VM::runQueue() {
    for (std::size_t guard = 0; !queue.empty() && guard < 200000; ++guard) {
        Task t = std::move(queue.front());
        queue.pop_front();
        auto target = t.target.lock();
        if (!target || !target->clip) continue;
        Budget budget;
        Budget* previous = gBudget;
        gBudget = &budget;
        try {
            if (t.classInit) {
                applyRegisteredClass(*target->clip);
            } else if (t.code) {
                Context ctx = frameContext(*target->clip);
                run(t.code, 0, t.code->size(), ctx);
            } else if (!t.handler.empty()) {
                Value fn = getMember(Value(std::static_pointer_cast<Object>(target)), t.handler);
                if (fn.isObject() && fn.o->callable()) call(fn, Value(std::static_pointer_cast<Object>(target)), {});
            } else if (t.clipEvent) {
                auto* holder = target->clip->holder();
                if (holder) {
                    const auto actions = holder->clipActions; // copy: handlers may modify the display list
                    for (const auto& ca : actions) {
                        if (!(ca.events & t.clipEvent) || !target->clip) continue;
                        Context ctx = frameContext(*target->clip);
                        run(ca.code, 0, ca.code->size(), ctx);
                    }
                }
            }
        } catch (const std::exception& e) {
            ++errors;
            if (errors < 20) std::cerr << "AVM1 error: " << e.what() << "\n";
        }
        gBudget = previous;
    }
}

void VM::runIntervals(double now) {
    std::vector<int> due;
    for (const auto& [id, iv] : intervals) if (now >= iv.next) due.push_back(id);
    for (int id : due) {
        auto it = intervals.find(id);
        if (it == intervals.end()) continue;
        Interval iv = it->second;
        if (iv.once) intervals.erase(it);
        else it->second.next = now + std::max(iv.period, 1.0);
        Budget budget;
        Budget* previous = gBudget;
        gBudget = &budget;
        try {
            if (!iv.method.empty()) callMethod(iv.thisv, iv.method, iv.args);
            else call(iv.fn, iv.thisv, iv.args);
        } catch (const std::exception& e) {
            ++errors;
            if (errors < 20) std::cerr << "AVM1 error: " << e.what() << "\n";
        }
        gBudget = previous;
    }
}

// ------------------------------------------------------------------ interpreter

namespace {

struct Payload {
    const std::vector<std::uint8_t>& d;
    std::size_t p, end;
    std::uint8_t u8() { if (p >= end) throw std::runtime_error("action payload truncated"); return d[p++]; }
    std::uint16_t u16() { const std::uint16_t lo = u8(); return static_cast<std::uint16_t>(lo | (u8() << 8)); }
    std::uint32_t u32() { const std::uint32_t lo = u16(); return lo | (static_cast<std::uint32_t>(u16()) << 16); }
    std::string str() {
        std::string s;
        while (p < end && d[p]) s.push_back(static_cast<char>(d[p++]));
        if (p < end) ++p;
        return s;
    }
};

const char* kPropertyNames[] = {"_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha",
                                "_visible", "_width", "_height", "_rotation", "_target", "_framesloaded", "_name",
                                "_droptarget", "_url", "_highquality", "_focusrect", "_soundbuftime", "_quality",
                                "_xmouse", "_ymouse"};

} // namespace

Value VM::run(const Code& codePtr, std::size_t start, std::size_t end, Context& ctx) {
    if (!codePtr || ctx.depth > 48) return {};
    Budget local;
    Budget* budget = gBudget ? gBudget : &local;
    const auto& d = *codePtr;
    end = std::min(end, d.size());
    std::vector<Value> stack;
    stack.reserve(16);
    auto pop = [&]() -> Value {
        if (stack.empty()) return {};
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    };
    auto target = [&]() -> Clip* { return ctx.target ? ctx.target->clip : nullptr; };
    std::vector<std::size_t> withEnds;
    const std::size_t baseScope = ctx.scope.size();

    std::size_t pc = start;
    while (pc < end) {
        while (!withEnds.empty() && pc >= withEnds.back()) {
            withEnds.pop_back();
            if (ctx.scope.size() > baseScope) ctx.scope.pop_back();
        }
        if (budget->left-- == 0) throw std::runtime_error("script is taking too long (possible infinite loop)");
        ++instructions;
        const std::uint8_t op = d[pc];
        std::size_t payload = pc + 1, length = 0;
        if (op >= 0x80) {
            if (pc + 3 > end) break;
            length = static_cast<std::size_t>(d[pc + 1] | (d[pc + 2] << 8));
            payload = pc + 3;
        }
        std::size_t next = payload + length;
        if (next > end) break;
        Payload in{d, payload, next};

        switch (op) {
            case 0x00: return {}; // End
            case 0x04: if (auto* c = target()) c->gotoFrame(c->currentFrame() + 1, false); break;
            case 0x05: if (auto* c = target()) c->gotoFrame(c->currentFrame() - 1, false); break;
            case 0x06: if (auto* c = target()) c->play(); break;
            case 0x07: if (auto* c = target()) { if (!player.ignoreStops) c->stop(); } break;
            case 0x08: break; // ToggleQuality
            case 0x09: player.audio.stopAll(); break; // StopSounds
            case 0x0a: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a + b); break; }
            case 0x0b: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a - b); break; }
            case 0x0c: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a * b); break; }
            case 0x0d: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a / b); break; }
            case 0x0e: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a == b); break; }
            case 0x0f: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a < b); break; }
            case 0x10: { const bool b = toBoolean(pop()), a = toBoolean(pop()); stack.emplace_back(a && b); break; }
            case 0x11: { const bool b = toBoolean(pop()), a = toBoolean(pop()); stack.emplace_back(a || b); break; }
            case 0x12: stack.emplace_back(!toBoolean(pop())); break;
            case 0x13: { const auto b = toString(pop()), a = toString(pop()); stack.emplace_back(a == b); break; }
            case 0x14: case 0x31: stack.emplace_back(static_cast<double>(toString(pop()).size())); break;
            case 0x15: case 0x35: { // StringExtract(string, index 1-based, count)
                const int count = toInt32(pop()), index = toInt32(pop());
                const auto s = toString(pop());
                const int from = std::max(0, index - 1);
                stack.emplace_back(from >= static_cast<int>(s.size()) || count <= 0 ? std::string()
                                                                                      : s.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(count)));
                break;
            }
            case 0x17: pop(); break;
            case 0x18: { const double n = toNumber(pop()); stack.emplace_back(std::isnan(n) ? 0.0 : std::trunc(n)); break; }
            case 0x1c: { const auto name = toString(pop()); stack.push_back(getVariable(ctx, name)); break; }
            case 0x1d: { Value v = pop(); const auto name = toString(pop()); setVariable(ctx, name, v); break; }
            case 0x20: { // SetTarget2
                const Value t = pop();
                const std::string path = t.isString() ? t.s : std::string();
                if ((t.isString() && path.empty()) || t.isUndefined()) { ctx.target = ctx.original; break; }
                Clip* c = t.isObject() ? clipOf(t) : clipOf(resolvePath(ctx, path));
                if (c) ctx.target = c->object();
                break;
            }
            case 0x21: { const auto b = toString(pop()), a = toString(pop()); stack.emplace_back(a + b); break; }
            case 0x22: { // GetProperty(target, index)
                const int idx = toInt32(pop());
                Value t = pop();
                Clip* c = t.isString() && t.s.empty() ? target() : clipOf(t.isString() ? resolvePath(ctx, t.s) : t);
                if (c && idx >= 0 && idx < 22) stack.push_back(getMember(Value(std::static_pointer_cast<Object>(c->object())), kPropertyNames[idx]));
                else stack.emplace_back();
                break;
            }
            case 0x23: { // SetProperty(target, index, value)
                Value v = pop();
                const int idx = toInt32(pop());
                Value t = pop();
                Clip* c = t.isString() && t.s.empty() ? target() : clipOf(t.isString() ? resolvePath(ctx, t.s) : t);
                if (c && idx >= 0 && idx < 22) setMember(Value(std::static_pointer_cast<Object>(c->object())), kPropertyNames[idx], v);
                break;
            }
            case 0x24: { // CloneSprite(source, target name, depth)
                const int depth = toInt32(pop());
                const auto name = toString(pop());
                Value src = pop();
                Clip* c = clipOf(src.isString() ? resolvePath(ctx, src.s) : src);
                if (c) callMethod(Value(std::static_pointer_cast<Object>(c->object())), "duplicateMovieClip", {Value(name), Value(depth - 16384)});
                break;
            }
            case 0x25: { // RemoveSprite
                Value t = pop();
                Clip* c = clipOf(t.isString() ? resolvePath(ctx, t.s) : t);
                if (c) callMethod(Value(std::static_pointer_cast<Object>(c->object())), "removeMovieClip", {});
                break;
            }
            case 0x26: std::cout << "trace: " << toString(pop()) << "\n"; break;
            case 0x27: { // StartDrag(target, lockcenter, constrain[, x1 y1 x2 y2])
                pop(); pop();
                if (toBoolean(pop())) { pop(); pop(); pop(); pop(); }
                break;
            }
            case 0x28: break; // EndDrag
            case 0x29: { const auto b = toString(pop()), a = toString(pop()); stack.emplace_back(a < b); break; }
            case 0x2a: throw std::runtime_error("uncaught throw: " + toString(pop()));
            case 0x2b: { Value obj = pop(); pop(); stack.push_back(obj); break; } // CastOp (lenient)
            case 0x2c: { const int n = toInt32(pop()); for (int i = 0; i < n; ++i) pop(); pop(); break; } // ImplementsOp
            case 0x30: { // RandomNumber(max)
                const int max = toInt32(pop());
                stack.emplace_back(max > 0 ? static_cast<double>(std::rand() % max) : 0.0);
                break;
            }
            case 0x32: case 0x36: { const auto s = toString(pop()); stack.emplace_back(s.empty() ? 0.0 : static_cast<double>(static_cast<unsigned char>(s[0]))); break; }
            case 0x33: case 0x37: { const int c = toInt32(pop()); stack.emplace_back(std::string(1, static_cast<char>(c))); break; }
            case 0x34: stack.emplace_back(std::floor(nowMs())); break;
            case 0x3a: { // Delete(object, name)
                const auto name = toString(pop());
                Value obj = pop();
                stack.emplace_back(obj.isObject() ? obj.o->removeOwn(name) : false);
                break;
            }
            case 0x3b: { // Delete2(name)
                const auto name = toString(pop());
                bool removed = false;
                if (ctx.locals && ctx.locals->removeOwn(name)) removed = true;
                else if (ctx.target && ctx.target->removeOwn(name)) removed = true;
                stack.emplace_back(removed);
                break;
            }
            case 0x3c: { // DefineLocal(name, value)
                Value v = pop();
                const auto name = toString(pop());
                if (ctx.locals) ctx.locals->props[name] = v;
                else setVariable(ctx, name, v);
                break;
            }
            case 0x3d: { // CallFunction(name, numArgs, args...)
                const auto name = toString(pop());
                const int n = toInt32(pop());
                std::vector<Value> args;
                for (int i = 0; i < n && !stack.empty(); ++i) args.push_back(pop());
                Value fn = getVariable(ctx, name);
                Value self = ctx.target ? Value(std::static_pointer_cast<Object>(ctx.target)) : Value();
                ++ctx.depth;
                stack.push_back(call(fn, self, std::move(args)));
                --ctx.depth;
                break;
            }
            case 0x3e: return pop(); // Return
            case 0x3f: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(std::fmod(a, b)); break; }
            case 0x40: { // NewObject(name, numArgs, args...)
                const auto name = toString(pop());
                const int n = toInt32(pop());
                std::vector<Value> args;
                for (int i = 0; i < n && !stack.empty(); ++i) args.push_back(pop());
                stack.push_back(construct(getVariable(ctx, name), std::move(args)));
                break;
            }
            case 0x41: { // DefineLocal2(name)
                const auto name = toString(pop());
                if (ctx.locals) { if (!ctx.locals->props.count(name)) ctx.locals->props[name] = Value(); }
                else if (ctx.target && !ctx.target->props.count(name)) ctx.target->props[name] = Value();
                break;
            }
            case 0x42: { // InitArray(count, items...)
                const int n = toInt32(pop());
                std::vector<Value> items;
                for (int i = 0; i < n && !stack.empty(); ++i) items.push_back(pop());
                stack.emplace_back(std::static_pointer_cast<Object>(newArray(std::move(items))));
                break;
            }
            case 0x43: { // InitObject(count, value/name pairs...)
                const int n = toInt32(pop());
                auto obj = newObject();
                for (int i = 0; i < n && stack.size() >= 2; ++i) {
                    Value v = pop();
                    obj->props[toString(pop())] = v;
                }
                stack.emplace_back(obj);
                break;
            }
            case 0x44: stack.emplace_back(typeOf(pop())); break;
            case 0x45: { Clip* c = clipOf(pop()); stack.emplace_back(c ? targetPath(c) : std::string()); break; }
            case 0x46: case 0x55: { // Enumerate(name) / Enumerate2(object)
                Value obj = op == 0x46 ? getVariable(ctx, toString(pop())) : pop();
                stack.push_back(Value::null());
                if (obj.isObject()) {
                    std::vector<std::string> names;
                    obj.o->keys(names);
                    for (auto it = names.rbegin(); it != names.rend(); ++it) stack.emplace_back(*it);
                }
                break;
            }
            case 0x47: { // Add2
                Value b = toPrimitive(pop()), a = toPrimitive(pop());
                if (a.isString() || b.isString()) stack.emplace_back(toString(a) + toString(b));
                else stack.emplace_back(toNumber(a) + toNumber(b));
                break;
            }
            case 0x48: { Value b = pop(), a = pop(); stack.push_back(less(a, b)); break; }
            case 0x49: { Value b = pop(), a = pop(); stack.emplace_back(equals(a, b)); break; }
            case 0x4a: stack.emplace_back(toNumber(pop())); break;
            case 0x4b: stack.emplace_back(toString(pop())); break;
            case 0x4c: stack.push_back(stack.empty() ? Value() : stack.back()); break;
            case 0x4d: if (stack.size() >= 2) std::swap(stack[stack.size() - 1], stack[stack.size() - 2]); break;
            case 0x4e: { // GetMember(object, name)
                const Value name = pop();
                Value obj = pop();
                stack.push_back(getMember(obj, toString(name)));
                break;
            }
            case 0x4f: { // SetMember(object, name, value)
                Value v = pop();
                const auto name = toString(pop());
                Value obj = pop();
                setMember(obj, name, v);
                break;
            }
            case 0x50: stack.emplace_back(toNumber(pop()) + 1); break;
            case 0x51: stack.emplace_back(toNumber(pop()) - 1); break;
            case 0x52: { // CallMethod(name, object, numArgs, args...)
                const Value nameV = pop();
                Value obj = pop();
                const int n = toInt32(pop());
                std::vector<Value> args;
                for (int i = 0; i < n && !stack.empty(); ++i) args.push_back(pop());
                ++ctx.depth;
                if (nameV.isNullish() || (nameV.isString() && nameV.s.empty())) stack.push_back(call(obj, Value(), std::move(args)));
                else {
                    // Debug aid: FP_TRACE_CALL=getViewX logs every call of that method and its result.
                    static const char* traceCall = std::getenv("FP_TRACE_CALL");
                    const auto name = toString(nameV);
                    Value result = callMethod(obj, name, std::move(args));
                    if (traceCall && name == traceCall) {
                        std::fprintf(stderr, "[call] %s() on %s -> %s\n", name.c_str(), obj.isObject() ? "object" : toString(obj).c_str(),
                                     toString(result).c_str());
                        if (obj.isObject()) {
                            for (auto& [k, v] : obj.o->props) std::fprintf(stderr, "    %s = %s\n", k.c_str(), toString(v).c_str());
                        }
                    }
                    stack.push_back(std::move(result));
                }
                --ctx.depth;
                break;
            }
            case 0x53: { // NewMethod(name, object, numArgs, args...)
                const Value nameV = pop();
                Value obj = pop();
                const int n = toInt32(pop());
                std::vector<Value> args;
                for (int i = 0; i < n && !stack.empty(); ++i) args.push_back(pop());
                Value ctor = nameV.isNullish() || (nameV.isString() && nameV.s.empty()) ? obj : getMember(obj, toString(nameV));
                stack.push_back(construct(ctor, std::move(args)));
                break;
            }
            case 0x54: { // InstanceOf(object, constructor)
                Value ctor = pop(), obj = pop();
                bool result = false;
                Value proto = getMember(ctor, "prototype");
                if (obj.isObject() && proto.isObject()) {
                    for (Object* o = obj.o->proto.get(); o; o = o->proto.get()) if (o == proto.o.get()) { result = true; break; }
                }
                stack.emplace_back(result);
                break;
            }
            case 0x60: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a & b)); break; }
            case 0x61: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a | b)); break; }
            case 0x62: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a ^ b)); break; }
            case 0x63: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(a) << (b & 31)))); break; }
            case 0x64: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a >> (b & 31))); break; }
            case 0x65: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(static_cast<std::uint32_t>(a) >> (b & 31))); break; }
            case 0x66: { Value b = pop(), a = pop(); stack.emplace_back(strictEquals(a, b)); break; }
            case 0x67: { Value b = pop(), a = pop(); stack.push_back(less(b, a)); break; } // Greater
            case 0x68: { const auto b = toString(pop()), a = toString(pop()); stack.emplace_back(a > b); break; }
            case 0x69: { // Extends(superclass, subclass)
                Value super = pop(), sub = pop();
                if (sub.isObject() && super.isObject()) {
                    auto proto = newObject();
                    Value sp = getMember(super, "prototype");
                    proto->proto = sp.isObject() ? sp.o : objectProto;
                    proto->props["__constructor__"] = super;
                    sub.o->props["prototype"] = Value(proto);
                }
                break;
            }
            // ---- long actions
            case 0x81: if (auto* c = target()) { c->gotoFrame(in.u16(), false); } break; // GotoFrame (stops)
            case 0x83: break;                                                             // GetURL: no browser
            case 0x87: { // StoreRegister
                const auto r = in.u8();
                if (r < ctx.registers.size()) ctx.registers[r] = stack.empty() ? Value() : stack.back();
                break;
            }
            case 0x88: { // ConstantPool
                auto pool = std::make_shared<std::vector<std::string>>();
                const auto n = in.u16();
                for (std::uint16_t i = 0; i < n && in.p < in.end; ++i) pool->push_back(in.str());
                ctx.pool = pool;
                break;
            }
            case 0x8a: in.u16(); in.u8(); break;           // WaitForFrame: everything is loaded
            case 0x8b: { // SetTarget
                const auto path = in.str();
                if (path.empty()) ctx.target = ctx.original;
                else if (Clip* c = clipOf(resolvePath(ctx, path))) ctx.target = c->object();
                break;
            }
            case 0x8c: { // GoToLabel (stops)
                const auto label = in.str();
                if (auto* c = target()) {
                    const int f = c->frameForLabel(label);
                    if (f >= 0) c->gotoFrame(f, false);
                }
                break;
            }
            case 0x8d: pop(); in.u8(); break;              // WaitForFrame2
            case 0x8e: case 0x9b: { // DefineFunction2 / DefineFunction
                auto fn = std::make_shared<ScriptFunction>();
                fn->proto = functionProto;
                fn->v2 = op == 0x8e;
                fn->name = in.str();
                const auto numParams = in.u16();
                if (fn->v2) {
                    fn->registerCount = in.u8();
                    fn->flags = in.u16();
                    for (std::uint16_t i = 0; i < numParams; ++i) {
                        const auto reg = in.u8();
                        fn->params.emplace_back(reg, in.str());
                    }
                } else {
                    for (std::uint16_t i = 0; i < numParams; ++i) fn->params.emplace_back(0, in.str());
                }
                const auto codeSize = in.u16();
                fn->code = codePtr;
                fn->start = next;
                fn->end = std::min(end, next + codeSize);
                fn->scope = ctx.scope;
                if (ctx.locals) fn->scope.push_back(ctx.locals);
                fn->base = ctx.target;
                fn->pool = ctx.pool;
                auto proto = newObject();
                proto->props["constructor"] = Value(std::static_pointer_cast<Object>(fn));
                fn->props["prototype"] = Value(proto);
                next = fn->end;
                if (fn->name.empty()) stack.emplace_back(std::static_pointer_cast<Object>(fn));
                else if (ctx.locals) ctx.locals->props[fn->name] = Value(std::static_pointer_cast<Object>(fn));
                else setVariable(ctx, fn->name, Value(std::static_pointer_cast<Object>(fn)));
                break;
            }
            case 0x8f: { // Try: run the try block; catch/finally blocks run only via fallthrough
                const auto flags = in.u8();
                const auto trySize = in.u16(), catchSize = in.u16(), finallySize = in.u16();
                if (flags & 0x04) in.u8(); else in.str();
                const std::size_t tryStart = next, catchStart = tryStart + trySize, finallyStart = catchStart + catchSize;
                try {
                    run(codePtr, tryStart, catchStart, ctx);
                } catch (const std::exception&) {
                    if (flags & 0x01) run(codePtr, catchStart, finallyStart, ctx);
                }
                if (flags & 0x02) run(codePtr, finallyStart, finallyStart + finallySize, ctx);
                next = finallyStart + finallySize;
                break;
            }
            case 0x94: { // With(object) { body }
                const auto size = in.u16();
                Value obj = pop();
                if (obj.isObject()) {
                    ctx.scope.push_back(obj.o);
                    withEnds.push_back(next + size);
                }
                break;
            }
            case 0x96: { // Push
                while (in.p < in.end) {
                    const auto type = in.u8();
                    switch (type) {
                        case 0: stack.emplace_back(in.str()); break;
                        case 1: { const auto raw = in.u32(); float f; std::memcpy(&f, &raw, 4); stack.emplace_back(static_cast<double>(f)); break; }
                        case 2: stack.push_back(Value::null()); break;
                        case 3: stack.emplace_back(); break;
                        case 4: { const auto r = in.u8(); stack.push_back(r < ctx.registers.size() ? ctx.registers[r] : Value()); break; }
                        case 5: stack.emplace_back(in.u8() != 0); break;
                        case 6: {
                            const std::uint64_t hi = in.u32(), lo = in.u32();
                            const std::uint64_t raw = (hi << 32) | lo;
                            double v;
                            std::memcpy(&v, &raw, 8);
                            stack.emplace_back(v);
                            break;
                        }
                        case 7: stack.emplace_back(static_cast<double>(static_cast<std::int32_t>(in.u32()))); break;
                        case 8: case 9: {
                            const std::size_t idx = type == 8 ? in.u8() : in.u16();
                            stack.emplace_back(ctx.pool && idx < ctx.pool->size() ? Value((*ctx.pool)[idx]) : Value());
                            break;
                        }
                        default: throw std::runtime_error("bad push type");
                    }
                }
                break;
            }
            case 0x99: { const auto off = static_cast<std::int16_t>(in.u16()); next = static_cast<std::size_t>(static_cast<std::int64_t>(next) + off); break; }
            case 0x9a: { in.u8(); pop(); pop(); break; } // GetURL2
            case 0x9d: { // If
                const auto off = static_cast<std::int16_t>(in.u16());
                if (toBoolean(pop())) next = static_cast<std::size_t>(static_cast<std::int64_t>(next) + off);
                break;
            }
            case 0x9e: { // Call: run a frame's actions
                Value f = pop();
                if (auto* c = target()) {
                    int frame = f.isString() ? c->frameForLabel(f.s) : toInt32(f) - 1;
                    if (f.isString() && frame < 0) frame = toInt32(f) - 1;
                    (void)frame; // executing another frame's scripts inline is rarely used; ignored
                }
                break;
            }
            case 0x9f: { // GotoFrame2(frame on stack)
                const auto flags = in.u8();
                const int bias = (flags & 0x02) ? in.u16() : 0;
                Value f = pop();
                Clip* c = target();
                if (f.isString()) {
                    // "path:frame" targets another clip.
                    const auto colon = f.s.rfind(':');
                    if (colon != std::string::npos) {
                        c = clipOf(resolvePath(ctx, f.s.substr(0, colon)));
                        f = Value(f.s.substr(colon + 1));
                    }
                }
                if (c) {
                    if (f.isString()) {
                        int frame = c->frameForLabel(f.s);
                        if (frame < 0) { const double n = toNumber(f); if (!std::isnan(n)) frame = static_cast<int>(n) - 1; }
                        if (frame >= 0) c->gotoFrame(frame + bias, (flags & 1) != 0);
                    } else {
                        c->gotoFrame(toInt32(f) - 1 + bias, (flags & 1) != 0);
                    }
                }
                break;
            }
            default:
                break; // unknown or unsupported actions are skipped
        }
        pc = next;
    }
    return {};
}

} // namespace fp::avm1

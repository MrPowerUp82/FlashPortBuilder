#include "AVM2.hpp"
#include <algorithm>
#include <exception>
#include <exception>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>

namespace fp::avm2 {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint64_t kInstructionBudget = 200'000'000;
thread_local std::uint64_t gBudget = kInstructionBudget;
thread_local int gDepth = 0;

bool nsMatches(const Namespace& traitNs, const std::vector<Namespace>& nss) {
    if (nss.empty()) return traitNs.kind == NsKind::Public && traitNs.uri.empty();
    for (const auto& n : nss) if (n == traitNs) return true;
    return false;
}

bool arrayIndex(const std::string& s, std::size_t& out) {
    if (s.empty() || s.size() > 10) return false;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (s.size() > 1 && s[0] == '0') return false;
    if (v >= 0xffffffffULL) return false;
    out = static_cast<std::size_t>(v);
    return true;
}

bool numberIndex(const Value& v, std::size_t& out) {
    if (!v.isNumber()) return false;
    if (v.n < 0 || v.n >= 4294967295.0 || v.n != std::floor(v.n)) return false;
    out = static_cast<std::size_t>(v.n);
    return true;
}

} // namespace

// ------------------------------------------------------------------ traits

const Trait* TraitTable::find(const Multiname& mn) const {
    auto range = byName.equal_range(mn.name);
    const Trait* best = nullptr;
    for (auto it = range.first; it != range.second; ++it) {
        const Trait& t = traits[it->second];
        if (nsMatches(t.ns, mn.nss)) {
            best = &t;
            break;
        }
    }
    if (!best) {
        // Interface methods are named in the interface's own namespace ("pkg:IFoo"); an implementing
        // class provides them under the same local name in its public namespace.
        for (const auto& ns : mn.nss) {
            // ("IFoo" for interfaces in the unnamed package).
            if (ns.kind == NsKind::Private || ns.uri.empty() || ns.uri.compare(0, 7, "http://") == 0) continue;
            for (auto it = range.first; it != range.second; ++it) {
                const Trait& t = traits[it->second];
                if (t.ns.kind == NsKind::Public && t.ns.uri.empty() && (t.kind == Trait::Kind::Method || t.kind == Trait::Kind::Accessor)) return &t;
            }
        }
    }
    return best;
}

const Trait* TraitTable::findName(const std::string& name, const Namespace& ns) const {
    auto range = byName.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        if (traits[it->second].ns == ns) return &traits[it->second];
    }
    return nullptr;
}

const Trait* TraitTable::findPublic(const std::string& name) const {
    auto range = byName.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        if (traits[it->second].ns.kind == NsKind::Public) return &traits[it->second];
    }
    return nullptr;
}

Trait* TraitTable::findExact(const std::string& name, const Namespace& ns) {
    auto range = byName.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) {
        if (traits[it->second].ns == ns) return &traits[it->second];
    }
    return nullptr;
}

Trait& TraitTable::add(Trait t) {
    if (Trait* existing = findExact(t.name, t.ns)) {
        if (t.kind == Trait::Kind::Accessor && existing->kind == Trait::Kind::Accessor) {
            if (t.getter.valid()) existing->getter = t.getter;
            if (t.setter.valid()) existing->setter = t.setter;
            existing->declaring = t.declaring;
            return *existing;
        }
        if (existing->kind == Trait::Kind::Slot || existing->kind == Trait::Kind::Class) t.slot = existing->slot;
        *existing = std::move(t);
        return *existing;
    }
    const auto idx = static_cast<std::uint32_t>(traits.size());
    byName.emplace(t.name, idx);
    traits.push_back(std::move(t));
    return traits.back();
}

bool Class::isSubclassOf(const Class* other) const {
    for (const Class* c = this; c; c = c->super.get()) {
        if (c == other) return true;
        for (const Class* i : c->interfaces) if (i == other) return true;
    }
    return false;
}

int DictionaryObject::indexOf(VM& vm, const Value& key) const {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& k = entries[i].first;
        if (k.isObject() || key.isObject() ? (k.isObject() && key.isObject() && k.o == key.o) : vm.strictEquals(k, key)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ------------------------------------------------------------------ VM setup

VM::VM(Player& p) : player(p) {
    toplevelTraits_ = std::make_shared<TraitTable>();
    toplevel = std::make_shared<Object>();
    toplevel->traits = toplevelTraits_;
    installBuiltins();
    installFlash();
}

VM::~VM() = default;

void VM::installBuiltins() { installBuiltinsImpl(*this); }
void VM::installFlash() { installFlashImpl(*this); }

double VM::nowMs() const { return player.timeMs(); }

void VM::warnOnce(const std::string& msg) {
    if (warned.size() < 400 && warned.insert(msg).second) std::cerr << "AVM2: " << msg << "\n";
}

ClassPtr VM::defineNativeClass(const std::string& pkg, const std::string& name, const ClassPtr& super) {
    auto c = std::make_shared<Class>();
    c->name = name;
    c->ns = Namespace::pub(pkg);
    c->super = super;
    c->instanceTraits = std::make_shared<TraitTable>();
    if (super) *c->instanceTraits = *super->instanceTraits;
    c->staticTraits = std::make_shared<TraitTable>();
    c->traits = c->staticTraits;
    c->prototype = std::make_shared<Object>();
    if (super && super->prototype) c->prototype->proto = super->prototype;
    c->dynamic.set("prototype", Value(c->prototype));
    if (classClass) { c->cls = classClass; c->proto = classClass->prototype; }
    defineGlobal(pkg, name, Value(c));
    return c;
}

void VM::defineGlobal(const std::string& pkg, const std::string& name, const Value& v) {
    Trait t;
    t.ns = Namespace::pub(pkg);
    t.name = name;
    t.kind = Trait::Kind::Slot;
    t.isConst = true;
    t.slot = toplevelTraits_->slotCount++;
    toplevelTraits_->add(t);
    toplevel->slots.resize(toplevelTraits_->slotCount);
    toplevel->slots[t.slot] = v;
    Definition d;
    d.ns = t.ns;
    d.value = v;
    definitions_.emplace(name, d);
}

void VM::defineGlobalFunction(const std::string& pkg, const std::string& name, NativeFn fn) {
    defineGlobal(pkg, name, Value(newFunction(std::move(fn))));
}

ClassBuilder& ClassBuilder::method(const std::string& name, NativeFn fn) {
    Trait t;
    t.ns = Namespace::pub();
    t.name = name;
    t.kind = Trait::Kind::Method;
    t.method.native = std::move(fn);
    t.declaring = c.get();
    c->instanceTraits->add(std::move(t));
    return *this;
}

ClassBuilder& ClassBuilder::getter(const std::string& name, NativeFn fn) {
    Trait t;
    t.ns = Namespace::pub();
    t.name = name;
    t.kind = Trait::Kind::Accessor;
    t.getter.native = std::move(fn);
    t.declaring = c.get();
    c->instanceTraits->add(std::move(t));
    return *this;
}

ClassBuilder& ClassBuilder::setter(const std::string& name, NativeFn fn) {
    Trait t;
    t.ns = Namespace::pub();
    t.name = name;
    t.kind = Trait::Kind::Accessor;
    t.setter.native = std::move(fn);
    t.declaring = c.get();
    c->instanceTraits->add(std::move(t));
    return *this;
}

ClassBuilder& ClassBuilder::staticMethod(const std::string& name, NativeFn fn) {
    Trait t;
    t.ns = Namespace::pub();
    t.name = name;
    t.kind = Trait::Kind::Method;
    t.method.native = std::move(fn);
    c->staticTraits->add(std::move(t));
    return *this;
}

ClassBuilder& ClassBuilder::staticGetter(const std::string& name, NativeFn fn) {
    Trait t;
    t.ns = Namespace::pub();
    t.name = name;
    t.kind = Trait::Kind::Accessor;
    t.getter.native = std::move(fn);
    c->staticTraits->add(std::move(t));
    return *this;
}

ClassBuilder& ClassBuilder::constant(const std::string& name, const Value& v) {
    Trait t;
    t.ns = Namespace::pub();
    t.name = name;
    t.kind = Trait::Kind::Slot;
    t.isConst = true;
    t.slot = c->staticTraits->slotCount++;
    c->staticTraits->add(t);
    c->slots.resize(c->staticTraits->slotCount);
    c->slots[t.slot] = v;
    return *this;
}

ObjectPtr VM::newObject() {
    auto o = std::make_shared<Object>();
    o->cls = objectClass;
    if (objectClass) { o->traits = objectClass->instanceTraits; o->proto = objectClass->prototype; }
    return o;
}

std::shared_ptr<ArrayObject> VM::newArray(std::vector<Value> items) {
    auto a = std::make_shared<ArrayObject>();
    a->cls = arrayClass;
    if (arrayClass) { a->traits = arrayClass->instanceTraits; a->proto = arrayClass->prototype; }
    a->items = std::move(items);
    return a;
}

ObjectPtr VM::newFunction(NativeFn fn) {
    auto f = std::make_shared<FunctionObject>();
    f->cls = functionClass;
    if (functionClass) { f->traits = functionClass->instanceTraits; f->proto = functionClass->prototype; }
    f->method.native = std::move(fn);
    f->dynamic.set("prototype", Value(newObject()));
    return f;
}

Value VM::newError(const std::string& clsName, const std::string& message) {
    ClassPtr c = findClass(clsName);
    if (!c) c = errorClass;
    Args args{Value(message)};
    return construct(Value(c), args);
}

void VM::throwError(const std::string& cls, const std::string& message) {
    throw ScriptException{newError(cls, message)};
}

// ------------------------------------------------------------------ loading

void VM::loadAbc(const Code& bytes, const std::string& name, int domain) {
    auto abc = parseAbc(*bytes, name);
    abc->domain = domain;
    for (std::uint32_t i = 0; i < abc->scripts.size(); ++i) {
        const auto scriptIndex = static_cast<int>(scripts_.size());
        ScriptEntry e;
        e.abc = abc.get();
        e.index = i;
        scripts_.push_back(e);
        for (const auto& ti : abc->scripts[i].second) {
            const auto& mn = abc->multinames[ti.name];
            Definition d;
            d.ns = mn.nss.empty() ? Namespace::pub() : mn.nss[0];
            d.script = scriptIndex;
            d.domain = domain;
            definitions_.emplace(mn.name, d);
        }
    }
    abcs_.push_back(std::move(abc));
}

void VM::initScript(std::size_t index) {
    auto& e = scripts_[index];
    if (e.initialized) return;
    e.initialized = true;
    auto& abc = *e.abc;
    auto traits = std::make_shared<TraitTable>();
    buildTraits(abc, abc.scripts[e.index].second, *traits, nullptr);
    e.global = std::make_shared<Object>();
    e.global->traits = traits;
    e.global->cls = objectClass;
    e.global->proto = objectClass ? objectClass->prototype : nullptr;
    initSlots(*e.global);
    const auto& info = abc.methods[abc.scripts[e.index].first];
    std::vector<ObjectPtr> scope{e.global};
    Args none;
    try {
        if (info.body >= 0) execute(abc, abc.bodies[static_cast<std::size_t>(info.body)], Value(e.global), none, &scope, nullptr, info);
    } catch (const ScriptException& ex) {
        reportError("script init", ex);
    }
}

ObjectPtr VM::findDefinition(const Multiname& mn, bool strict, int domain) {
    if (domain < 0) domain = currentDomain_;
    auto range = definitions_.equal_range(mn.name);
    // Parent (domain 0: runtime + entry SWF) first, then the caller's own domain.
    for (int pass = 0; pass < 2; ++pass) {
        const int want = pass == 0 ? 0 : domain;
        if (pass == 1 && domain == 0) break;
        const Definition* found = nullptr;
        std::size_t foundScript = 0;
        for (auto it = range.first; it != range.second; ++it) {
            const Definition& d = it->second;
            if (d.domain != want || !nsMatches(d.ns, mn.nss)) continue;
            // unordered_multimap order is unspecified: the lowest script index (load order) wins.
            if (!found || (d.script >= 0 && static_cast<std::size_t>(d.script) < foundScript)) {
                found = &d;
                foundScript = d.script >= 0 ? static_cast<std::size_t>(d.script) : 0;
            }
        }
        if (!found) continue;
        if (found->script >= 0) {
            initScript(static_cast<std::size_t>(found->script));
            return scripts_[static_cast<std::size_t>(found->script)].global;
        }
        return toplevel;
    }
    (void)strict;
    return nullptr;
}

ClassPtr VM::findClassIn(const std::string& qualifiedName, int domain) {
    const int saved = currentDomain_;
    currentDomain_ = domain;
    auto c = findClass(qualifiedName);
    currentDomain_ = saved;
    return c;
}

ClassPtr VM::findClass(const std::string& qualifiedName) {
    Multiname mn;
    const auto sep = qualifiedName.rfind("::");
    const auto dot = qualifiedName.rfind('.');
    if (sep != std::string::npos) {
        mn.name = qualifiedName.substr(sep + 2);
        mn.nss.push_back(Namespace::pub(qualifiedName.substr(0, sep)));
    } else if (dot != std::string::npos) {
        mn.name = qualifiedName.substr(dot + 1);
        mn.nss.push_back(Namespace::pub(qualifiedName.substr(0, dot)));
    } else {
        mn.name = qualifiedName;
        mn.nss.push_back(Namespace::pub());
    }
    auto holder = findDefinition(mn, false);
    if (!holder) return nullptr;
    Value v = getProperty(Value(holder), mn);
    return v.isObject() ? std::dynamic_pointer_cast<Class>(v.o) : nullptr;
}

Value VM::defaultForType(const std::string& t) {
    if (t == "int" || t == "uint") return Value(0);
    if (t == "Number") return Value(kNaN);
    if (t == "Boolean") return Value(false);
    if (t.empty() || t == "*") return Value();
    return Value::null();
}

void VM::buildTraits(AbcFile& abc, const std::vector<TraitInfo>& infos, TraitTable& table, Class* declaring) {
    for (const auto& ti : infos) {
        const auto& mn = abc.multinames[ti.name];
        Trait t;
        t.name = mn.name;
        t.ns = mn.nss.empty() ? Namespace::pub() : mn.nss[0];
        t.declaring = declaring;
        switch (ti.kind) {
            case 0: case 6: {
                t.kind = Trait::Kind::Slot;
                t.isConst = ti.kind == 6;
                t.typeName = ti.typeName < abc.multinames.size() ? abc.multinames[ti.typeName].name : std::string();
                if (ti.typeName == 0) t.typeName.clear();
                if (ti.vindex) {
                    t.hasInitial = true;
                    switch (ti.vkind) {
                        case 0x03: t.initial = Value(static_cast<double>(abc.ints[ti.vindex])); break;
                        case 0x04: t.initial = Value(static_cast<double>(abc.uints[ti.vindex])); break;
                        case 0x06: t.initial = Value(abc.doubles[ti.vindex]); break;
                        case 0x02: t.initial = Value(ti.vindex < abc.floats.size() ? abc.floats[ti.vindex] : 0.0); break;
                        case 0x01: t.initial = Value(abc.strings[ti.vindex]); break;
                        case 0x0b: t.initial = Value(true); break;
                        case 0x0a: t.initial = Value(false); break;
                        case 0x0c: t.initial = Value::null(); break;
                        case 0x00: t.initial = Value(); break;
                        default: t.initial = Value(abc.namespaces[ti.vindex].uri); break;
                    }
                } else {
                    t.initial = defaultForType(t.typeName);
                    t.hasInitial = true;
                }
                break;
            }
            case 1: t.kind = Trait::Kind::Method; t.method.abc = &abc; t.method.index = ti.index; break;
            case 2: t.kind = Trait::Kind::Accessor; t.getter.abc = &abc; t.getter.index = ti.index; break;
            case 3: t.kind = Trait::Kind::Accessor; t.setter.abc = &abc; t.setter.index = ti.index; break;
            case 4: t.kind = Trait::Kind::Class; t.classIndex = static_cast<int>(ti.index); t.initial = Value::null(); t.hasInitial = true; break;
            case 5: t.kind = Trait::Kind::Slot; t.method.abc = &abc; t.method.index = ti.index; break; // function slot
            default: continue;
        }
        if (t.kind == Trait::Kind::Slot || t.kind == Trait::Kind::Class) {
            // Explicit slot ids matter for getslot/setslot (activations, globals, catch scopes).
            if (ti.slotId && !declaring) {
                t.slot = ti.slotId - 1;
                table.slotCount = std::max(table.slotCount, ti.slotId);
                Trait& added = table.add(t);
                added.slot = ti.slotId - 1;
            } else if (!table.findExact(t.name, t.ns)) {
                t.slot = table.slotCount++;
                table.add(t);
            } else {
                table.add(t);
            }
        } else {
            table.add(t);
        }
    }
}

void VM::initSlots(Object& o) {
    if (!o.traits) return;
    o.slots.assign(o.traits->slotCount, Value());
    for (const auto& t : o.traits->traits) {
        if ((t.kind == Trait::Kind::Slot || t.kind == Trait::Kind::Class) && t.slot < o.slots.size() && t.hasInitial) {
            o.slots[t.slot] = t.initial;
        }
        if (t.kind == Trait::Kind::Slot && t.method.abc && t.slot < o.slots.size()) { // function trait
            auto f = std::make_shared<FunctionObject>();
            f->cls = functionClass;
            f->proto = functionClass ? functionClass->prototype : nullptr;
            f->method = t.method;
            o.slots[t.slot] = Value(f);
        }
    }
}

ClassPtr VM::newClass(AbcFile& abc, std::uint32_t index, const ClassPtr& base, const std::vector<ObjectPtr>& scope) {
    const auto& inst = abc.instances[index];
    auto c = std::make_shared<Class>();
    const auto& mn = abc.multinames[inst.name];
    c->name = mn.name;
    c->ns = mn.nss.empty() ? Namespace::pub() : mn.nss[0];
    c->super = base;
    c->abc = &abc;
    c->index = static_cast<int>(index);
    c->sealed = (inst.flags & 0x01) != 0;
    c->isInterface = (inst.flags & 0x04) != 0;
    c->cls = classClass;
    c->proto = classClass ? classClass->prototype : nullptr;
    for (auto i : inst.interfaces) {
        const auto& imn = abc.multinames[i];
        if (auto holder = findDefinition(imn, false)) {
            Value iv = getProperty(Value(holder), imn);
            if (auto ic = iv.isObject() ? std::dynamic_pointer_cast<Class>(iv.o) : nullptr) c->interfaces.push_back(ic.get());
        }
    }
    c->instanceTraits = std::make_shared<TraitTable>();
    if (base) *c->instanceTraits = *base->instanceTraits;
    buildTraits(abc, inst.traits, *c->instanceTraits, c.get());
    c->staticTraits = std::make_shared<TraitTable>();
    buildTraits(abc, abc.classes[index].second, *c->staticTraits, c.get());
    c->traits = c->staticTraits;
    initSlots(*c);
    c->prototype = std::make_shared<Object>();
    c->prototype->cls = objectClass;
    c->prototype->proto = base ? base->prototype : (objectClass ? objectClass->prototype : nullptr);
    c->prototype->dynamic.set("constructor", Value(c));
    c->dynamic.set("prototype", Value(c->prototype));
    c->iinit.abc = &abc;
    c->iinit.index = inst.iinit;
    c->scope = scope;
    c->scope.push_back(c);
    abc.classObjects[index] = c;
    // Static initialiser runs with the class on the scope chain.
    const auto& cinfo = abc.methods[abc.classes[index].first];
    if (cinfo.body >= 0) {
        Args none;
        try {
            execute(abc, abc.bodies[static_cast<std::size_t>(cinfo.body)], Value(c), none, &c->scope, c.get(), cinfo);
        } catch (const ScriptException& ex) {
            reportError("class init", ex);
        }
    }
    return c;
}

ObjectPtr VM::createInstance(const ClassPtr& cls) {
    ObjectPtr o;
    for (Class* c = cls.get(); c && !o; c = c->super.get()) if (c->allocator) o = c->allocator(*this);
    if (!o) o = std::make_shared<Object>();
    o->cls = cls;
    o->traits = cls->instanceTraits;
    o->proto = cls->prototype;
    o->isDynamic = !cls->sealed;
    initSlots(*o);
    for (Class* c = cls.get(); c; c = c->super.get()) {
        if (c->nativeInit) { c->nativeInit(*this, *o); break; }
    }
    return o;
}

void VM::runConstructor(const ClassPtr& cls, const ObjectPtr& obj, Args& args) {
    if (!cls) return;
    if (cls->iinit.native) { cls->iinit.native(*this, Value(obj), args); return; }
    if (cls->iinit.abc) { invoke(cls->iinit, Value(obj), args, &cls->scope, cls.get()); return; }
    if (cls->super) runConstructor(cls->super, obj, args); // native class without a constructor body
}

Value VM::construct(const Value& ctor, Args args) {
    if (!ctor.isObject()) throwError("TypeError", "Instantiation attempted on a non-constructor.");
    if (auto cls = std::dynamic_pointer_cast<Class>(ctor.o)) {
        if (cls->isInterface) throwError("TypeError", "Cannot instantiate interface " + cls->name);
        auto obj = createInstance(cls);
        runConstructor(cls, obj, args);
        return Value(obj);
    }
    if (auto fn = std::dynamic_pointer_cast<FunctionObject>(ctor.o)) {
        auto obj = newObject();
        Value p = getPublic(ctor, "prototype");
        if (p.isObject()) obj->proto = p.o;
        Value r = call(ctor, Value(obj), std::move(args));
        return r.isObject() ? r : Value(obj);
    }
    throwError("TypeError", "Instantiation attempted on a non-constructor.");
}

Value VM::invoke(const MethodRef& m, const Value& thisv, Args& args, const std::vector<ObjectPtr>* scope, Class* declaring) {
    if (m.native) return m.native(*this, thisv, args);
    if (!m.abc || m.index >= m.abc->methods.size()) return {};
    const auto& info = m.abc->methods[m.index];
    if (info.body < 0) return {}; // native/interface method without body
    return execute(*m.abc, m.abc->bodies[static_cast<std::size_t>(info.body)], thisv, args, scope, declaring, info);
}

Value VM::call(const Value& fn, const Value& thisv, Args args) {
    if (!fn.isObject()) throwError("TypeError", "value is not a function");
    if (auto f = std::dynamic_pointer_cast<FunctionObject>(fn.o)) {
        const Value self = f->isMethodClosure ? f->boundThis : thisv;
        return invoke(f->method, self, args, &f->scope, f->declaring);
    }
    if (auto c = std::dynamic_pointer_cast<Class>(fn.o)) {
        if (c->callAsFunction) return c->callAsFunction(*this, args);
        // Class(value) is a cast.
        return args.empty() ? Value() : args[0];
    }
    throwError("TypeError", "value is not a function");
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
            Value p = toPrimitive(v, true);
            if (p.isObject()) return "[object Object]";
            return toString(p);
        }
    }
    return "";
}

Value VM::toPrimitive(const Value& v, bool preferString) {
    if (!v.isObject()) return v;
    {
        Value native;
        if (v.o->nativePrimitive(*this, native)) return native;
    }
    if (auto a = std::dynamic_pointer_cast<ArrayObject>(v.o)) {
        std::string out;
        for (std::size_t i = 0; i < a->items.size(); ++i) {
            if (i) out += ",";
            if (!a->items[i].isNullish()) out += toString(a->items[i]);
        }
        return Value(out);
    }
    if (auto c = std::dynamic_pointer_cast<Class>(v.o)) return Value("[class " + c->name + "]");
    if (v.o->isFunction()) return Value(std::string("function Function() {}"));
    const char* order[2] = {preferString ? "toString" : "valueOf", preferString ? "valueOf" : "toString"};
    for (const char* name : order) {
        Value fn;
        try { fn = getPublic(v, name); } catch (const ScriptException&) { continue; }
        if (fn.isObject() && fn.o->isFunction()) {
            Value r = call(fn, v, {});
            if (!r.isObject()) return r;
        }
    }
    const std::string cname = v.o->cls ? v.o->cls->name : "Object";
    return Value("[object " + cname + "]");
}

double VM::toNumber(const Value& v) {
    switch (v.type) {
        case Value::Type::Undefined: return kNaN;
        case Value::Type::Null: return 0;
        case Value::Type::Boolean: return v.b ? 1 : 0;
        case Value::Type::Number: return v.n;
        case Value::Type::String: {
            const char* s = v.s.c_str();
            while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
            if (!*s) return 0;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return static_cast<double>(std::strtoll(s + 2, nullptr, 16));
            char* end = nullptr;
            const double d = std::strtod(s, &end);
            while (end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
            if (end == s || (end && *end)) return kNaN;
            return d;
        }
        case Value::Type::Object: return toNumber(toPrimitive(v));
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
    const double d = v.isNumber() ? v.n : toNumber(v);
    if (std::isnan(d) || std::isinf(d)) return 0;
    if (d >= -2147483648.0 && d <= 2147483647.0) return static_cast<std::int32_t>(d);
    const double m = std::fmod(std::trunc(d), 4294967296.0);
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(m < 0 ? m + 4294967296.0 : m)));
}

std::uint32_t VM::toUint32(const Value& v) { return static_cast<std::uint32_t>(toInt32(v)); }

bool VM::strictEquals(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case Value::Type::Undefined: case Value::Type::Null: return true;
        case Value::Type::Boolean: return a.b == b.b;
        case Value::Type::Number: return a.n == b.n;
        case Value::Type::String: return a.s == b.s;
        case Value::Type::Object: return a.o == b.o || sameFunction(a, b);
    }
    return false;
}

bool VM::equals(const Value& a, const Value& b) {
    if (a.type == b.type) return strictEquals(a, b);
    if (a.isNullish() && b.isNullish()) return true;
    if (a.isNullish() || b.isNullish()) return false;
    if (a.isNumber() && b.isString()) return a.n == toNumber(b);
    if (a.isString() && b.isNumber()) return toNumber(a) == b.n;
    if (a.isBoolean()) return equals(Value(a.b ? 1.0 : 0.0), b);
    if (b.isBoolean()) return equals(a, Value(b.b ? 1.0 : 0.0));
    if (a.isObject() && !b.isObject()) return equals(toPrimitive(a), b);
    if (b.isObject() && !a.isObject()) return equals(a, toPrimitive(b));
    return false;
}

Value VM::lessThan(const Value& a, const Value& b) {
    const Value pa = toPrimitive(a), pb = toPrimitive(b);
    if (pa.isString() && pb.isString()) return Value(pa.s < pb.s);
    const double x = toNumber(pa), y = toNumber(pb);
    if (std::isnan(x) || std::isnan(y)) return Value();
    return Value(x < y);
}

std::string VM::typeOf(const Value& v) {
    switch (v.type) {
        case Value::Type::Undefined: return "undefined";
        case Value::Type::Null: return "object";
        case Value::Type::Boolean: return "boolean";
        case Value::Type::Number: return "number";
        case Value::Type::String: return "string";
        case Value::Type::Object: return v.o->isFunction() ? "function" : "object";
    }
    return "undefined";
}

bool VM::sameFunction(const Value& a, const Value& b) {
    if (!a.isObject() || !b.isObject()) return false;
    if (a.o == b.o) return true;
    auto fa = std::dynamic_pointer_cast<FunctionObject>(a.o), fb = std::dynamic_pointer_cast<FunctionObject>(b.o);
    if (!fa || !fb || !fa->isMethodClosure || !fb->isMethodClosure) return false;
    return fa->method.abc == fb->method.abc && fa->method.index == fb->method.index && !fa->method.native && !fb->method.native &&
           fa->boundThis.isObject() && fb->boundThis.isObject() && fa->boundThis.o == fb->boundThis.o;
}

ClassPtr VM::classOf(const Value& v) {
    switch (v.type) {
        case Value::Type::Boolean: return booleanClass;
        case Value::Type::Number:
            if (v.n == std::floor(v.n) && std::fabs(v.n) < 2147483648.0) return intClass;
            return numberClass;
        case Value::Type::String: return stringClass;
        case Value::Type::Object:
            if (std::dynamic_pointer_cast<Class>(v.o)) return classClass;
            return v.o->cls ? v.o->cls : objectClass;
        default: return nullptr;
    }
}

bool VM::isType(const Value& v, const ClassPtr& cls) {
    if (!cls) return false;
    if (cls == objectClass) return !v.isNullish();
    if (v.isNumber()) {
        if (cls == numberClass) return true;
        if (cls == intClass) return v.n == std::floor(v.n) && v.n >= -2147483648.0 && v.n <= 2147483647.0;
        if (cls == uintClass) return v.n == std::floor(v.n) && v.n >= 0 && v.n <= 4294967295.0;
        return false;
    }
    if (v.isString()) return cls == stringClass;
    if (v.isBoolean()) return cls == booleanClass;
    if (!v.isObject()) return false;
    if (cls == classClass) return std::dynamic_pointer_cast<Class>(v.o) != nullptr;
    if (cls == functionClass) return v.o->isFunction();
    return v.o->cls && v.o->cls->isSubclassOf(cls.get());
}

Value VM::coerceToType(const Value& v, const std::string& t) {
    if (t.empty() || t == "*") return v;
    if (t == "int") return Value(static_cast<double>(toInt32(v)));
    if (t == "uint") return Value(static_cast<double>(toUint32(v)));
    if (t == "Number") return Value(toNumber(v));
    if (t == "Boolean") return Value(toBoolean(v));
    if (t == "String") return v.isNullish() ? Value::null() : Value(toString(v));
    if (v.isUndefined()) return Value::null();
    return v;
}

// ------------------------------------------------------------------ properties

const TraitTable* VM::traitsOf(const Value& v, Object** holder, Value* receiver) {
    if (receiver) *receiver = v;
    if (v.isObject()) {
        if (holder) *holder = v.o.get();
        return v.o->traits.get();
    }
    if (holder) *holder = nullptr;
    ClassPtr c = v.isString() ? stringClass : v.isBoolean() ? booleanClass : v.isNumber() ? numberClass : nullptr;
    return c ? c->instanceTraits.get() : nullptr;
}

Value VM::getFromTraits(Object* holder, const Trait& t, const Value& receiver) {
    switch (t.kind) {
        case Trait::Kind::Slot:
        case Trait::Kind::Class:
            return holder && t.slot < holder->slots.size() ? holder->slots[t.slot] : Value();
        case Trait::Kind::Method: {
            auto f = std::make_shared<FunctionObject>();
            f->cls = functionClass;
            f->proto = functionClass ? functionClass->prototype : nullptr;
            f->method = t.method;
            f->boundThis = receiver;
            f->isMethodClosure = true;
            f->declaring = t.declaring;
            if (t.declaring) f->scope = t.declaring->scope;
            return Value(f);
        }
        case Trait::Kind::Accessor: {
            if (!t.getter.valid()) return {};
            Args none;
            return invoke(t.getter, receiver, none, t.declaring ? &t.declaring->scope : nullptr, t.declaring);
        }
    }
    return {};
}

Value VM::getProperty(const Value& obj, const Multiname& mn, const Value* key) {
    if (obj.isNullish()) {
        throwError("TypeError", "Cannot access property " + (key ? toString(*key) : mn.name) + " of a null object reference.");
    }
    if (obj.isObject()) {
        if (key) {
            if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
                const int i = d->indexOf(*this, *key);
                if (i >= 0) return d->entries[static_cast<std::size_t>(i)].second;
                if (!key->isObject()) { /* fall through to string lookup */ } else return {};
            }
            std::size_t idx;
            if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) {
                if (numberIndex(*key, idx) || (key->isString() && arrayIndex(key->s, idx))) return idx < a->items.size() ? a->items[idx] : Value();
            }
        } else if (!mn.name.empty() && mn.name[0] >= '0' && mn.name[0] <= '9') {
            std::size_t idx;
            if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) if (arrayIndex(mn.name, idx)) return idx < a->items.size() ? a->items[idx] : Value();
        }
    }
    Multiname m = mn;
    if (key) m.name = toString(*key);
    Object* holder = nullptr;
    Value receiver;
    const TraitTable* traits = traitsOf(obj, &holder, &receiver);
    if (traits) if (const Trait* t = traits->find(m)) return getFromTraits(holder, *t, receiver);
    if (obj.isObject()) {
        Value native;
        if (obj.o->nativeGet(*this, m, key, native)) return native;
    }
    if (!m.hasPublic() && !key) {
        // Non-public names only resolve through traits.
    }
    if (obj.isObject()) {
        for (Object* o = obj.o.get(); o; o = o->proto.get()) {
            if (Value* v = o->dynamic.find(m.name)) return *v;
        }
        if (!obj.o->isDynamic && !key) warnOnce("property '" + m.name + "' not found on " + (obj.o->cls ? obj.o->cls->qualifiedName() : std::string("object")));
    } else {
        // Primitive: look on the class prototype.
        ClassPtr c = classOf(obj);
        for (Object* o = c ? c->prototype.get() : nullptr; o; o = o->proto.get()) {
            if (Value* v = o->dynamic.find(m.name)) return *v;
        }
    }
    return {};
}

void VM::setProperty(const Value& obj, const Multiname& mn, const Value& v, bool init, const Value* key) {
    if (obj.isNullish()) throwError("TypeError", "Cannot set property " + (key ? toString(*key) : mn.name) + " of a null object reference.");
    if (!obj.isObject()) return; // primitives are immutable
    Object* o = obj.o.get();
    if (key) {
        if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
            if (key->isObject()) {
                const int i = d->indexOf(*this, *key);
                if (i >= 0) d->entries[static_cast<std::size_t>(i)].second = v;
                else d->entries.emplace_back(*key, v);
                return;
            }
        }
        std::size_t idx;
        if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) {
            if ((numberIndex(*key, idx) || (key->isString() && arrayIndex(key->s, idx))) && idx < 50'000'000) {
                if (idx >= a->items.size()) a->items.resize(idx + 1);
                a->items[idx] = v;
                return;
            }
        }
    }
    Multiname m = mn;
    if (key) m.name = toString(*key);
    if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
        // String keys on a Dictionary are ordinary entries too.
        const Value k(m.name);
        const int i = d->indexOf(*this, k);
        if (i >= 0) d->entries[static_cast<std::size_t>(i)].second = v;
        else if (!o->traits || !o->traits->find(m)) { d->entries.emplace_back(k, v); return; }
    }
    if (!(o->traits && o->traits->find(m)) && o->nativeSet(*this, m, key, v)) return;
    if (o->traits) {
        if (const Trait* t = o->traits->find(m)) {
            switch (t->kind) {
                case Trait::Kind::Slot:
                case Trait::Kind::Class:
                    if (t->isConst && !init) { warnOnce("assignment to constant " + m.name); }
                    if (t->slot < o->slots.size()) o->slots[t->slot] = coerceToType(v, t->typeName);
                    return;
                case Trait::Kind::Accessor:
                    if (t->setter.valid()) {
                        Args a{v};
                        invoke(t->setter, obj, a, t->declaring ? &t->declaring->scope : nullptr, t->declaring);
                    } else {
                        warnOnce("property " + m.name + " is read-only");
                    }
                    return;
                case Trait::Kind::Method:
                    warnOnce("cannot assign to method " + m.name);
                    return;
            }
        }
    }
    if (!o->isDynamic) warnOnce("creating dynamic property '" + m.name + "' on sealed " + (o->cls ? o->cls->qualifiedName() : "object"));
    o->dynamic.set(m.name, v);
}

bool VM::hasProperty(const Value& obj, const Multiname& mn, const Value* key) {
    if (!obj.isObject()) return false;
    if (key) {
        if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
            if (d->indexOf(*this, *key) >= 0) return true;
            if (key->isObject()) return false;
        }
        std::size_t idx;
        if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) {
            if (numberIndex(*key, idx) || (key->isString() && arrayIndex(key->s, idx))) return idx < a->items.size();
        }
    }
    Multiname m = mn;
    if (key) m.name = toString(*key);
    if (obj.o->traits && obj.o->traits->find(m)) return true;
    if (obj.o->nativeHas(*this, m, key)) return true;
    for (Object* o = obj.o.get(); o; o = o->proto.get()) if (o->dynamic.find(m.name)) return true;
    return false;
}

bool VM::deleteProperty(const Value& obj, const Multiname& mn, const Value* key) {
    if (!obj.isObject()) return false;
    if (key) {
        if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
            const int i = d->indexOf(*this, *key);
            if (i >= 0) { d->entries.erase(d->entries.begin() + i); return true; }
            if (key->isObject()) return false;
        }
        std::size_t idx;
        if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) {
            if (numberIndex(*key, idx) || (key->isString() && arrayIndex(key->s, idx))) {
                if (idx < a->items.size()) a->items[idx] = Value();
                return true;
            }
        }
    }
    const std::string name = key ? toString(*key) : mn.name;
    if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
        const int i = d->indexOf(*this, Value(name));
        if (i >= 0) { d->entries.erase(d->entries.begin() + i); return true; }
    }
    return obj.o->dynamic.remove(name);
}

Value VM::callProperty(const Value& obj, const Multiname& mn, Args& args, bool lex, const Value* key) {
    if (obj.isNullish()) throwError("TypeError", "Cannot call method " + (key ? toString(*key) : mn.name) + " of a null object reference.");
    Object* holder = nullptr;
    Value receiver;
    if (!key) {
        const TraitTable* traits = traitsOf(obj, &holder, &receiver);
        if (traits) {
            if (const Trait* t = traits->find(mn)) {
                if (t->kind == Trait::Kind::Method) {
                    return invoke(t->method, lex ? Value::null() : receiver, args, t->declaring ? &t->declaring->scope : nullptr, t->declaring);
                }
                Value fn = getFromTraits(holder, *t, receiver);
                if (!fn.isObject()) throwError("TypeError", "value is not a function: " + mn.name);
                return call(fn, lex ? Value::null() : obj, std::move(args));
            }
        }
    }
    Value fn = getProperty(obj, mn, key);
    if (!fn.isObject()) {
        throwError("TypeError", "call to undefined method " + (key ? toString(*key) : mn.name) + " on " +
                                    (obj.isObject() && obj.o->cls ? obj.o->cls->qualifiedName() : typeOf(obj)));
    }
    return call(fn, lex ? Value::null() : obj, std::move(args));
}

void VM::reportError(const char* where, const ScriptException& e) {
    ++errors;
    if (errors > 30) return;
    std::string msg;
    try {
        msg = e.value.isObject() ? toString(getPublic(e.value, "message")) : toString(e.value);
        if (e.value.isObject() && e.value.o->cls) msg = e.value.o->cls->name + ": " + msg;
    } catch (...) {
        msg = "<error>";
    }
    std::cerr << "AVM2 uncaught (" << where << "): " << msg << "\n";
    static const bool traceStack = std::getenv("FP_TRACE_STACK") != nullptr;
    if (traceStack) {
        for (std::size_t i = errorStack_.size(), n = 0; i-- > 0 && n < 8; ++n) std::cerr << "    at " << errorStack_[i] << "\n";
    }
    errorStackFresh_ = true;
}

// ------------------------------------------------------------------ decoding

void VM::decode(AbcFile& abc, MethodBody& body) {
    (void)abc;
    const auto& code = body.code;
    std::vector<std::int32_t> offsetToIndex(code.size() + 1, -1);
    std::vector<std::pair<std::uint32_t, std::vector<std::int32_t>>> pendingTargets; // instr -> absolute offsets
    std::size_t pc = 0;
    auto u30 = [&](std::size_t& p) {
        std::uint32_t r = 0;
        for (int i = 0; i < 5 && p < code.size(); ++i) {
            const auto b = code[p++];
            r |= static_cast<std::uint32_t>(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) break;
        }
        return r;
    };
    auto s24 = [&](std::size_t& p) {
        if (p + 3 > code.size()) return 0;
        std::uint32_t v = code[p] | (code[p + 1] << 8) | (code[p + 2] << 16);
        p += 3;
        if (v & 0x800000u) v |= 0xff000000u;
        return static_cast<std::int32_t>(v);
    };
    while (pc < code.size()) {
        Instr ins;
        const std::size_t start = pc;
        ins.op = code[pc++];
        offsetToIndex[start] = static_cast<std::int32_t>(body.instrs.size());
        std::vector<std::int32_t> absTargets;
        switch (ins.op) {
            // two u30 operands
            case 0x32: case 0x43: case 0x44: case 0x45: case 0x46: case 0x4a: case 0x4c: case 0x4e: case 0x4f:
                ins.a = static_cast<std::int32_t>(u30(pc));
                ins.b = static_cast<std::int32_t>(u30(pc));
                break;
            // one u30 operand
            case 0x04: case 0x05: case 0x06: case 0x08: case 0x22: case 0x25: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
            case 0x31: case 0x40: case 0x41: case 0x42: case 0x49: case 0x53: case 0x55: case 0x56: case 0x58: case 0x59:
            case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: case 0x60: case 0x61: case 0x62: case 0x63:
            case 0x66: case 0x67: case 0x68: case 0x6a: case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0x80: case 0x86:
            case 0x92: case 0x94: case 0xb2: case 0xc2: case 0xc3: case 0xf0: case 0xf1: case 0xf2:
                ins.a = static_cast<std::int32_t>(u30(pc));
                break;
            case 0x24: case 0x65: // pushbyte, getscopeobject
                ins.a = pc < code.size() ? code[pc++] : 0;
                break;
            case 0xef: // debug
                pc++; u30(pc); pc++; u30(pc);
                break;
            case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
            case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a: {
                const auto off = s24(pc);
                absTargets.push_back(static_cast<std::int32_t>(pc) + off);
                break;
            }
            case 0x1b: { // lookupswitch: offsets relative to the instruction start
                absTargets.push_back(static_cast<std::int32_t>(start) + s24(pc));
                const auto count = u30(pc);
                for (std::uint32_t i = 0; i <= count && pc < code.size(); ++i) absTargets.push_back(static_cast<std::int32_t>(start) + s24(pc));
                break;
            }
            default: break;
        }
        if (!absTargets.empty()) pendingTargets.emplace_back(static_cast<std::uint32_t>(body.instrs.size()), absTargets);
        body.instrs.push_back(std::move(ins));
    }
    offsetToIndex[code.size()] = static_cast<std::int32_t>(body.instrs.size());
    auto resolve = [&](std::int32_t off) -> std::int32_t {
        if (off < 0 || static_cast<std::size_t>(off) > code.size()) return static_cast<std::int32_t>(body.instrs.size());
        // Targets always hit an instruction start in compiler output; search forward otherwise.
        for (std::size_t o = static_cast<std::size_t>(off); o <= code.size(); ++o) if (offsetToIndex[o] >= 0) return offsetToIndex[o];
        return static_cast<std::int32_t>(body.instrs.size());
    };
    for (auto& [idx, targets] : pendingTargets) {
        auto& ins = body.instrs[idx];
        if (ins.op == 0x1b) for (auto t : targets) ins.targets.push_back(resolve(t));
        else ins.target = resolve(targets[0]);
    }
    for (auto& ex : body.exceptions) {
        ex.from = static_cast<std::uint32_t>(resolve(static_cast<std::int32_t>(ex.from)));
        ex.to = static_cast<std::uint32_t>(resolve(static_cast<std::int32_t>(ex.to)));
        ex.target = static_cast<std::uint32_t>(resolve(static_cast<std::int32_t>(ex.target)));
    }
    body.decoded = true;
}

// ------------------------------------------------------------------ interpreter

Value VM::execute(AbcFile& abc, MethodBody& body, const Value& thisv, Args& args, const std::vector<ObjectPtr>* outer,
                  Class* declaring, const MethodInfo& info) {
    if (!body.decoded) decode(abc, body);
    if (gDepth > 400) throwError("Error", "Stack overflow");
    struct DepthGuard { DepthGuard() { ++gDepth; } ~DepthGuard() { --gDepth; } } guard;
    // Name lookups made by this method resolve in the application domain of its ABC block.
    struct DomainGuard {
        int& slot;
        int saved;
        DomainGuard(int& s, int d) : slot(s), saved(s) { slot = d; }
        ~DomainGuard() { slot = saved; }
    } domainGuard(currentDomain_, abc.domain);
    struct StackGuard {
        VM& vm;
        StackGuard(VM& v, std::string n) : vm(v) { vm.callStack_.push_back(std::move(n)); }
        ~StackGuard() {
            if (std::uncaught_exceptions() > 0 && vm.errorStackFresh_) { vm.errorStack_ = vm.callStack_; vm.errorStackFresh_ = false; }
            vm.callStack_.pop_back();
        }
    } stackGuard(*this, (declaring ? declaring->name + "." : std::string()) + info.name);

    std::vector<Value> locals(std::max<std::size_t>(body.localCount, info.paramCount + 2));
    locals[0] = thisv;
    for (std::uint32_t i = 0; i < info.paramCount; ++i) {
        Value v;
        if (i < args.size()) v = args[i];
        else {
            const std::size_t optIndex = i - (info.paramCount - info.optionals.size());
            if (i >= info.paramCount - info.optionals.size() && optIndex < info.optionals.size()) {
                const auto [vi, vk] = info.optionals[optIndex];
                switch (vk) {
                    case 0x03: v = Value(static_cast<double>(abc.ints[vi])); break;
                    case 0x04: v = Value(static_cast<double>(abc.uints[vi])); break;
                    case 0x06: v = Value(abc.doubles[vi]); break;
                    case 0x02: v = Value(vi < abc.floats.size() ? abc.floats[vi] : 0.0); break;
                    case 0x01: v = Value(abc.strings[vi]); break;
                    case 0x0b: v = Value(true); break;
                    case 0x0a: v = Value(false); break;
                    case 0x0c: v = Value::null(); break;
                    default: v = Value(); break;
                }
            }
        }
        if (i < info.paramTypes.size() && info.paramTypes[i]) v = coerceToType(v, abc.multinames[info.paramTypes[i]].name);
        locals[i + 1] = v;
    }
    if (info.flags & 0x04) { // NEED_REST
        std::vector<Value> rest;
        for (std::size_t i = info.paramCount; i < args.size(); ++i) rest.push_back(args[i]);
        if (info.paramCount + 1 < locals.size()) locals[info.paramCount + 1] = Value(newArray(rest));
    } else if (info.flags & 0x01) { // NEED_ARGUMENTS
        if (info.paramCount + 1 < locals.size()) locals[info.paramCount + 1] = Value(newArray(args));
    }

    std::vector<Value> stack;
    stack.reserve(body.maxStack + 4);
    std::vector<Value> scopeStack;
    std::vector<bool> scopeWith;
    static const std::vector<ObjectPtr> kNoScope;
    const std::vector<ObjectPtr>& outerScope = outer ? *outer : kNoScope;

    auto pop = [&]() -> Value {
        if (stack.empty()) return {};
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    };
    auto popArgs = [&](std::int32_t n) {
        Args a(static_cast<std::size_t>(std::max(0, n)));
        for (std::int32_t i = n - 1; i >= 0; --i) a[static_cast<std::size_t>(i)] = pop();
        return a;
    };
    // Multiname operand with runtime parts popped from the stack.
    struct RtName { const Multiname* mn; Multiname owned; Value key; bool hasKey = false; };
    auto readName = [&](std::int32_t index, RtName& out) {
        const Multiname& mn = abc.multinames[static_cast<std::size_t>(index)];
        out.mn = &mn;
        out.hasKey = false;
        if (mn.rtName) {
            out.key = pop();
            out.hasKey = true;
        }
        if (mn.rtNs) {
            Value ns = pop();
            out.owned = mn;
            out.owned.nss = {Namespace::pub(ns.isString() ? ns.s : std::string())};
            out.mn = &out.owned;
        }
    };
    auto findProperty = [&](const RtName& n, bool strict) -> Value {
        const Value* key = n.hasKey ? &n.key : nullptr;
        for (std::size_t i = scopeStack.size(); i-- > 0;) {
            const Value& s = scopeStack[i];
            if (s.isObject() && hasProperty(s, *n.mn, key)) return s;
        }
        for (std::size_t i = outerScope.size(); i-- > 0;) {
            Value s(outerScope[i]);
            if (hasProperty(s, *n.mn, key)) return s;
        }
        if (auto def = findDefinition(*n.mn, strict)) return Value(def);
        if (strict) {
            // Global function or class not provided by this runtime: fail loudly once, keep going.
            warnOnce("unresolved name '" + n.mn->name + "'");
            throwError("ReferenceError", "Variable " + n.mn->name + " is not defined.");
        }
        return outerScope.empty() ? (scopeStack.empty() ? Value(toplevel) : scopeStack.front()) : Value(outerScope.front());
    };

    std::size_t ip = 0;
    const auto& code = body.instrs;
    while (true) {
        try {
            while (ip < code.size()) {
                const Instr& in = code[ip++];
                ++instructions;
                if (--gBudget == 0) {
                    gBudget = kInstructionBudget;
                    throwError("Error", "Script timeout (possible infinite loop)");
                }
                switch (in.op) {
                    case 0x02: case 0x09: case 0x01: case 0xef: case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0x06: break;
                    case 0x03: throw ScriptException{pop()};
                    case 0x04: case 0x05: { // getsuper / setsuper
                        Value v = in.op == 0x05 ? pop() : Value();
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        Class* sup = declaring ? declaring->super.get() : nullptr;
                        const Trait* t = sup ? sup->instanceTraits->find(*n.mn) : nullptr;
                        if (in.op == 0x04) stack.push_back(t ? getFromTraits(obj.o.get(), *t, obj) : getProperty(obj, *n.mn));
                        else if (t && t->kind == Trait::Kind::Accessor && t->setter.valid()) { Args a{v}; invoke(t->setter, obj, a, &t->declaring->scope, t->declaring); }
                        else setProperty(obj, *n.mn, v);
                        break;
                    }
                    case 0x07: pop(); break; // dxnslate
                    case 0x08: locals[static_cast<std::size_t>(in.a)] = Value(); break;
                    case 0x0c: { Value b = pop(), a = pop(); Value r = lessThan(a, b); if (!(r.isBoolean() && r.b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x0d: { Value b = pop(), a = pop(); Value r = lessThan(b, a); if (!(r.isBoolean() && !r.b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x0e: { Value b = pop(), a = pop(); Value r = lessThan(b, a); if (!(r.isBoolean() && r.b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x0f: { Value b = pop(), a = pop(); Value r = lessThan(a, b); if (!(r.isBoolean() && !r.b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x10: ip = static_cast<std::size_t>(in.target); break;
                    case 0x11: if (toBoolean(pop())) ip = static_cast<std::size_t>(in.target); break;
                    case 0x12: if (!toBoolean(pop())) ip = static_cast<std::size_t>(in.target); break;
                    case 0x13: { Value b = pop(), a = pop(); if (equals(a, b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x14: { Value b = pop(), a = pop(); if (!equals(a, b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x15: { Value b = pop(), a = pop(); Value r = lessThan(a, b); if (r.isBoolean() && r.b) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x16: { Value b = pop(), a = pop(); Value r = lessThan(b, a); if (r.isBoolean() && !r.b) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x17: { Value b = pop(), a = pop(); Value r = lessThan(b, a); if (r.isBoolean() && r.b) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x18: { Value b = pop(), a = pop(); Value r = lessThan(a, b); if (r.isBoolean() && !r.b) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x19: { Value b = pop(), a = pop(); if (strictEquals(a, b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x1a: { Value b = pop(), a = pop(); if (!strictEquals(a, b)) ip = static_cast<std::size_t>(in.target); break; }
                    case 0x1b: {
                        const std::int32_t i = toInt32(pop());
                        const std::size_t cases = in.targets.size() - 1;
                        ip = static_cast<std::size_t>(i >= 0 && static_cast<std::size_t>(i) < cases ? in.targets[static_cast<std::size_t>(i) + 1] : in.targets[0]);
                        break;
                    }
                    case 0x1c: { Value o = pop(); scopeStack.push_back(o); scopeWith.push_back(true); break; }
                    case 0x1d: if (!scopeStack.empty()) { scopeStack.pop_back(); scopeWith.pop_back(); } break;
                    case 0x1e: case 0x23: { // nextname / nextvalue
                        const std::int32_t idx = toInt32(pop());
                        Value obj = pop();
                        Value result;
                        if (obj.isObject() && idx > 0) {
                            std::size_t i = static_cast<std::size_t>(idx - 1);
                            std::vector<Value> nativeItems;
                            if (obj.o->nativeItems(*this, nativeItems)) {
                                if (i < nativeItems.size()) result = in.op == 0x1e ? Value(std::to_string(i)) : nativeItems[i];
                                goto pushed;
                            }
                            if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) {
                                if (i < a->items.size()) { result = in.op == 0x1e ? Value(std::to_string(i)) : a->items[i]; goto pushed; }
                                i -= a->items.size();
                            }
                            if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) {
                                if (i < d->entries.size()) { result = in.op == 0x1e ? d->entries[i].first : d->entries[i].second; goto pushed; }
                                i -= d->entries.size();
                            }
                            if (i < obj.o->dynamic.entries.size()) {
                                const auto& e = obj.o->dynamic.entries[i];
                                result = in.op == 0x1e ? Value(e.key) : e.value;
                            }
                        }
                    pushed:
                        stack.push_back(result);
                        break;
                    }
                    case 0x1f: case 0x32: { // hasnext / hasnext2
                        auto nextIndex = [&](const Value& obj, std::int32_t idx) -> std::int32_t {
                            if (!obj.isObject()) return 0;
                            std::size_t total = 0;
                            std::vector<bool> alive;
                            std::vector<Value> nativeItems;
                            if (obj.o->nativeItems(*this, nativeItems)) alive.assign(nativeItems.size(), true);
                            if (auto a = std::dynamic_pointer_cast<ArrayObject>(obj.o)) { for (auto& v : a->items) { (void)v; alive.push_back(true); } }
                            if (auto d = std::dynamic_pointer_cast<DictionaryObject>(obj.o)) for (std::size_t k = 0; k < d->entries.size(); ++k) alive.push_back(true);
                            for (const auto& e : obj.o->dynamic.entries) alive.push_back(e.alive);
                            total = alive.size();
                            for (std::size_t k = static_cast<std::size_t>(std::max(0, idx)); k < total; ++k) {
                                if (alive[k]) return static_cast<std::int32_t>(k + 1);
                            }
                            return 0;
                        };
                        if (in.op == 0x1f) {
                            const std::int32_t idx = toInt32(pop());
                            Value obj = pop();
                            stack.emplace_back(static_cast<double>(nextIndex(obj, idx)));
                        } else {
                            Value& obj = locals[static_cast<std::size_t>(in.a)];
                            Value& idxV = locals[static_cast<std::size_t>(in.b)];
                            const std::int32_t next = nextIndex(obj, toInt32(idxV));
                            idxV = Value(static_cast<double>(next));
                            if (next == 0) obj = Value::null();
                            stack.emplace_back(next != 0);
                        }
                        break;
                    }
                    case 0x20: stack.push_back(Value::null()); break;
                    case 0x21: stack.emplace_back(); break;
                    case 0x22: // pushfloat (Harman AIR); pushconstant otherwise
                        if (static_cast<std::size_t>(in.a) < abc.floats.size() && abc.floats.size() > 1)
                            stack.emplace_back(abc.floats[static_cast<std::size_t>(in.a)]);
                        else
                            stack.emplace_back();
                        break;
                    case 0x24: stack.emplace_back(static_cast<double>(static_cast<std::int8_t>(in.a))); break;
                    case 0x25: stack.emplace_back(static_cast<double>(static_cast<std::int16_t>(in.a))); break;
                    case 0x26: stack.emplace_back(true); break;
                    case 0x27: stack.emplace_back(false); break;
                    case 0x28: stack.emplace_back(kNaN); break;
                    case 0x29: pop(); break;
                    case 0x2a: stack.push_back(stack.empty() ? Value() : stack.back()); break;
                    case 0x2b: if (stack.size() >= 2) std::swap(stack[stack.size() - 1], stack[stack.size() - 2]); break;
                    case 0x2c: stack.emplace_back(abc.strings[static_cast<std::size_t>(in.a)]); break;
                    case 0x2d: stack.emplace_back(static_cast<double>(abc.ints[static_cast<std::size_t>(in.a)])); break;
                    case 0x2e: stack.emplace_back(static_cast<double>(abc.uints[static_cast<std::size_t>(in.a)])); break;
                    case 0x2f: stack.emplace_back(abc.doubles[static_cast<std::size_t>(in.a)]); break;
                    case 0x30: { Value o = pop(); scopeStack.push_back(o); scopeWith.push_back(false); break; }
                    case 0x31: stack.emplace_back(abc.namespaces[static_cast<std::size_t>(in.a)].uri); break;
                    case 0x40: { // newfunction
                        auto f = std::make_shared<FunctionObject>();
                        f->cls = functionClass;
                        f->proto = functionClass ? functionClass->prototype : nullptr;
                        f->method.abc = &abc;
                        f->method.index = static_cast<std::uint32_t>(in.a);
                        f->scope = outerScope;
                        for (auto& s : scopeStack) if (s.isObject()) f->scope.push_back(s.o);
                        f->declaring = declaring;
                        auto proto = newObject();
                        proto->dynamic.set("constructor", Value(f));
                        f->dynamic.set("prototype", Value(proto));
                        stack.emplace_back(f);
                        break;
                    }
                    case 0x41: { // call
                        Args a = popArgs(in.a);
                        Value receiver = pop();
                        Value fn = pop();
                        stack.push_back(call(fn, receiver, std::move(a)));
                        break;
                    }
                    case 0x42: { Args a = popArgs(in.a); Value ctor = pop(); stack.push_back(construct(ctor, std::move(a))); break; }
                    case 0x45: case 0x4e: { // callsuper / callsupervoid
                        Args a = popArgs(in.b);
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        Class* sup = declaring ? declaring->super.get() : nullptr;
                        const Trait* t = sup ? sup->instanceTraits->find(*n.mn) : nullptr;
                        Value r;
                        if (t && t->kind == Trait::Kind::Method) r = invoke(t->method, obj, a, t->declaring ? &t->declaring->scope : nullptr, t->declaring);
                        else if (t) r = call(getFromTraits(obj.o.get(), *t, obj), obj, std::move(a));
                        else warnOnce("super method not found: " + n.mn->name);
                        if (in.op == 0x45) stack.push_back(r);
                        break;
                    }
                    case 0x46: case 0x4c: case 0x4f: { // callproperty / callproplex / callpropvoid
                        Args a = popArgs(in.b);
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        Value r = callProperty(obj, *n.mn, a, in.op == 0x4c, n.hasKey ? &n.key : nullptr);
                        if (in.op != 0x4f) stack.push_back(r);
                        break;
                    }
                    case 0x43: case 0x44: { // callmethod / callstatic (dispatch ids): unsupported, keep stack shape
                        popArgs(in.b);
                        pop();
                        warnOnce("callmethod/callstatic not supported");
                        stack.emplace_back();
                        break;
                    }
                    case 0x47: return {};
                    case 0x48: return pop();
                    case 0x49: { // constructsuper
                        Args a = popArgs(in.a);
                        Value obj = pop();
                        if (declaring && declaring->super && obj.isObject()) runConstructor(declaring->super, obj.o, a);
                        break;
                    }
                    case 0x4a: { // constructprop
                        Args a = popArgs(in.b);
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        Value ctor = getProperty(obj, *n.mn, n.hasKey ? &n.key : nullptr);
                        if (!ctor.isObject()) throwError("TypeError", "cannot construct undefined " + n.mn->name);
                        stack.push_back(construct(ctor, std::move(a)));
                        break;
                    }
                    case 0x50: stack.emplace_back(static_cast<double>(toInt32(pop()) & 1 ? -1 : 0)); break;
                    case 0x51: stack.emplace_back(static_cast<double>(static_cast<std::int8_t>(toInt32(pop())))); break;
                    case 0x52: stack.emplace_back(static_cast<double>(static_cast<std::int16_t>(toInt32(pop())))); break;
                    case 0x53: { popArgs(in.a); break; } // applytype: leave the base type on the stack
                    case 0x55: { // newobject
                        auto o = newObject();
                        std::vector<std::pair<Value, Value>> pairs;
                        for (std::int32_t i = 0; i < in.a; ++i) { Value v = pop(); Value k = pop(); pairs.emplace_back(k, v); }
                        for (auto it = pairs.rbegin(); it != pairs.rend(); ++it) o->dynamic.set(toString(it->first), it->second);
                        stack.emplace_back(o);
                        break;
                    }
                    case 0x56: { Args a = popArgs(in.a); stack.emplace_back(newArray(std::move(a))); break; }
                    case 0x57: { // newactivation
                        if (!body.activationTraits) {
                            body.activationTraits = std::make_shared<TraitTable>();
                            buildTraits(abc, body.traits, *body.activationTraits, nullptr);
                        }
                        auto o = std::make_shared<Object>();
                        o->traits = body.activationTraits;
                        initSlots(*o);
                        stack.emplace_back(o);
                        break;
                    }
                    case 0x58: { // newclass
                        Value base = pop();
                        ClassPtr b = base.isObject() ? std::dynamic_pointer_cast<Class>(base.o) : nullptr;
                        std::vector<ObjectPtr> scope = outerScope;
                        for (auto& s : scopeStack) if (s.isObject()) scope.push_back(s.o);
                        stack.emplace_back(newClass(abc, static_cast<std::uint32_t>(in.a), b, scope));
                        break;
                    }
                    case 0x59: { // getdescendants
                        const auto& mn = abc.multinames[static_cast<std::size_t>(in.a)];
                        Value obj = pop();
                        Value out;
                        if (!obj.isObject() || !obj.o->nativeDescendants(*this, mn, out)) throwError("TypeError", "descendants of a non-XML value");
                        stack.push_back(out);
                        break;
                    }
                    case 0x5a: { // newcatch
                        const auto& ex = body.exceptions[static_cast<std::size_t>(in.a)];
                        auto table = std::make_shared<TraitTable>();
                        Trait t;
                        const auto& vn = abc.multinames[ex.varName];
                        t.name = vn.name;
                        t.ns = vn.nss.empty() ? Namespace::pub() : vn.nss[0];
                        t.kind = Trait::Kind::Slot;
                        t.slot = 0;
                        table->slotCount = 1;
                        table->add(t);
                        auto o = std::make_shared<Object>();
                        o->traits = table;
                        o->slots.resize(1);
                        stack.emplace_back(o);
                        break;
                    }
                    case 0x5b: case 0x5c: case 0x5d: case 0x5e: { // findprop(global)(strict)
                        RtName n; readName(in.a, n);
                        stack.push_back(findProperty(n, in.op == 0x5d || in.op == 0x5b));
                        break;
                    }
                    case 0x5f: { RtName n; readName(in.a, n); stack.push_back(findProperty(n, true)); break; } // finddef
                    case 0x60: { // getlex
                        RtName n; readName(in.a, n);
                        Value holder = findProperty(n, true);
                        stack.push_back(getProperty(holder, *n.mn));
                        break;
                    }
                    case 0x61: case 0x68: { // setproperty / initproperty
                        Value v = pop();
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        setProperty(obj, *n.mn, v, in.op == 0x68, n.hasKey ? &n.key : nullptr);
                        break;
                    }
                    case 0x62: stack.push_back(locals[static_cast<std::size_t>(in.a)]); break;
                    case 0x63: locals[static_cast<std::size_t>(in.a)] = pop(); break;
                    case 0x64: stack.push_back(outerScope.empty() ? (scopeStack.empty() ? Value() : scopeStack.front()) : Value(outerScope.front())); break;
                    case 0x65: stack.push_back(static_cast<std::size_t>(in.a) < scopeStack.size() ? scopeStack[static_cast<std::size_t>(in.a)] : Value()); break;
                    case 0x66: { // getproperty
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        stack.push_back(getProperty(obj, *n.mn, n.hasKey ? &n.key : nullptr));
                        break;
                    }
                    case 0x67: stack.push_back(static_cast<std::size_t>(in.a) < outerScope.size() ? Value(outerScope[static_cast<std::size_t>(in.a)]) : Value()); break;
                    case 0x6a: { // deleteproperty
                        RtName n; readName(in.a, n);
                        Value obj = pop();
                        stack.emplace_back(deleteProperty(obj, *n.mn, n.hasKey ? &n.key : nullptr));
                        break;
                    }
                    case 0x6c: { // getslot
                        Value obj = pop();
                        const std::size_t s = static_cast<std::size_t>(in.a - 1);
                        stack.push_back(obj.isObject() && s < obj.o->slots.size() ? obj.o->slots[s] : Value());
                        break;
                    }
                    case 0x6d: { // setslot
                        Value v = pop();
                        Value obj = pop();
                        const std::size_t s = static_cast<std::size_t>(in.a - 1);
                        if (obj.isObject()) {
                            if (s >= obj.o->slots.size()) obj.o->slots.resize(s + 1);
                            obj.o->slots[s] = v;
                        }
                        break;
                    }
                    case 0x6e: case 0x6f: { // get/setglobalslot
                        Value global = outerScope.empty() ? (scopeStack.empty() ? Value() : scopeStack.front()) : Value(outerScope.front());
                        const std::size_t s = static_cast<std::size_t>(in.a - 1);
                        if (in.op == 0x6e) stack.push_back(global.isObject() && s < global.o->slots.size() ? global.o->slots[s] : Value());
                        else { Value v = pop(); if (global.isObject() && s < global.o->slots.size()) global.o->slots[s] = v; }
                        break;
                    }
                    case 0x70: case 0x85: { Value v = pop(); stack.emplace_back(in.op == 0x85 && v.isNullish() ? Value::null() : Value(toString(v))); break; }
                    case 0x71: case 0x72: stack.emplace_back(toString(pop())); break;
                    case 0x73: case 0x83: stack.emplace_back(static_cast<double>(toInt32(pop()))); break;
                    case 0x74: case 0x88: stack.emplace_back(static_cast<double>(toUint32(pop()))); break;
                    case 0x75: case 0x84: case 0x7a: stack.emplace_back(toNumber(pop())); break;
                    case 0x79: stack.emplace_back(static_cast<double>(static_cast<float>(toNumber(pop())))); break;
                    case 0x76: case 0x81: stack.emplace_back(toBoolean(pop())); break;
                    case 0x77: { Value v = pop(); if (v.isNullish()) throwError("TypeError", "Cannot convert null to an object"); stack.push_back(v); break; }
                    case 0x78: break; // checkfilter
                    case 0x80: { // coerce
                        Value v = pop();
                        const auto& type = abc.multinames[static_cast<std::size_t>(in.a)].name;
                        stack.push_back(coerceToType(v, type));
                        break;
                    }
                    case 0x82: break; // coerce_a
                    case 0x89: { Value v = pop(); stack.push_back(v.isUndefined() ? Value::null() : v); break; }
                    case 0x86: case 0xb2: { // astype / istype
                        Value v = pop();
                        RtName n; n.mn = &abc.multinames[static_cast<std::size_t>(in.a)];
                        ClassPtr c;
                        if (auto holder = findDefinition(*n.mn, false)) {
                            Value cv = getProperty(Value(holder), *n.mn);
                            c = cv.isObject() ? std::dynamic_pointer_cast<Class>(cv.o) : nullptr;
                        }
                        const bool is = c ? isType(v, c) : false;
                        if (in.op == 0x86) stack.push_back(is ? v : Value::null());
                        else stack.emplace_back(is);
                        break;
                    }
                    case 0x87: case 0xb3: { // astypelate / istypelate
                        Value t = pop();
                        Value v = pop();
                        ClassPtr c = t.isObject() ? std::dynamic_pointer_cast<Class>(t.o) : nullptr;
                        const bool is = c ? isType(v, c) : false;
                        if (in.op == 0x87) stack.push_back(is ? v : Value::null());
                        else stack.emplace_back(is);
                        break;
                    }
                    case 0x90: stack.emplace_back(-toNumber(pop())); break;
                    case 0x91: stack.emplace_back(toNumber(pop()) + 1); break;
                    case 0x93: stack.emplace_back(toNumber(pop()) - 1); break;
                    case 0x92: { auto& l = locals[static_cast<std::size_t>(in.a)]; l = Value(toNumber(l) + 1); break; }
                    case 0x94: { auto& l = locals[static_cast<std::size_t>(in.a)]; l = Value(toNumber(l) - 1); break; }
                    case 0xc2: { auto& l = locals[static_cast<std::size_t>(in.a)]; l = Value(static_cast<double>(toInt32(l) + 1)); break; }
                    case 0xc3: { auto& l = locals[static_cast<std::size_t>(in.a)]; l = Value(static_cast<double>(toInt32(l) - 1)); break; }
                    case 0x95: stack.emplace_back(typeOf(pop())); break;
                    case 0x96: stack.emplace_back(!toBoolean(pop())); break;
                    case 0x97: stack.emplace_back(static_cast<double>(~toInt32(pop()))); break;
                    case 0xa0: { // add
                        Value b = pop(), a = pop();
                        if (a.isNumber() && b.isNumber()) { stack.emplace_back(a.n + b.n); break; }
                        Value pa = toPrimitive(a), pb = toPrimitive(b);
                        if (pa.isString() || pb.isString()) stack.emplace_back(toString(pa) + toString(pb));
                        else stack.emplace_back(toNumber(pa) + toNumber(pb));
                        break;
                    }
                    case 0xa1: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a - b); break; }
                    case 0xa2: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a * b); break; }
                    case 0xa3: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(a / b); break; }
                    case 0xa4: { const double b = toNumber(pop()), a = toNumber(pop()); stack.emplace_back(std::fmod(a, b)); break; }
                    case 0xa5: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(static_cast<std::int32_t>(static_cast<std::uint32_t>(a) << (b & 31)))); break; }
                    case 0xa6: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a >> (b & 31))); break; }
                    case 0xa7: { const auto b = toInt32(pop()); const auto a = toUint32(pop()); stack.emplace_back(static_cast<double>(a >> (b & 31))); break; }
                    case 0xa8: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a & b)); break; }
                    case 0xa9: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a | b)); break; }
                    case 0xaa: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(a ^ b)); break; }
                    case 0xab: { Value b = pop(), a = pop(); stack.emplace_back(equals(a, b)); break; }
                    case 0xac: { Value b = pop(), a = pop(); stack.emplace_back(strictEquals(a, b)); break; }
                    case 0xad: { Value b = pop(), a = pop(); Value r = lessThan(a, b); stack.emplace_back(r.isBoolean() && r.b); break; }
                    case 0xae: { Value b = pop(), a = pop(); Value r = lessThan(b, a); stack.emplace_back(r.isBoolean() && !r.b); break; }
                    case 0xaf: { Value b = pop(), a = pop(); Value r = lessThan(b, a); stack.emplace_back(r.isBoolean() && r.b); break; }
                    case 0xb0: { Value b = pop(), a = pop(); Value r = lessThan(a, b); stack.emplace_back(r.isBoolean() && !r.b); break; }
                    case 0xb1: { // instanceof
                        Value t = pop(), v = pop();
                        bool r = false;
                        Value p = t.isObject() ? getPublic(t, "prototype") : Value();
                        if (v.isObject() && p.isObject()) for (Object* o = v.o->proto.get(); o; o = o->proto.get()) if (o == p.o.get()) { r = true; break; }
                        stack.emplace_back(r);
                        break;
                    }
                    case 0xb4: { // in
                        Value obj = pop(), name = pop();
                        Multiname m = Multiname::publicName(toString(name));
                        stack.emplace_back(hasProperty(obj, m, &name));
                        break;
                    }
                    case 0xc0: stack.emplace_back(static_cast<double>(toInt32(pop()) + 1)); break;
                    case 0xc1: stack.emplace_back(static_cast<double>(toInt32(pop()) - 1)); break;
                    case 0xc4: stack.emplace_back(static_cast<double>(-toInt32(pop()))); break;
                    case 0xc5: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(static_cast<std::int32_t>(static_cast<std::int64_t>(a) + b))); break; }
                    case 0xc6: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(static_cast<std::int32_t>(static_cast<std::int64_t>(a) - b))); break; }
                    case 0xc7: { const auto b = toInt32(pop()), a = toInt32(pop()); stack.emplace_back(static_cast<double>(static_cast<std::int32_t>(static_cast<std::int64_t>(a) * b))); break; }
                    case 0xd0: case 0xd1: case 0xd2: case 0xd3: stack.push_back(locals[in.op - 0xd0u]); break;
                    case 0xd4: case 0xd5: case 0xd6: case 0xd7: locals[in.op - 0xd4u] = pop(); break;
                    case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: pop(); stack.emplace_back(0); break; // alchemy loads
                    case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: pop(); pop(); break;              // alchemy stores
                    default:
                        warnOnce("unsupported opcode " + std::to_string(in.op));
                        break;
                }
            }
            return {};
        } catch (ScriptException& ex) {
            // Find a handler covering the faulting instruction.
            const std::size_t faulting = ip == 0 ? 0 : ip - 1;
            bool handled = false;
            for (const auto& h : body.exceptions) {
                if (faulting < h.from || faulting >= h.to) continue;
                if (h.typeName) {
                    const auto& tn = abc.multinames[h.typeName];
                    ClassPtr c;
                    if (auto holder = findDefinition(tn, false)) {
                        Value cv = getProperty(Value(holder), tn);
                        c = cv.isObject() ? std::dynamic_pointer_cast<Class>(cv.o) : nullptr;
                    }
                    if (c && !isType(ex.value, c)) continue;
                }
                stack.clear();
                scopeStack.clear();
                scopeWith.clear();
                stack.push_back(ex.value);
                ip = h.target;
                handled = true;
                break;
            }
            if (!handled) throw;
        }
    }
}

// ------------------------------------------------------------------ queue / timers

void VM::runQueue() {
    for (std::size_t guard = 0; !queue.empty() && guard < 100000; ++guard) {
        Task t = queue.front();
        queue.pop_front();
        auto obj = t.target.lock();
        if (!obj) continue;
        runFrameScript(obj, t.frame);
    }
}

void VM::runTimers(double now) {
    std::vector<int> due;
    for (const auto& [id, t] : timers) if (now >= t.next) due.push_back(id);
    for (int id : due) {
        auto it = timers.find(id);
        if (it == timers.end()) continue;
        Timer t = it->second;
        if (t.once) timers.erase(it);
        else it->second.next = now + std::max(1.0, t.period);
        gBudget = kInstructionBudget;
        try {
            if (auto timerObj = t.timer.lock()) {
                // flash.utils.Timer: handled by the Timer native through its callback
                call(t.fn, Value(timerObj), t.args);
            } else if (t.fn.isObject()) {
                call(t.fn, Value::null(), t.args);
            }
        } catch (const ScriptException& e) {
            reportError("timer", e);
        }
    }
}

} // namespace fp::avm2

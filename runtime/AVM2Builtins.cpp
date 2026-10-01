#include "AVM2.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <limits>

namespace fp::avm2 {
namespace {

Value arg(const Args& a, std::size_t i) { return i < a.size() ? a[i] : Value(); }
double num(VM& vm, const Args& a, std::size_t i, double def = 0) { return i < a.size() ? vm.toNumber(a[i]) : def; }

std::shared_ptr<ArrayObject> asArray(const Value& v) { return v.isObject() ? std::dynamic_pointer_cast<ArrayObject>(v.o) : nullptr; }

constexpr int kCaseInsensitive = 1, kDescending = 2, kUniqueSort = 4, kReturnIndexedArray = 8, kNumeric = 16;

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Array.sort / sortOn comparison following the Flash rules (undefined sorts last).
int compareValues(VM& vm, const Value& x, const Value& y, int options, const Value& fn) {
    if (fn.isObject()) {
        const double r = vm.toNumber(vm.call(fn, Value::null(), {x, y}));
        return r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    if (x.isUndefined() || y.isUndefined()) return x.isUndefined() == y.isUndefined() ? 0 : x.isUndefined() ? 1 : -1;
    if (options & kNumeric) {
        const double a = vm.toNumber(x), b = vm.toNumber(y);
        return a < b ? -1 : a > b ? 1 : 0;
    }
    std::string a = vm.toString(x), b = vm.toString(y);
    if (options & kCaseInsensitive) { a = lowerStr(a); b = lowerStr(b); }
    return a < b ? -1 : a > b ? 1 : 0;
}

Value sortArray(VM& vm, const std::shared_ptr<ArrayObject>& arr, std::vector<Value> keys, int options, const Value& fn) {
    std::vector<std::size_t> order(arr->items.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t i, std::size_t j) {
        int c = compareValues(vm, keys[i], keys[j], options, fn);
        if (options & kDescending) c = -c;
        return c < 0;
    });
    if (options & kUniqueSort) {
        for (std::size_t i = 1; i < order.size(); ++i) if (compareValues(vm, keys[order[i - 1]], keys[order[i]], options, fn) == 0) return Value(0);
    }
    if (options & kReturnIndexedArray) {
        std::vector<Value> idx;
        for (auto i : order) idx.emplace_back(static_cast<double>(i));
        return Value(vm.newArray(idx));
    }
    std::vector<Value> sorted;
    for (auto i : order) sorted.push_back(arr->items[i]);
    arr->items = std::move(sorted);
    return Value(arr);
}

std::string numberToRadix(double v, int radix) {
    if (radix == 10 || radix < 2 || radix > 36) return {};
    long long n = static_cast<long long>(v);
    const bool neg = n < 0;
    if (neg) n = -n;
    std::string s;
    do { s.insert(s.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[n % radix]); n /= radix; } while (n);
    return neg ? "-" + s : s;
}

std::string utf8Encode(std::uint32_t c) {
    std::string s;
    if (c < 0x80) s.push_back(static_cast<char>(c));
    else if (c < 0x800) { s.push_back(static_cast<char>(0xc0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    else { s.push_back(static_cast<char>(0xe0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); s.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    return s;
}

struct DateData : NativeData { double time = 0; };

double dateOf(const Value& self) {
    if (!self.isObject()) return 0;
    auto d = std::dynamic_pointer_cast<DateData>(self.o->native);
    return d ? d->time : 0;
}

} // namespace

void installBuiltinsImpl(VM& vm) {
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // ---- Object, Class, Function (bootstrap order matters)
    vm.objectClass = vm.defineNativeClass("", "Object", nullptr);
    vm.classClass = vm.defineNativeClass("", "Class", vm.objectClass);
    vm.functionClass = vm.defineNativeClass("", "Function", vm.objectClass);
    for (auto& c : {vm.objectClass, vm.classClass, vm.functionClass}) { c->cls = vm.classClass; c->proto = vm.classClass->prototype; }
    vm.objectClass->prototype->proto = nullptr;
    vm.classClass->prototype->proto = vm.objectClass->prototype;
    vm.functionClass->prototype->proto = vm.objectClass->prototype;
    vm.toplevel->cls = vm.objectClass;
    vm.toplevel->proto = vm.objectClass->prototype;

    auto objProto = vm.objectClass->prototype;
    auto protoFn = [&](const ObjectPtr& proto, const std::string& name, NativeFn fn) { proto->dynamic.set(name, Value(vm.newFunction(std::move(fn)))); };
    protoFn(objProto, "hasOwnProperty", [](VM& vm, const Value& self, Args& a) {
        if (!self.isObject()) return Value(false);
        Value key = arg(a, 0);
        const auto name = vm.toString(key);
        if (auto arr = asArray(self)) {
            const double d = vm.toNumber(key);
            if (d >= 0 && d == std::floor(d) && d < arr->items.size()) return Value(true);
        }
        if (auto dict = std::dynamic_pointer_cast<DictionaryObject>(self.o)) return Value(dict->indexOf(vm, key) >= 0);
        if (self.o->traits && self.o->traits->findPublic(name)) return Value(true);
        return Value(self.o->dynamic.find(name) != nullptr);
    });
    protoFn(objProto, "propertyIsEnumerable", [](VM& vm, const Value& self, Args& a) {
        return Value(self.isObject() && self.o->dynamic.find(vm.toString(arg(a, 0))) != nullptr);
    });
    protoFn(objProto, "isPrototypeOf", [](VM&, const Value& self, Args& a) {
        const Value o = arg(a, 0);
        if (!self.isObject() || !o.isObject()) return Value(false);
        for (Object* p = o.o->proto.get(); p; p = p->proto.get()) if (p == self.o.get()) return Value(true);
        return Value(false);
    });
    protoFn(objProto, "toString", [](VM&, const Value& self, Args&) {
        if (self.isObject() && self.o->cls) return Value("[object " + self.o->cls->name + "]");
        return Value("[object Object]");
    });
    protoFn(objProto, "toLocaleString", [](VM& vm, const Value& self, Args&) { return Value(vm.toString(self)); });
    protoFn(objProto, "valueOf", [](VM&, const Value& self, Args&) { return self; });
    protoFn(objProto, "setPropertyIsEnumerable", [](VM&, const Value&, Args&) { return Value(); });
    vm.objectClass->callAsFunction = [](VM& vm, Args& a) { return a.empty() || a[0].isNullish() ? Value(vm.newObject()) : a[0]; };

    auto fnProto = vm.functionClass->prototype;
    protoFn(fnProto, "call", [](VM& vm, const Value& self, Args& a) {
        Args rest(a.size() > 1 ? a.begin() + 1 : a.end(), a.end());
        return vm.call(self, arg(a, 0), rest);
    });
    protoFn(fnProto, "apply", [](VM& vm, const Value& self, Args& a) {
        Args rest;
        if (auto arr = asArray(arg(a, 1))) rest = arr->items;
        return vm.call(self, arg(a, 0), rest);
    });
    ClassBuilder{vm, vm.functionClass}.getter("length", [](VM& vm, const Value& self, Args&) {
        if (auto f = self.isObject() ? std::dynamic_pointer_cast<FunctionObject>(self.o) : nullptr) {
            if (f->method.abc) return Value(static_cast<double>(f->method.abc->methods[f->method.index].paramCount));
        }
        (void)vm;
        return Value(0);
    });

    // ---- Namespace / QName (rarely used; enough for `new Namespace(uri)`)
    vm.namespaceClass = vm.defineNativeClass("", "Namespace", vm.objectClass);
    ClassBuilder{vm, vm.namespaceClass}.ctor([](VM& vm, const Value& self, Args& a) {
        if (self.isObject()) self.o->dynamic.set("uri", Value(vm.toString(arg(a, a.size() > 1 ? 1 : 0))));
        return Value();
    });

    // ---- Array
    vm.arrayClass = vm.defineNativeClass("", "Array", vm.objectClass);
    vm.arrayClass->allocator = [](VM&) -> ObjectPtr { return std::make_shared<ArrayObject>(); };
    vm.arrayClass->callAsFunction = [](VM& vm, Args& a) {
        if (a.size() == 1 && a[0].isNumber()) { auto arr = vm.newArray(); arr->items.resize(static_cast<std::size_t>(std::max(0.0, a[0].n))); return Value(arr); }
        return Value(vm.newArray(a));
    };
    ClassBuilder ab{vm, vm.arrayClass};
    ab.ctor([](VM&, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        if (a.size() == 1 && a[0].isNumber()) arr->items.resize(static_cast<std::size_t>(std::max(0.0, std::min(a[0].n, 1e8))));
        else arr->items = a;
        return Value();
    });
    ab.constant("CASEINSENSITIVE", Value(1)).constant("DESCENDING", Value(2)).constant("UNIQUESORT", Value(4))
        .constant("RETURNINDEXEDARRAY", Value(8)).constant("NUMERIC", Value(16));
    ab.property("length",
        [](VM&, const Value& self, Args&) { auto arr = asArray(self); return Value(arr ? static_cast<double>(arr->items.size()) : 0.0); },
        [](VM& vm, const Value& self, Args& a) { if (auto arr = asArray(self)) arr->items.resize(static_cast<std::size_t>(std::max(0.0, vm.toNumber(arg(a, 0))))); return Value(); });
    ab.method("push", [](VM&, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        for (auto& v : a) arr->items.push_back(v);
        return Value(static_cast<double>(arr->items.size()));
    });
    ab.method("pop", [](VM&, const Value& self, Args&) {
        auto arr = asArray(self);
        if (!arr || arr->items.empty()) return Value();
        Value v = arr->items.back();
        arr->items.pop_back();
        return v;
    });
    ab.method("shift", [](VM&, const Value& self, Args&) {
        auto arr = asArray(self);
        if (!arr || arr->items.empty()) return Value();
        Value v = arr->items.front();
        arr->items.erase(arr->items.begin());
        return v;
    });
    ab.method("unshift", [](VM&, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        arr->items.insert(arr->items.begin(), a.begin(), a.end());
        return Value(static_cast<double>(arr->items.size()));
    });
    ab.method("splice", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        const long n = static_cast<long>(arr->items.size());
        long start = static_cast<long>(num(vm, a, 0));
        if (start < 0) start = std::max(0L, n + start);
        start = std::min(start, n);
        long count = a.size() > 1 ? static_cast<long>(vm.toNumber(a[1])) : n - start;
        count = std::clamp(count, 0L, n - start);
        std::vector<Value> removed(arr->items.begin() + start, arr->items.begin() + start + count);
        arr->items.erase(arr->items.begin() + start, arr->items.begin() + start + count);
        if (a.size() > 2) arr->items.insert(arr->items.begin() + start, a.begin() + 2, a.end());
        return Value(vm.newArray(removed));
    });
    ab.method("slice", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        const long n = static_cast<long>(arr->items.size());
        long s = a.empty() ? 0 : static_cast<long>(vm.toNumber(a[0]));
        long e = a.size() > 1 ? static_cast<long>(vm.toNumber(a[1])) : n;
        if (s < 0) s = std::max(0L, n + s);
        if (e < 0) e = std::max(0L, n + e);
        s = std::min(s, n);
        e = std::clamp(e, s, n);
        return Value(vm.newArray({arr->items.begin() + s, arr->items.begin() + e}));
    });
    ab.method("concat", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        auto out = vm.newArray(arr ? arr->items : std::vector<Value>{});
        for (auto& v : a) {
            if (auto other = asArray(v)) out->items.insert(out->items.end(), other->items.begin(), other->items.end());
            else out->items.push_back(v);
        }
        return Value(out);
    });
    ab.method("join", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value("");
        const std::string sep = a.empty() || a[0].isUndefined() ? "," : vm.toString(a[0]);
        std::string out;
        for (std::size_t i = 0; i < arr->items.size(); ++i) {
            if (i) out += sep;
            if (!arr->items[i].isNullish()) out += vm.toString(arr->items[i]);
        }
        return Value(out);
    });
    ab.method("toString", [](VM& vm, const Value& self, Args&) { return Value(vm.toString(vm.toPrimitive(self, true))); });
    ab.method("reverse", [](VM&, const Value& self, Args&) {
        if (auto arr = asArray(self)) std::reverse(arr->items.begin(), arr->items.end());
        return self;
    });
    ab.method("indexOf", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value(-1);
        for (std::size_t i = static_cast<std::size_t>(std::max(0.0, num(vm, a, 1))); i < arr->items.size(); ++i) {
            if (vm.strictEquals(arr->items[i], arg(a, 0))) return Value(static_cast<double>(i));
        }
        return Value(-1);
    });
    ab.method("lastIndexOf", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value(-1);
        for (std::size_t i = arr->items.size(); i-- > 0;) if (vm.strictEquals(arr->items[i], arg(a, 0))) return Value(static_cast<double>(i));
        return Value(-1);
    });
    auto iterate = [](const char* kind) {
        return [kind](VM& vm, const Value& self, Args& a) -> Value {
            auto arr = asArray(self);
            if (!arr) return Value();
            const Value fn = arg(a, 0), thisObj = arg(a, 1);
            const std::string k = kind;
            auto out = vm.newArray();
            const auto items = arr->items;
            for (std::size_t i = 0; i < items.size(); ++i) {
                Value r = vm.call(fn, thisObj, {items[i], Value(static_cast<double>(i)), self});
                if (k == "every" && !vm.toBoolean(r)) return Value(false);
                if (k == "some" && vm.toBoolean(r)) return Value(true);
                if (k == "filter" && vm.toBoolean(r)) out->items.push_back(items[i]);
                if (k == "map") out->items.push_back(r);
            }
            if (k == "every") return Value(true);
            if (k == "some") return Value(false);
            if (k == "forEach") return Value();
            return Value(out);
        };
    };
    for (const char* k : {"forEach", "map", "filter", "every", "some"}) ab.method(k, iterate(k));
    ab.method("sort", [](VM& vm, const Value& self, Args& a) -> Value {
        auto arr = asArray(self);
        if (!arr) return self;
        Value fn;
        int options = 0;
        if (!a.empty() && a[0].isObject()) { fn = a[0]; options = a.size() > 1 ? vm.toInt32(a[1]) : 0; }
        else if (!a.empty()) options = vm.toInt32(a[0]);
        return sortArray(vm, arr, arr->items, options, fn);
    });
    ab.method("sortOn", [](VM& vm, const Value& self, Args& a) -> Value {
        auto arr = asArray(self);
        if (!arr) return self;
        const Value names = arg(a, 0);
        const std::string field = asArray(names) && !asArray(names)->items.empty() ? vm.toString(asArray(names)->items[0]) : vm.toString(names);
        int options = 0;
        if (a.size() > 1) options = asArray(a[1]) && !asArray(a[1])->items.empty() ? vm.toInt32(asArray(a[1])->items[0]) : vm.toInt32(a[1]);
        std::vector<Value> keys;
        for (auto& v : arr->items) keys.push_back(v.isObject() ? vm.getPublic(v, field) : Value());
        return sortArray(vm, arr, keys, options, Value());
    });

    // ---- Vector.<T>: type-erased; behaves like an Array (applytype leaves the base class on the stack)
    {
        auto vector = vm.defineNativeClass("__AS3__.vec", "Vector", vm.arrayClass);
        vector->allocator = [](VM&) -> ObjectPtr { return std::make_shared<ArrayObject>(); };
        vector->callAsFunction = [](VM& vm, Args& a) {
            auto arr = vm.newArray();
            if (auto src = a.empty() ? nullptr : asArray(a[0])) arr->items = src->items;
            return Value(arr);
        };
        ClassBuilder vb{vm, vector};
        vb.ctor([](VM& vm, const Value& self, Args& a) {
            auto arr = asArray(self);
            if (!arr) return Value();
            const double n = a.empty() ? 0.0 : vm.toNumber(a[0]);
            // Elements start as null: it converts to 0 in arithmetic like the numeric defaults do.
            arr->items.assign(static_cast<std::size_t>(std::max(0.0, std::min(std::isnan(n) ? 0.0 : n, 1e8))), Value::null());
            return Value();
        });
        vb.property("fixed", [](VM&, const Value&, Args&) { return Value(false); }, [](VM&, const Value&, Args&) { return Value(); });
        vb.method("insertAt", [](VM& vm, const Value& self, Args& a) {
            if (auto arr = asArray(self)) {
                int i = vm.toInt32(arg(a, 0));
                if (i < 0) i += static_cast<int>(arr->items.size());
                i = std::clamp(i, 0, static_cast<int>(arr->items.size()));
                arr->items.insert(arr->items.begin() + i, arg(a, 1));
            }
            return Value();
        });
        vb.method("removeAt", [](VM& vm, const Value& self, Args& a) {
            auto arr = asArray(self);
            if (!arr) return Value();
            int i = vm.toInt32(arg(a, 0));
            if (i < 0) i += static_cast<int>(arr->items.size());
            if (i < 0 || i >= static_cast<int>(arr->items.size())) return Value();
            Value v = arr->items[static_cast<std::size_t>(i)];
            arr->items.erase(arr->items.begin() + i);
            return v;
        });
        vm.defineGlobal("", "Vector", Value(vector)); // code compiled without the vec package namespace
    }

    // ---- String
    vm.stringClass = vm.defineNativeClass("", "String", vm.objectClass);
    vm.stringClass->callAsFunction = [](VM& vm, Args& a) { return Value(a.empty() ? std::string() : vm.toString(a[0])); };
    ClassBuilder sb{vm, vm.stringClass};
    auto str = [](VM& vm, const Value& self) { return vm.toString(self); };
    sb.getter("length", [str](VM& vm, const Value& self, Args&) { return Value(static_cast<double>(str(vm, self).size())); });
    sb.staticMethod("fromCharCode", [](VM& vm, const Value&, Args& a) {
        std::string s;
        for (auto& v : a) s += utf8Encode(vm.toUint32(v));
        return Value(s);
    });
    sb.method("toString", [str](VM& vm, const Value& self, Args&) { return Value(str(vm, self)); });
    sb.method("valueOf", [str](VM& vm, const Value& self, Args&) { return Value(str(vm, self)); });
    sb.method("charAt", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const double i = num(vm, a, 0);
        return Value(i >= 0 && i < s.size() ? std::string(1, s[static_cast<std::size_t>(i)]) : std::string());
    });
    sb.method("charCodeAt", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const double i = num(vm, a, 0);
        return Value(i >= 0 && i < s.size() ? static_cast<double>(static_cast<unsigned char>(s[static_cast<std::size_t>(i)])) : std::numeric_limits<double>::quiet_NaN());
    });
    sb.method("indexOf", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const auto pos = s.find(vm.toString(arg(a, 0)), static_cast<std::size_t>(std::max(0.0, num(vm, a, 1))));
        return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
    });
    sb.method("lastIndexOf", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const auto pos = s.rfind(vm.toString(arg(a, 0)));
        return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
    });
    sb.method("split", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        auto out = vm.newArray();
        if (a.empty() || a[0].isUndefined()) { out->items.emplace_back(s); return Value(out); }
        const auto sep = vm.toString(a[0]);
        if (sep.empty()) { for (char c : s) out->items.emplace_back(std::string(1, c)); return Value(out); }
        std::size_t start = 0;
        while (true) {
            const auto pos = s.find(sep, start);
            out->items.emplace_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
            if (pos == std::string::npos) break;
            start = pos + sep.size();
        }
        return Value(out);
    });
    sb.method("substr", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const long n = static_cast<long>(s.size());
        long start = static_cast<long>(num(vm, a, 0));
        if (start < 0) start = std::max(0L, n + start);
        start = std::min(start, n);
        long len = a.size() > 1 && !a[1].isUndefined() ? static_cast<long>(vm.toNumber(a[1])) : n - start;
        len = std::clamp(len, 0L, n - start);
        return Value(s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(len)));
    });
    sb.method("substring", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const long n = static_cast<long>(s.size());
        long x = std::clamp(static_cast<long>(num(vm, a, 0)), 0L, n);
        long y = a.size() > 1 && !a[1].isUndefined() ? std::clamp(static_cast<long>(vm.toNumber(a[1])), 0L, n) : n;
        if (x > y) std::swap(x, y);
        return Value(s.substr(static_cast<std::size_t>(x), static_cast<std::size_t>(y - x)));
    });
    sb.method("slice", [str](VM& vm, const Value& self, Args& a) {
        const auto s = str(vm, self);
        const long n = static_cast<long>(s.size());
        long x = static_cast<long>(num(vm, a, 0));
        long y = a.size() > 1 && !a[1].isUndefined() ? static_cast<long>(vm.toNumber(a[1])) : n;
        if (x < 0) x = std::max(0L, n + x);
        if (y < 0) y = std::max(0L, n + y);
        x = std::min(x, n);
        y = std::clamp(y, x, n);
        return Value(s.substr(static_cast<std::size_t>(x), static_cast<std::size_t>(y - x)));
    });
    sb.method("toUpperCase", [str](VM& vm, const Value& self, Args&) { auto s = str(vm, self); for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return Value(s); });
    sb.method("toLowerCase", [str](VM& vm, const Value& self, Args&) { return Value(lowerStr(str(vm, self))); });
    sb.method("concat", [str](VM& vm, const Value& self, Args& a) { auto s = str(vm, self); for (auto& v : a) s += vm.toString(v); return Value(s); });
    sb.method("replace", [str](VM& vm, const Value& self, Args& a) {
        auto s = str(vm, self);
        const auto what = vm.toString(arg(a, 0));
        const auto pos = s.find(what);
        if (pos != std::string::npos && !what.empty()) s.replace(pos, what.size(), vm.toString(arg(a, 1)));
        return Value(s);
    });
    sb.method("localeCompare", [str](VM& vm, const Value& self, Args& a) {
        const auto x = str(vm, self), y = vm.toString(arg(a, 0));
        return Value(x < y ? -1 : x > y ? 1 : 0);
    });

    // ---- Number / int / uint / Boolean
    auto numberFormat = [](VM& vm, const Value& self, Args& a) {
        const double v = vm.toNumber(self);
        if (!a.empty() && !a[0].isUndefined()) {
            const auto r = numberToRadix(v, vm.toInt32(a[0]));
            if (!r.empty()) return Value(r);
        }
        return Value(vm.toString(Value(v)));
    };
    auto fixed = [](VM& vm, const Value& self, Args& a) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*f", std::clamp(vm.toInt32(arg(a, 0)), 0, 20), vm.toNumber(self));
        return Value(std::string(buf));
    };
    for (auto* name : {"Number", "int", "uint"}) {
        auto c = vm.defineNativeClass("", name, vm.objectClass);
        ClassBuilder{vm, c}.method("toString", numberFormat).method("toFixed", fixed).method("valueOf", [](VM& vm, const Value& self, Args&) { return Value(vm.toNumber(self)); });
        const std::string n = name;
        if (n == "Number") {
            vm.numberClass = c;
            c->callAsFunction = [](VM& vm, Args& a) { return Value(a.empty() ? 0.0 : vm.toNumber(a[0])); };
            ClassBuilder{vm, c}.constant("NaN", Value(nan)).constant("MAX_VALUE", Value(std::numeric_limits<double>::max()))
                .constant("MIN_VALUE", Value(std::numeric_limits<double>::denorm_min()))
                .constant("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()))
                .constant("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
        } else if (n == "int") {
            vm.intClass = c;
            c->callAsFunction = [](VM& vm, Args& a) { return Value(static_cast<double>(a.empty() ? 0 : vm.toInt32(a[0]))); };
            ClassBuilder{vm, c}.constant("MAX_VALUE", Value(2147483647.0)).constant("MIN_VALUE", Value(-2147483648.0));
        } else {
            vm.uintClass = c;
            c->callAsFunction = [](VM& vm, Args& a) { return Value(static_cast<double>(a.empty() ? 0u : vm.toUint32(a[0]))); };
            ClassBuilder{vm, c}.constant("MAX_VALUE", Value(4294967295.0)).constant("MIN_VALUE", Value(0));
        }
    }
    vm.booleanClass = vm.defineNativeClass("", "Boolean", vm.objectClass);
    vm.booleanClass->callAsFunction = [](VM& vm, Args& a) { return Value(!a.empty() && vm.toBoolean(a[0])); };
    ClassBuilder{vm, vm.booleanClass}.method("toString", [](VM& vm, const Value& self, Args&) { return Value(vm.toString(self)); })
        .method("valueOf", [](VM& vm, const Value& self, Args&) { return Value(vm.toBoolean(self)); });

    // ---- Math
    auto math = vm.newObject();
    vm.defineGlobal("", "Math", Value(math));
    auto mathFn = [&](const char* name, double (*f)(double)) {
        math->dynamic.set(name, Value(vm.newFunction([f](VM& vm, const Value&, Args& a) { return Value(f(num(vm, a, 0, std::numeric_limits<double>::quiet_NaN()))); })));
    };
    mathFn("abs", [](double x) { return std::fabs(x); });
    mathFn("floor", [](double x) { return std::floor(x); });
    mathFn("ceil", [](double x) { return std::ceil(x); });
    mathFn("round", [](double x) { return std::floor(x + 0.5); });
    mathFn("sqrt", [](double x) { return std::sqrt(x); });
    mathFn("sin", [](double x) { return std::sin(x); });
    mathFn("cos", [](double x) { return std::cos(x); });
    mathFn("tan", [](double x) { return std::tan(x); });
    mathFn("asin", [](double x) { return std::asin(x); });
    mathFn("acos", [](double x) { return std::acos(x); });
    mathFn("atan", [](double x) { return std::atan(x); });
    mathFn("exp", [](double x) { return std::exp(x); });
    mathFn("log", [](double x) { return std::log(x); });
    math->dynamic.set("atan2", Value(vm.newFunction([](VM& vm, const Value&, Args& a) { return Value(std::atan2(num(vm, a, 0), num(vm, a, 1))); })));
    math->dynamic.set("pow", Value(vm.newFunction([](VM& vm, const Value&, Args& a) { return Value(std::pow(num(vm, a, 0), num(vm, a, 1))); })));
    math->dynamic.set("random", Value(vm.newFunction([](VM&, const Value&, Args&) { return Value(std::rand() / (static_cast<double>(RAND_MAX) + 1.0)); })));
    math->dynamic.set("min", Value(vm.newFunction([](VM& vm, const Value&, Args& a) {
        double r = std::numeric_limits<double>::infinity();
        for (auto& v : a) { const double x = vm.toNumber(v); if (std::isnan(x)) return Value(x); r = std::min(r, x); }
        return Value(r);
    })));
    math->dynamic.set("max", Value(vm.newFunction([](VM& vm, const Value&, Args& a) {
        double r = -std::numeric_limits<double>::infinity();
        for (auto& v : a) { const double x = vm.toNumber(v); if (std::isnan(x)) return Value(x); r = std::max(r, x); }
        return Value(r);
    })));
    for (auto [n, v] : {std::pair<const char*, double>{"PI", M_PI}, {"E", M_E}, {"SQRT2", M_SQRT2}, {"SQRT1_2", M_SQRT1_2},
                        {"LN2", M_LN2}, {"LN10", M_LN10}, {"LOG2E", M_LOG2E}, {"LOG10E", M_LOG10E}}) {
        math->dynamic.set(n, Value(v));
    }

    // ---- Errors
    vm.errorClass = vm.defineNativeClass("", "Error", vm.objectClass);
    auto errorCtor = [](VM& vm, const Value& self, Args& a) {
        if (self.isObject()) {
            self.o->dynamic.set("message", Value(a.empty() ? std::string() : vm.toString(a[0])));
            self.o->dynamic.set("errorID", a.size() > 1 ? a[1] : Value(0));
            self.o->dynamic.set("name", Value(self.o->cls ? self.o->cls->name : std::string("Error")));
        }
        return Value();
    };
    ClassBuilder{vm, vm.errorClass}.ctor(errorCtor)
        .method("toString", [](VM& vm, const Value& self, Args&) {
            return Value(vm.toString(vm.getPublic(self, "name")) + ": " + vm.toString(vm.getPublic(self, "message")));
        })
        .method("getStackTrace", [](VM&, const Value&, Args&) { return Value::null(); });
    for (auto* n : {"TypeError", "ReferenceError", "RangeError", "ArgumentError", "SecurityError", "SyntaxError",
                    "EvalError", "URIError", "VerifyError", "DefinitionError", "UninitializedError"}) {
        vm.defineNativeClass("", n, vm.errorClass);
    }
    for (auto* n : {"IOError", "EOFError", "IllegalOperationError", "MemoryError", "ScriptTimeoutError", "StackOverflowError"}) {
        vm.defineNativeClass(std::string(n) == "IOError" || std::string(n) == "EOFError" ? "flash.errors" : "flash.errors", n, vm.errorClass);
    }

    // ---- Date (enough for timestamps and simple getters)
    auto dateClass = vm.defineNativeClass("", "Date", vm.objectClass);
    ClassBuilder db{vm, dateClass};
    db.init([](VM&, Object& o) {
        auto d = std::make_shared<DateData>();
        d->time = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        o.native = d;
    });
    db.ctor([](VM& vm, const Value& self, Args& a) {
        if (!self.isObject()) return Value();
        auto d = std::dynamic_pointer_cast<DateData>(self.o->native);
        if (d && a.size() == 1 && a[0].isNumber()) d->time = a[0].n;
        else if (d && a.size() >= 2) {
            std::tm t{};
            t.tm_year = vm.toInt32(a[0]) - 1900;
            t.tm_mon = vm.toInt32(a[1]);
            t.tm_mday = a.size() > 2 ? vm.toInt32(a[2]) : 1;
            t.tm_hour = a.size() > 3 ? vm.toInt32(a[3]) : 0;
            t.tm_min = a.size() > 4 ? vm.toInt32(a[4]) : 0;
            t.tm_sec = a.size() > 5 ? vm.toInt32(a[5]) : 0;
            d->time = static_cast<double>(std::mktime(&t)) * 1000.0 + (a.size() > 6 ? vm.toNumber(a[6]) : 0);
        }
        return Value();
    });
    auto tmField = [](VM&, const Value& self, int which) {
        const double ms = dateOf(self);
        const std::time_t secs = static_cast<std::time_t>(ms / 1000.0);
        std::tm t{};
#ifdef _WIN32
        localtime_s(&t, &secs);
#else
        localtime_r(&secs, &t);
#endif
        const int values[] = {t.tm_year + 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday,
                              static_cast<int>(std::fmod(ms, 1000.0))};
        return Value(static_cast<double>(values[which]));
    };
    const char* names[] = {"fullYear", "month", "date", "hours", "minutes", "seconds", "day", "milliseconds"};
    const char* methods[] = {"getFullYear", "getMonth", "getDate", "getHours", "getMinutes", "getSeconds", "getDay", "getMilliseconds"};
    for (int i = 0; i < 8; ++i) {
        db.getter(names[i], [tmField, i](VM& vm, const Value& self, Args&) { return tmField(vm, self, i); });
        db.method(methods[i], [tmField, i](VM& vm, const Value& self, Args&) { return tmField(vm, self, i); });
    }
    db.method("getTime", [](VM&, const Value& self, Args&) { return Value(dateOf(self)); });
    db.getter("time", [](VM&, const Value& self, Args&) { return Value(dateOf(self)); });
    db.method("valueOf", [](VM&, const Value& self, Args&) { return Value(dateOf(self)); });
    db.method("getTimezoneOffset", [](VM&, const Value&, Args&) { return Value(0); });
    db.method("toString", [](VM&, const Value& self, Args&) {
        const std::time_t secs = static_cast<std::time_t>(dateOf(self) / 1000.0);
        char buf[64];
        std::strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y", std::localtime(&secs));
        return Value(std::string(buf));
    });
    db.staticMethod("now", [](VM&, const Value&, Args&) {
        return Value(static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    });

    // ---- flash.utils.Dictionary and ByteArray (minimal)
    vm.dictionaryClass = vm.defineNativeClass("flash.utils", "Dictionary", vm.objectClass);
    vm.dictionaryClass->allocator = [](VM&) -> ObjectPtr { return std::make_shared<DictionaryObject>(); };
    auto byteArray = vm.defineNativeClass("flash.utils", "ByteArray", vm.objectClass);
    struct Bytes : NativeData { std::vector<std::uint8_t> data; std::size_t pos = 0; };
    ClassBuilder ba{vm, byteArray};
    ba.init([](VM&, Object& o) { o.native = std::make_shared<Bytes>(); });
    auto bytes = [](const Value& self) { return self.isObject() ? std::dynamic_pointer_cast<Bytes>(self.o->native) : nullptr; };
    ba.property("length",
        [bytes](VM&, const Value& self, Args&) { auto b = bytes(self); return Value(b ? static_cast<double>(b->data.size()) : 0.0); },
        [bytes](VM& vm, const Value& self, Args& a) { if (auto b = bytes(self)) b->data.resize(vm.toUint32(arg(a, 0))); return Value(); });
    ba.property("position",
        [bytes](VM&, const Value& self, Args&) { auto b = bytes(self); return Value(b ? static_cast<double>(b->pos) : 0.0); },
        [bytes](VM& vm, const Value& self, Args& a) { if (auto b = bytes(self)) b->pos = vm.toUint32(arg(a, 0)); return Value(); });
    ba.getter("bytesAvailable", [bytes](VM&, const Value& self, Args&) {
        auto b = bytes(self);
        return Value(b && b->pos < b->data.size() ? static_cast<double>(b->data.size() - b->pos) : 0.0);
    });
    auto writeN = [bytes](int n) {
        return [bytes, n](VM& vm, const Value& self, Args& a) {
            auto b = bytes(self);
            if (!b) return Value();
            const std::uint32_t v = vm.toUint32(arg(a, 0));
            for (int i = n - 1; i >= 0; --i) {
                if (b->pos >= b->data.size()) b->data.resize(b->pos + 1);
                b->data[b->pos++] = static_cast<std::uint8_t>(v >> (8 * i));
            }
            return Value();
        };
    };
    ba.method("writeByte", writeN(1)).method("writeShort", writeN(2)).method("writeInt", writeN(4)).method("writeUnsignedInt", writeN(4));
    ba.method("writeUTFBytes", [bytes](VM& vm, const Value& self, Args& a) {
        if (auto b = bytes(self)) for (char c : vm.toString(arg(a, 0))) { if (b->pos >= b->data.size()) b->data.resize(b->pos + 1); b->data[b->pos++] = static_cast<std::uint8_t>(c); }
        return Value();
    });
    auto readN = [bytes](int n, bool sign) {
        return [bytes, n, sign](VM&, const Value& self, Args&) {
            auto b = bytes(self);
            if (!b) return Value(0);
            std::uint32_t v = 0;
            for (int i = 0; i < n; ++i) v = (v << 8) | (b->pos < b->data.size() ? b->data[b->pos++] : 0);
            if (sign && n == 1) return Value(static_cast<double>(static_cast<std::int8_t>(v)));
            if (sign && n == 2) return Value(static_cast<double>(static_cast<std::int16_t>(v)));
            if (sign && n == 4) return Value(static_cast<double>(static_cast<std::int32_t>(v)));
            return Value(static_cast<double>(v));
        };
    };
    ba.method("readByte", readN(1, true)).method("readUnsignedByte", readN(1, false)).method("readShort", readN(2, true))
        .method("readInt", readN(4, true)).method("readUnsignedInt", readN(4, false));
    ba.method("toString", [bytes](VM&, const Value& self, Args&) { auto b = bytes(self); return Value(b ? std::string(b->data.begin(), b->data.end()) : std::string()); });
    for (auto* noop : {"compress", "uncompress", "clear"}) ba.method(noop, [](VM&, const Value&, Args&) { return Value(); });

    // ---- global functions
    vm.defineGlobal("", "NaN", Value(nan));
    vm.defineGlobal("", "Infinity", Value(std::numeric_limits<double>::infinity()));
    vm.defineGlobal("", "undefined", Value());
    vm.defineGlobalFunction("", "trace", [](VM& vm, const Value&, Args& a) {
        std::string s;
        for (std::size_t i = 0; i < a.size(); ++i) s += (i ? " " : "") + vm.toString(a[i]);
        std::cout << "trace: " << s << "\n";
        return Value();
    });
    vm.defineGlobalFunction("", "isNaN", [](VM& vm, const Value&, Args& a) { return Value(std::isnan(num(vm, a, 0, std::numeric_limits<double>::quiet_NaN()))); });
    vm.defineGlobalFunction("", "isFinite", [](VM& vm, const Value&, Args& a) { return Value(std::isfinite(num(vm, a, 0))); });
    vm.defineGlobalFunction("", "parseInt", [](VM& vm, const Value&, Args& a) {
        const std::string s = vm.toString(arg(a, 0));
        int radix = a.size() > 1 ? vm.toInt32(a[1]) : 0;
        const char* p = s.c_str();
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if ((radix == 0 || radix == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
        if (radix == 0) radix = 10;
        if (radix < 2 || radix > 36) return Value(std::numeric_limits<double>::quiet_NaN());
        char* end = nullptr;
        const long long v = std::strtoll(p, &end, radix);
        return Value(end == p ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>(v));
    });
    vm.defineGlobalFunction("", "parseFloat", [](VM& vm, const Value&, Args& a) {
        const std::string s = vm.toString(arg(a, 0));
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        return Value(end == s.c_str() ? std::numeric_limits<double>::quiet_NaN() : v);
    });
    auto escapeFn = [](bool component) {
        return [component](VM& vm, const Value&, Args& a) {
            std::string out;
            for (unsigned char c : vm.toString(arg(a, 0))) {
                const bool keep = std::isalnum(c) || c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/' ||
                                  (component && (c == '!' || c == '~' || c == '\'' || c == '(' || c == ')'));
                if (keep && !(component && (c == '@' || c == '+' || c == '/'))) out.push_back(static_cast<char>(c));
                else { char buf[4]; std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
            }
            return Value(out);
        };
    };
    auto unescapeFn = [](VM& vm, const Value&, Args& a) {
        const std::string s = vm.toString(arg(a, 0));
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) { out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16))); i += 2; }
            else out.push_back(s[i]);
        }
        return Value(out);
    };
    vm.defineGlobalFunction("", "escape", escapeFn(false));
    vm.defineGlobalFunction("", "encodeURIComponent", escapeFn(true));
    vm.defineGlobalFunction("", "encodeURI", escapeFn(true));
    vm.defineGlobalFunction("", "unescape", unescapeFn);
    vm.defineGlobalFunction("", "decodeURIComponent", unescapeFn);
    vm.defineGlobalFunction("", "decodeURI", unescapeFn);

    // XML / RegExp: not used by the target games; provide inert constructors so class
    // definitions that mention them still load.
    for (auto* n : {"RegExp", "QName"}) vm.defineNativeClass("", n, vm.objectClass);
    installXml(vm);
}

} // namespace fp::avm2

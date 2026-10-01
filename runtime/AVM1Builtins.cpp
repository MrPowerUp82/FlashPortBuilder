#include "AVM1.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace fp::avm1 {

// The AVM1 `Sound` object: remembers the attached pack sound, its mix and its live voices.
struct SoundObject : Object {
    ObjectPtr target;
    std::uint32_t soundId = 0;
    double volume = 100, pan = 0;
    std::vector<int> handles;
    bool getOwn(VM& vm, const std::string& name, Value& out) override {
        if (name == "duration" || name == "position") {
            const auto it = vm.player.movie.sounds.find(soundId);
            if (it == vm.player.movie.sounds.end()) { out = Value(0.0); return true; }
            if (name == "duration") { out = Value(it->second.durationMs()); return true; }
            double ms = 0;
            for (int h : handles) ms = std::max(ms, vm.player.audio.positionMs(h));
            out = Value(ms);
            return true;
        }
        return Object::getOwn(vm, name, out);
    }
};

void VM::pollSounds() {
    for (int h : player.audio.takeFinished()) {
        auto it = std::find_if(soundVoices.begin(), soundVoices.end(), [h](const SoundVoice& v) { return v.handle == h; });
        if (it == soundVoices.end()) continue;
        auto owner = it->owner.lock();
        soundVoices.erase(it);
        if (!owner) continue;
        if (auto s = std::dynamic_pointer_cast<SoundObject>(owner)) {
            s->handles.erase(std::remove(s->handles.begin(), s->handles.end(), h), s->handles.end());
        }
        const Value fn = getMember(Value(owner), "onSoundComplete");
        if (fn.isObject() && fn.o->callable()) call(fn, Value(owner), {});
    }
}
namespace {

using Args = std::vector<Value>;

Value arg(const Args& a, std::size_t i) { return i < a.size() ? a[i] : Value(); }

void def(VM& vm, const ObjectPtr& obj, const std::string& name, NativeFunction::Fn fn) {
    obj->props[name] = Value(vm.nativeFunction(std::move(fn)));
}

std::shared_ptr<ArrayObject> asArray(const Value& v) {
    return v.isObject() ? std::dynamic_pointer_cast<ArrayObject>(v.o) : nullptr;
}

double num(VM& vm, const Args& a, std::size_t i) { return vm.toNumber(arg(a, i)); }

// Array.sort / sortOn option flags
constexpr int kCaseInsensitive = 1, kDescending = 2, kNumeric = 16;

void sortValues(VM& vm, std::vector<Value>& items, const Value& compare, int options) {
    auto cmp = [&](const Value& x, const Value& y) {
        if (compare.isObject() && compare.o->callable()) return vm.toNumber(vm.call(compare, Value(), {x, y})) < 0;
        bool r;
        if (options & kNumeric) r = vm.toNumber(x) < vm.toNumber(y);
        else {
            std::string a = vm.toString(x), b = vm.toString(y);
            if (options & kCaseInsensitive) {
                for (auto& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (auto& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            r = a < b;
        }
        return r;
    };
    std::stable_sort(items.begin(), items.end(), cmp);
    if (options & kDescending) std::reverse(items.begin(), items.end());
}

} // namespace

int flashKeyCode(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 112 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 97 + (k - SDLK_KP_1);
    switch (k) {
        case SDLK_KP_0: return 96;
        case SDLK_BACKSPACE: return 8;
        case SDLK_TAB: return 9;
        case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 16;
        case SDLK_LCTRL: case SDLK_RCTRL: return 17;
        case SDLK_LALT: case SDLK_RALT: return 18;
        case SDLK_CAPSLOCK: return 20;
        case SDLK_ESCAPE: return 27;
        case SDLK_SPACE: return 32;
        case SDLK_PAGEUP: return 33;
        case SDLK_PAGEDOWN: return 34;
        case SDLK_END: return 35;
        case SDLK_HOME: return 36;
        case SDLK_LEFT: return 37;
        case SDLK_UP: return 38;
        case SDLK_RIGHT: return 39;
        case SDLK_DOWN: return 40;
        case SDLK_INSERT: return 45;
        case SDLK_DELETE: return 46;
        case SDLK_SEMICOLON: return 186;
        case SDLK_EQUALS: return 187;
        case SDLK_COMMA: return 188;
        case SDLK_MINUS: return 189;
        case SDLK_PERIOD: return 190;
        case SDLK_SLASH: return 191;
        case SDLK_BACKQUOTE: return 192;
        case SDLK_LEFTBRACKET: return 219;
        case SDLK_BACKSLASH: return 220;
        case SDLK_RIGHTBRACKET: return 221;
        case SDLK_QUOTE: return 222;
        default: return 0;
    }
}

void installBuiltins(VM& vm) {
    auto& g = vm.global;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    // ---- Object / Function
    def(vm, vm.objectProto, "toString", [](VM& vm, const Value& self, Args&) { return Value(self.isObject() && vm.typeOf(self) == "object" ? std::string("[object Object]") : vm.toString(self)); });
    def(vm, vm.objectProto, "valueOf", [](VM&, const Value& self, Args&) { return self; });
    def(vm, vm.objectProto, "hasOwnProperty", [](VM& vm, const Value& self, Args& a) {
        if (!self.isObject()) return Value(false);
        Value out;
        return Value(self.o->getOwn(vm, vm.toString(arg(a, 0)), out));
    });
    def(vm, vm.objectProto, "addProperty", [](VM&, const Value&, Args&) { return Value(false); });
    def(vm, vm.objectProto, "watch", [](VM&, const Value&, Args&) { return Value(false); });
    def(vm, vm.objectProto, "unwatch", [](VM&, const Value&, Args&) { return Value(false); });
    def(vm, vm.objectProto, "isPrototypeOf", [](VM&, const Value& self, Args& a) {
        const Value o = arg(a, 0);
        if (!self.isObject() || !o.isObject()) return Value(false);
        for (Object* p = o.o->proto.get(); p; p = p->proto.get()) if (p == self.o.get()) return Value(true);
        return Value(false);
    });
    def(vm, vm.functionProto, "call", [](VM& vm, const Value& self, Args& a) {
        Args rest(a.size() > 1 ? a.begin() + 1 : a.end(), a.end());
        return vm.call(self, arg(a, 0), rest);
    });
    def(vm, vm.functionProto, "apply", [](VM& vm, const Value& self, Args& a) {
        Args rest;
        if (auto arr = asArray(arg(a, 1))) rest = arr->items;
        return vm.call(self, arg(a, 0), rest);
    });

    auto objectCtor = vm.nativeFunction([](VM& vm, const Value& self, Args& a) {
        if (!a.empty() && a[0].isObject()) return a[0];
        return self.isObject() ? self : Value(vm.newObject());
    });
    objectCtor->props["prototype"] = Value(vm.objectProto);
    def(vm, objectCtor, "registerClass", [](VM& vm, const Value&, Args& a) {
        const auto it = vm.player.movie.exports.find(vm.toString(arg(a, 0)));
        if (it == vm.player.movie.exports.end()) return Value(false);
        if (arg(a, 1).isObject()) vm.registeredClasses[it->second] = arg(a, 1);
        else vm.registeredClasses.erase(it->second);
        return Value(true);
    });
    g->props["Object"] = Value(objectCtor);
    auto functionCtor = vm.nativeFunction([](VM&, const Value&, Args&) { return Value(); });
    functionCtor->props["prototype"] = Value(vm.functionProto);
    g->props["Function"] = Value(functionCtor);

    // ---- Array
    auto arrayCtor = vm.nativeFunction([](VM& vm, const Value&, Args& a) {
        if (a.size() == 1 && a[0].isNumber()) {
            auto arr = vm.newArray();
            arr->items.resize(static_cast<std::size_t>(std::max(0.0, std::min(a[0].n, 1e7))));
            return Value(std::static_pointer_cast<Object>(arr));
        }
        return Value(std::static_pointer_cast<Object>(vm.newArray(a)));
    });
    arrayCtor->props["prototype"] = Value(vm.arrayProto);
    arrayCtor->props["CASEINSENSITIVE"] = Value(1);
    arrayCtor->props["DESCENDING"] = Value(2);
    arrayCtor->props["UNIQUESORT"] = Value(4);
    arrayCtor->props["RETURNINDEXEDARRAY"] = Value(8);
    arrayCtor->props["NUMERIC"] = Value(16);
    g->props["Array"] = Value(arrayCtor);
    auto& ap = vm.arrayProto;
    def(vm, ap, "push", [](VM&, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        for (auto& v : a) arr->items.push_back(v);
        return Value(static_cast<double>(arr->items.size()));
    });
    def(vm, ap, "pop", [](VM&, const Value& self, Args&) {
        auto arr = asArray(self);
        if (!arr || arr->items.empty()) return Value();
        Value v = arr->items.back();
        arr->items.pop_back();
        return v;
    });
    def(vm, ap, "shift", [](VM&, const Value& self, Args&) {
        auto arr = asArray(self);
        if (!arr || arr->items.empty()) return Value();
        Value v = arr->items.front();
        arr->items.erase(arr->items.begin());
        return v;
    });
    def(vm, ap, "unshift", [](VM&, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        arr->items.insert(arr->items.begin(), a.begin(), a.end());
        return Value(static_cast<double>(arr->items.size()));
    });
    def(vm, ap, "splice", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        const auto n = static_cast<long>(arr->items.size());
        long start = static_cast<long>(vm.toNumber(arg(a, 0)));
        if (start < 0) start = std::max(0L, n + start);
        start = std::min(start, n);
        long count = a.size() > 1 ? static_cast<long>(vm.toNumber(a[1])) : n - start;
        count = std::clamp(count, 0L, n - start);
        std::vector<Value> removed(arr->items.begin() + start, arr->items.begin() + start + count);
        arr->items.erase(arr->items.begin() + start, arr->items.begin() + start + count);
        if (a.size() > 2) arr->items.insert(arr->items.begin() + start, a.begin() + 2, a.end());
        return Value(std::static_pointer_cast<Object>(vm.newArray(removed)));
    });
    def(vm, ap, "slice", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return Value();
        const auto n = static_cast<long>(arr->items.size());
        long s = a.empty() ? 0 : static_cast<long>(vm.toNumber(a[0]));
        long e = a.size() > 1 && !a[1].isUndefined() ? static_cast<long>(vm.toNumber(a[1])) : n;
        if (s < 0) s = std::max(0L, n + s);
        if (e < 0) e = std::max(0L, n + e);
        s = std::min(s, n);
        e = std::clamp(e, s, n);
        return Value(std::static_pointer_cast<Object>(vm.newArray({arr->items.begin() + s, arr->items.begin() + e})));
    });
    def(vm, ap, "join", [](VM& vm, const Value& self, Args& a) {
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
    def(vm, ap, "toString", [](VM& vm, const Value& self, Args&) { return Value(vm.toString(self)); });
    def(vm, ap, "reverse", [](VM&, const Value& self, Args&) {
        if (auto arr = asArray(self)) std::reverse(arr->items.begin(), arr->items.end());
        return self;
    });
    def(vm, ap, "concat", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        auto out = vm.newArray(arr ? arr->items : std::vector<Value>{});
        for (auto& v : a) {
            if (auto other = asArray(v)) out->items.insert(out->items.end(), other->items.begin(), other->items.end());
            else out->items.push_back(v);
        }
        return Value(std::static_pointer_cast<Object>(out));
    });
    def(vm, ap, "sort", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return self;
        Value compare;
        int options = 0;
        if (!a.empty() && a[0].isObject()) { compare = a[0]; options = static_cast<int>(vm.toNumber(arg(a, 1))); }
        else if (!a.empty()) options = static_cast<int>(vm.toNumber(a[0]));
        if (options < 0 || std::isnan(static_cast<double>(options))) options = 0;
        sortValues(vm, arr->items, compare, options);
        return self;
    });
    def(vm, ap, "sortOn", [](VM& vm, const Value& self, Args& a) {
        auto arr = asArray(self);
        if (!arr) return self;
        const std::string field = vm.toString(arg(a, 0));
        const int options = a.size() > 1 ? static_cast<int>(vm.toNumber(a[1])) : 0;
        std::stable_sort(arr->items.begin(), arr->items.end(), [&](const Value& x, const Value& y) {
            const Value fx = vm.getMember(x, field), fy = vm.getMember(y, field);
            if (options & kNumeric) return vm.toNumber(fx) < vm.toNumber(fy);
            return vm.toString(fx) < vm.toString(fy);
        });
        if (options & kDescending) std::reverse(arr->items.begin(), arr->items.end());
        return self;
    });

    // ---- String
    auto stringCtor = vm.nativeFunction([](VM& vm, const Value&, Args& a) { return Value(a.empty() ? std::string() : vm.toString(a[0])); });
    stringCtor->props["prototype"] = Value(vm.stringProto);
    def(vm, stringCtor, "fromCharCode", [](VM& vm, const Value&, Args& a) {
        std::string s;
        for (auto& v : a) {
            const auto c = static_cast<std::uint32_t>(vm.toNumber(v));
            if (c < 0x80) s.push_back(static_cast<char>(c));
            else if (c < 0x800) { s.push_back(static_cast<char>(0xc0 | (c >> 6))); s.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
            else { s.push_back(static_cast<char>(0xe0 | (c >> 12))); s.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); s.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        }
        return Value(s);
    });
    g->props["String"] = Value(stringCtor);
    auto& sp = vm.stringProto;
    def(vm, sp, "toString", [](VM& vm, const Value& self, Args&) { return Value(vm.toString(self)); });
    def(vm, sp, "valueOf", [](VM& vm, const Value& self, Args&) { return Value(vm.toString(self)); });
    def(vm, sp, "split", [](VM& vm, const Value& self, Args& a) {
        const std::string s = vm.toString(self);
        auto out = vm.newArray();
        if (a.empty() || a[0].isUndefined()) { out->items.emplace_back(s); return Value(std::static_pointer_cast<Object>(out)); }
        const std::string sep = vm.toString(a[0]);
        if (sep.empty()) {
            for (char c : s) out->items.emplace_back(std::string(1, c));
        } else {
            std::size_t start = 0;
            while (true) {
                const auto pos = s.find(sep, start);
                out->items.emplace_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
                if (pos == std::string::npos) break;
                start = pos + sep.size();
            }
        }
        return Value(std::static_pointer_cast<Object>(out));
    });
    def(vm, sp, "substr", [](VM& vm, const Value& self, Args& a) {
        const std::string s = vm.toString(self);
        const long n = static_cast<long>(s.size());
        long start = static_cast<long>(vm.toNumber(arg(a, 0)));
        if (start < 0) start = std::max(0L, n + start);
        start = std::min(start, n);
        long len = a.size() > 1 && !a[1].isUndefined() ? static_cast<long>(vm.toNumber(a[1])) : n - start;
        len = std::clamp(len, 0L, n - start);
        return Value(s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(len)));
    });
    auto substring = [](VM& vm, const Value& self, Args& a) {
        const std::string s = vm.toString(self);
        const long n = static_cast<long>(s.size());
        long x = std::clamp(static_cast<long>(vm.toNumber(arg(a, 0))), 0L, n);
        long y = a.size() > 1 && !a[1].isUndefined() ? std::clamp(static_cast<long>(vm.toNumber(a[1])), 0L, n) : n;
        if (x > y) std::swap(x, y);
        return Value(s.substr(static_cast<std::size_t>(x), static_cast<std::size_t>(y - x)));
    };
    def(vm, sp, "substring", substring);
    def(vm, sp, "slice", substring);
    def(vm, sp, "charAt", [](VM& vm, const Value& self, Args& a) {
        const std::string s = vm.toString(self);
        const double i = vm.toNumber(arg(a, 0));
        return Value(i >= 0 && i < s.size() ? std::string(1, s[static_cast<std::size_t>(i)]) : std::string());
    });
    def(vm, sp, "charCodeAt", [](VM& vm, const Value& self, Args& a) {
        const std::string s = vm.toString(self);
        const double i = vm.toNumber(arg(a, 0));
        return Value(i >= 0 && i < s.size() ? static_cast<double>(static_cast<unsigned char>(s[static_cast<std::size_t>(i)]))
                                            : std::numeric_limits<double>::quiet_NaN());
    });
    def(vm, sp, "indexOf", [](VM& vm, const Value& self, Args& a) {
        const std::string s = vm.toString(self);
        const auto from = static_cast<std::size_t>(std::max(0.0, vm.toNumber(a.size() > 1 ? a[1] : Value(0))));
        const auto pos = s.find(vm.toString(arg(a, 0)), from);
        return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
    });
    def(vm, sp, "lastIndexOf", [](VM& vm, const Value& self, Args& a) {
        const auto pos = vm.toString(self).rfind(vm.toString(arg(a, 0)));
        return Value(pos == std::string::npos ? -1.0 : static_cast<double>(pos));
    });
    def(vm, sp, "toUpperCase", [](VM& vm, const Value& self, Args&) {
        std::string s = vm.toString(self);
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value(s);
    });
    def(vm, sp, "toLowerCase", [](VM& vm, const Value& self, Args&) {
        std::string s = vm.toString(self);
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return Value(s);
    });
    def(vm, sp, "concat", [](VM& vm, const Value& self, Args& a) {
        std::string s = vm.toString(self);
        for (auto& v : a) s += vm.toString(v);
        return Value(s);
    });

    // ---- Number / Boolean / conversions
    auto numberCtor = vm.nativeFunction([](VM& vm, const Value&, Args& a) { return Value(a.empty() ? 0.0 : vm.toNumber(a[0])); });
    numberCtor->props["MAX_VALUE"] = Value(std::numeric_limits<double>::max());
    numberCtor->props["MIN_VALUE"] = Value(std::numeric_limits<double>::denorm_min());
    numberCtor->props["NaN"] = Value(nan);
    numberCtor->props["POSITIVE_INFINITY"] = Value(std::numeric_limits<double>::infinity());
    numberCtor->props["NEGATIVE_INFINITY"] = Value(-std::numeric_limits<double>::infinity());
    g->props["Number"] = Value(numberCtor);
    def(vm, g, "Boolean", [](VM& vm, const Value&, Args& a) { return Value(vm.toBoolean(arg(a, 0))); });
    g->props["NaN"] = Value(nan);
    g->props["Infinity"] = Value(std::numeric_limits<double>::infinity());
    def(vm, g, "isNaN", [](VM& vm, const Value&, Args& a) { return Value(std::isnan(vm.toNumber(arg(a, 0)))); });
    def(vm, g, "isFinite", [](VM& vm, const Value&, Args& a) { return Value(std::isfinite(vm.toNumber(arg(a, 0)))); });
    def(vm, g, "parseInt", [](VM& vm, const Value&, Args& a) {
        const std::string s = vm.toString(arg(a, 0));
        int radix = a.size() > 1 ? static_cast<int>(vm.toNumber(a[1])) : 10;
        const char* p = s.c_str();
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if ((a.size() < 2 || radix == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
        if (radix < 2 || radix > 36) radix = 10;
        char* end = nullptr;
        const long long v = std::strtoll(p, &end, radix);
        return Value(end == p ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>(v));
    });
    def(vm, g, "parseFloat", [](VM& vm, const Value&, Args& a) {
        const std::string s = vm.toString(arg(a, 0));
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        return Value(end == s.c_str() ? std::numeric_limits<double>::quiet_NaN() : v);
    });
    def(vm, g, "escape", [](VM& vm, const Value&, Args& a) {
        std::string out;
        for (unsigned char c : vm.toString(arg(a, 0))) {
            if (std::isalnum(c) || c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/') out.push_back(static_cast<char>(c));
            else { char buf[4]; std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
        }
        return Value(out);
    });
    def(vm, g, "unescape", [](VM& vm, const Value&, Args& a) {
        const std::string s = vm.toString(arg(a, 0));
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) { out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16))); i += 2; }
            else out.push_back(s[i]);
        }
        return Value(out);
    });
    def(vm, g, "trace", [](VM& vm, const Value&, Args& a) { std::cout << "trace: " << vm.toString(arg(a, 0)) << "\n"; return Value(); });
    def(vm, g, "getTimer", [](VM& vm, const Value&, Args&) { return Value(std::floor(vm.nowMs())); });
    def(vm, g, "random", [](VM& vm, const Value&, Args& a) { const int n = vm.toInt32(arg(a, 0)); return Value(n > 0 ? static_cast<double>(std::rand() % n) : 0.0); });
    def(vm, g, "ASSetPropFlags", [](VM&, const Value&, Args&) { return Value(); });
    def(vm, g, "updateAfterEvent", [](VM&, const Value&, Args&) { return Value(); });
    def(vm, g, "fscommand", [](VM&, const Value&, Args&) { return Value(); });
    def(vm, g, "getVersion", [](VM&, const Value&, Args&) { return Value("WIN 10,0,0,0"); });

    // ---- timers
    auto addInterval = [](VM& vm, Args& a, bool once) {
        VM::Interval iv;
        std::size_t i = 0;
        if (a.size() >= 2 && a[0].isObject() && !a[0].o->callable() && a[1].isString()) {
            iv.thisv = a[0];
            iv.method = a[1].s;
            i = 2;
        } else {
            iv.fn = arg(a, 0);
            i = 1;
        }
        iv.period = std::max(1.0, vm.toNumber(arg(a, i)));
        iv.args.assign(a.size() > i + 1 ? a.begin() + static_cast<std::ptrdiff_t>(i + 1) : a.end(), a.end());
        iv.next = vm.nowMs() + iv.period;
        iv.once = once;
        const int id = vm.nextInterval++;
        vm.intervals[id] = std::move(iv);
        return Value(id);
    };
    def(vm, g, "setInterval", [addInterval](VM& vm, const Value&, Args& a) { return addInterval(vm, a, false); });
    def(vm, g, "setTimeout", [addInterval](VM& vm, const Value&, Args& a) { return addInterval(vm, a, true); });
    auto clear = [](VM& vm, const Value&, Args& a) { vm.intervals.erase(vm.toInt32(arg(a, 0))); return Value(); };
    def(vm, g, "clearInterval", clear);
    def(vm, g, "clearTimeout", clear);

    // ---- Math
    auto math = vm.newObject();
    g->props["Math"] = Value(math);
    math->props["PI"] = Value(M_PI);
    math->props["E"] = Value(M_E);
    math->props["SQRT2"] = Value(M_SQRT2);
    math->props["SQRT1_2"] = Value(M_SQRT1_2);
    math->props["LN2"] = Value(M_LN2);
    math->props["LN10"] = Value(M_LN10);
    math->props["LOG2E"] = Value(M_LOG2E);
    math->props["LOG10E"] = Value(M_LOG10E);
    auto unary = [&](const char* name, double (*f)(double)) {
        def(vm, math, name, [f](VM& vm, const Value&, Args& a) { return Value(f(num(vm, a, 0))); });
    };
    unary("abs", [](double x) { return std::fabs(x); });
    unary("floor", [](double x) { return std::floor(x); });
    unary("ceil", [](double x) { return std::ceil(x); });
    unary("round", [](double x) { return std::floor(x + 0.5); });
    unary("sqrt", [](double x) { return std::sqrt(x); });
    unary("sin", [](double x) { return std::sin(x); });
    unary("cos", [](double x) { return std::cos(x); });
    unary("tan", [](double x) { return std::tan(x); });
    unary("asin", [](double x) { return std::asin(x); });
    unary("acos", [](double x) { return std::acos(x); });
    unary("atan", [](double x) { return std::atan(x); });
    unary("exp", [](double x) { return std::exp(x); });
    unary("log", [](double x) { return std::log(x); });
    def(vm, math, "atan2", [](VM& vm, const Value&, Args& a) { return Value(std::atan2(num(vm, a, 0), num(vm, a, 1))); });
    def(vm, math, "pow", [](VM& vm, const Value&, Args& a) { return Value(std::pow(num(vm, a, 0), num(vm, a, 1))); });
    def(vm, math, "random", [](VM&, const Value&, Args&) { return Value(std::rand() / (static_cast<double>(RAND_MAX) + 1.0)); });
    def(vm, math, "min", [](VM& vm, const Value&, Args& a) {
        double r = std::numeric_limits<double>::infinity();
        for (auto& v : a) { const double x = vm.toNumber(v); if (std::isnan(x)) return Value(x); r = std::min(r, x); }
        return Value(r);
    });
    def(vm, math, "max", [](VM& vm, const Value&, Args& a) {
        double r = -std::numeric_limits<double>::infinity();
        for (auto& v : a) { const double x = vm.toNumber(v); if (std::isnan(x)) return Value(x); r = std::max(r, x); }
        return Value(r);
    });

    // ---- Key
    auto key = vm.newObject();
    g->props["Key"] = Value(key);
    const std::pair<const char*, int> keyNames[] = {
        {"BACKSPACE", 8}, {"TAB", 9}, {"ENTER", 13}, {"SHIFT", 16}, {"CONTROL", 17}, {"ALT", 18}, {"CAPSLOCK", 20},
        {"ESCAPE", 27}, {"SPACE", 32}, {"PGUP", 33}, {"PGDN", 34}, {"END", 35}, {"HOME", 36}, {"LEFT", 37},
        {"UP", 38}, {"RIGHT", 39}, {"DOWN", 40}, {"INSERT", 45}, {"DELETEKEY", 46}};
    for (const auto& [n, c] : keyNames) key->props[n] = Value(c);
    def(vm, key, "isDown", [](VM& vm, const Value&, Args& a) {
        const int code = vm.toInt32(arg(a, 0));
        return Value(code > 0 && code < 256 && vm.player.keys[code]);
    });
    def(vm, key, "getCode", [](VM& vm, const Value&, Args&) { return Value(vm.player.lastKey); });
    def(vm, key, "getAscii", [](VM& vm, const Value&, Args&) {
        const int c = vm.player.lastKey;
        return Value((c >= 'A' && c <= 'Z') ? c + 32 : (c >= 32 && c < 127) ? c : 0);
    });
    def(vm, key, "isToggled", [](VM&, const Value&, Args&) { return Value(false); });
    def(vm, key, "addListener", [](VM& vm, const Value&, Args& a) {
        if (arg(a, 0).isObject()) vm.keyListeners.push_back(a[0].o);
        return Value(true);
    });
    def(vm, key, "removeListener", [](VM& vm, const Value&, Args& a) {
        auto& l = vm.keyListeners;
        const auto before = l.size();
        if (arg(a, 0).isObject()) l.erase(std::remove(l.begin(), l.end(), a[0].o), l.end());
        return Value(l.size() != before);
    });

    // ---- Mouse
    auto mouse = vm.newObject();
    g->props["Mouse"] = Value(mouse);
    def(vm, mouse, "addListener", [](VM& vm, const Value&, Args& a) {
        if (arg(a, 0).isObject()) vm.mouseListeners.push_back(a[0].o);
        return Value(true);
    });
    def(vm, mouse, "removeListener", [](VM& vm, const Value&, Args& a) {
        auto& l = vm.mouseListeners;
        if (arg(a, 0).isObject()) l.erase(std::remove(l.begin(), l.end(), a[0].o), l.end());
        return Value(true);
    });
    def(vm, mouse, "hide", [](VM&, const Value&, Args&) { SDL_ShowCursor(SDL_DISABLE); return Value(0); });
    def(vm, mouse, "show", [](VM&, const Value&, Args&) { SDL_ShowCursor(SDL_ENABLE); return Value(1); });

    // ---- Stage / System
    auto stage = vm.newObject();
    g->props["Stage"] = Value(stage);
    stage->props["width"] = Value(vm.player.movie.stageWidth());
    stage->props["height"] = Value(vm.player.movie.stageHeight());
    stage->props["scaleMode"] = Value("showAll");
    stage->props["align"] = Value("");
    def(vm, stage, "addListener", [](VM&, const Value&, Args&) { return Value(); });
    def(vm, stage, "removeListener", [](VM&, const Value&, Args&) { return Value(); });
    auto system = vm.newObject();
    g->props["System"] = Value(system);
    auto caps = vm.newObject();
    system->props["capabilities"] = Value(caps);
    caps->props["os"] = Value("Windows");
    caps->props["version"] = Value("WIN 10,0,0,0");
    system->props["useCodepage"] = Value(false);
    auto security = vm.newObject();
    system->props["security"] = Value(security);
    def(vm, security, "allowDomain", [](VM&, const Value&, Args&) { return Value(); });

    // ---- SharedObject (kept in memory for the session)
    auto shared = vm.newObject();
    g->props["SharedObject"] = Value(shared);
    auto store = std::make_shared<std::map<std::string, ObjectPtr>>();
    def(vm, shared, "getLocal", [store](VM& vm, const Value&, Args& a) {
        const std::string name = vm.toString(arg(a, 0));
        auto& so = (*store)[name];
        if (!so) {
            so = vm.newObject();
            so->props["data"] = Value(vm.newObject());
            def(vm, so, "flush", [](VM&, const Value&, Args&) { return Value(true); });
            def(vm, so, "clear", [](VM& vm, const Value& self, Args&) {
                if (self.isObject()) self.o->props["data"] = Value(vm.newObject());
                return Value();
            });
            def(vm, so, "getSize", [](VM&, const Value&, Args&) { return Value(0); });
        }
        return Value(so);
    });

    // ---- Sound: plays pack sounds through the audio mixer
    auto soundProto = vm.newObject();
    soundProto->proto = vm.objectProto;
    auto soundOf = [](const Value& self) { return std::dynamic_pointer_cast<SoundObject>(self.o); };
    def(vm, soundProto, "attachSound", [soundOf](VM& vm, const Value& self, Args& a) {
        auto s = soundOf(self);
        if (!s) return Value();
        const auto it = vm.player.movie.exports.find(vm.toString(arg(a, 0)));
        s->soundId = it != vm.player.movie.exports.end() && vm.player.movie.sounds.count(it->second) ? it->second : 0;
        return Value();
    });
    def(vm, soundProto, "start", [soundOf](VM& vm, const Value& self, Args& a) {
        auto s = soundOf(self);
        if (!s || !s->soundId) return Value();
        const auto it = vm.player.movie.sounds.find(s->soundId);
        if (it == vm.player.movie.sounds.end()) return Value();
        const double offset = a.empty() ? 0.0 : vm.toNumber(a[0]);
        const int loops = a.size() > 1 ? static_cast<int>(vm.toNumber(a[1])) : 1;
        const int h = vm.player.audio.play(it->second, s->soundId, std::isnan(offset) ? 0 : offset * 1000.0, loops,
                                           static_cast<float>(s->volume / 100.0), static_cast<float>(s->pan / 100.0));
        if (h) {
            s->handles.push_back(h);
            vm.soundVoices.push_back({h, s});
        }
        return Value();
    });
    def(vm, soundProto, "stop", [soundOf](VM& vm, const Value& self, Args& a) {
        auto s = soundOf(self);
        if (!s) return Value();
        std::uint32_t only = 0;
        if (!a.empty()) {
            const auto it = vm.player.movie.exports.find(vm.toString(a[0]));
            if (it != vm.player.movie.exports.end()) only = it->second;
        }
        if (s->handles.empty() && !s->soundId && only == 0) { vm.player.audio.stopAll(); return Value(); }
        if (only) vm.player.audio.stopSound(only);
        else for (int h : s->handles) vm.player.audio.stop(h);
        if (!only) s->handles.clear();
        return Value();
    });
    auto applyMix = [soundOf](VM& vm, const Value& self) {
        auto s = soundOf(self);
        if (!s) return;
        for (int h : s->handles) vm.player.audio.setVoice(h, static_cast<float>(s->volume / 100.0), static_cast<float>(s->pan / 100.0));
    };
    def(vm, soundProto, "setVolume", [soundOf, applyMix](VM& vm, const Value& self, Args& a) {
        if (auto s = soundOf(self)) { const double v = vm.toNumber(arg(a, 0)); s->volume = std::isnan(v) ? 0 : std::max(0.0, v); applyMix(vm, self); }
        return Value();
    });
    def(vm, soundProto, "getVolume", [soundOf](VM&, const Value& self, Args&) {
        auto s = soundOf(self);
        return Value(s ? s->volume : 100.0);
    });
    def(vm, soundProto, "setPan", [soundOf, applyMix](VM& vm, const Value& self, Args& a) {
        if (auto s = soundOf(self)) { const double v = vm.toNumber(arg(a, 0)); s->pan = std::isnan(v) ? 0 : std::clamp(v, -100.0, 100.0); applyMix(vm, self); }
        return Value();
    });
    def(vm, soundProto, "getPan", [soundOf](VM&, const Value& self, Args&) {
        auto s = soundOf(self);
        return Value(s ? s->pan : 0.0);
    });
    for (const char* m : {"setTransform", "loadSound"}) {
        def(vm, soundProto, m, [](VM&, const Value&, Args&) { return Value(); });
    }
    def(vm, soundProto, "getTransform", [](VM& vm, const Value&, Args&) {
        auto t = vm.newObject();
        for (const char* k : {"ll", "rr"}) t->props[k] = Value(100);
        for (const char* k : {"lr", "rl"}) t->props[k] = Value(0);
        return Value(t);
    });
    def(vm, soundProto, "getBytesLoaded", [](VM&, const Value&, Args&) { return Value(1000); });
    def(vm, soundProto, "getBytesTotal", [](VM&, const Value&, Args&) { return Value(1000); });
    auto soundCtor = vm.nativeFunction([soundProto](VM& vm, const Value&, Args& a) {
        auto s = std::make_shared<SoundObject>();
        s->proto = soundProto;
        if (!a.empty() && a[0].isObject()) s->target = a[0].o;
        (void)vm;
        return Value(std::static_pointer_cast<Object>(s));
    });
    soundCtor->props["prototype"] = Value(soundProto);
    g->props["Sound"] = Value(soundCtor);
}

} // namespace fp::avm1

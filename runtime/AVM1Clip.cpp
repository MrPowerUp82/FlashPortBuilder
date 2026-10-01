#include "AVM1.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fp {

// ------------------------------------------------------------------ transform cache

void DisplayObject::decompose() {
    if (cachedTransform) return;
    xscale = std::sqrt(double(matrix.a) * matrix.a + double(matrix.b) * matrix.b) * 100.0;
    yscale = std::sqrt(double(matrix.c) * matrix.c + double(matrix.d) * matrix.d) * 100.0;
    if (double(matrix.a) * matrix.d - double(matrix.b) * matrix.c < 0) yscale = -yscale;
    rotation = std::atan2(double(matrix.b), double(matrix.a)) * 180.0 / M_PI;
    cachedTransform = true;
}

DisplayObject::~DisplayObject() {
    if (auto t = std::dynamic_pointer_cast<avm1::TextFieldObject>(textObject)) t->field = nullptr;
}

void DisplayObject::recompose() {
    const double r = rotation * M_PI / 180.0, xs = xscale / 100.0, ys = yscale / 100.0;
    matrix.a = static_cast<float>(xs * std::cos(r));
    matrix.b = static_cast<float>(xs * std::sin(r));
    matrix.c = static_cast<float>(-ys * std::sin(r));
    matrix.d = static_cast<float>(ys * std::cos(r));
}

namespace avm1 {
namespace {

using Args = std::vector<Value>;

Value arg(const Args& a, std::size_t i) { return i < a.size() ? a[i] : Value(); }

Clip* self(VM& vm, const Value& v) { return vm.clipOf(v); }

void def(VM& vm, const ObjectPtr& obj, const std::string& name, NativeFunction::Fn fn) {
    obj->props[name] = Value(vm.nativeFunction(std::move(fn)));
}

Value clipValue(Clip* c) { return c ? Value(std::static_pointer_cast<Object>(c->object())) : Value(); }

// Script depth (0 = first dynamic depth) <-> internal depth (SWF timeline depths start at 1).
constexpr int kDepthOffset = 16384;

bool worldBounds(VM& vm, Clip& clip, float out[4]) {
    return vm.player.geometry.clipBounds(clip, clip.worldMatrix(), out);
}

void copyInit(VM& vm, Clip& clip, const Value& init) {
    if (!init.isObject()) return;
    std::vector<std::string> names;
    init.o->keys(names);
    Value target(std::static_pointer_cast<Object>(clip.object()));
    for (const auto& n : names) vm.setMember(target, n, vm.getMember(init, n));
}

} // namespace

// ------------------------------------------------------------------ ClipObject

bool ClipObject::getOwn(VM& vm, const std::string& name, Value& out) {
    if (!clip) return false;
    if (Object::getOwn(vm, name, out)) return true;
    if (auto* child = clip->childByName(name)) {
        if (child->clip) { out = clipValue(child->clip.get()); return true; }
        if (child->kind == DisplayObject::Kind::Text) {
            if (!child->textObject) {
                auto t = std::make_shared<TextFieldObject>();
                t->field = child;
                t->proto = vm.objectProto;
                child->textObject = t;
            }
            out = Value(child->textObject);
            return true;
        }
    }
    if (name.empty() || name[0] != '_') return false;
    std::string n = name;
    for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    DisplayObject* h = clip->holder();
    if (n == "_x") { out = h ? h->matrix.tx / 20.0 : 0.0; return true; }
    if (n == "_y") { out = h ? h->matrix.ty / 20.0 : 0.0; return true; }
    if (n == "_xscale" || n == "_yscale" || n == "_rotation") {
        if (!h) { out = n == "_rotation" ? 0.0 : 100.0; return true; }
        h->decompose();
        out = n == "_xscale" ? h->xscale : n == "_yscale" ? h->yscale : h->rotation;
        return true;
    }
    if (n == "_alpha") { out = h ? h->cxform.mul[3] * 100.0 : 100.0; return true; }
    if (n == "_visible") { out = h ? h->visible : true; return true; }
    if (n == "_width" || n == "_height") {
        float b[4];
        const Matrix m = h ? h->matrix : Matrix{};
        if (!vm.player.geometry.clipBounds(*clip, m, b)) { out = 0.0; return true; }
        out = (n == "_width" ? b[2] - b[0] : b[3] - b[1]) / 20.0;
        return true;
    }
    if (n == "_currentframe") { out = clip->currentFrame() + 1; return true; }
    if (n == "_totalframes" || n == "_framesloaded") { out = clip->totalFrames(); return true; }
    if (n == "_name") { out = h ? h->name : std::string(); return true; }
    if (n == "_target") { out = vm.targetPath(clip); return true; }
    if (n == "_parent") { out = clipValue(clip->parent()); return true; }
    if (n == "_root" || n == "_level0") { out = clipValue(vm.player.root.get()); return true; }
    if (n == "_global") { out = Value(vm.global); return true; }
    if (n == "_xmouse" || n == "_ymouse") {
        Matrix inv;
        float x = vm.player.mouseX * 20, y = vm.player.mouseY * 20, lx = 0, ly = 0;
        if (clip->worldMatrix().invert(inv)) inv.apply(x, y, lx, ly);
        out = (n == "_xmouse" ? lx : ly) / 20.0;
        return true;
    }
    if (n == "_url") { out = "file:///game.swf"; return true; }
    if (n == "_droptarget") { out = ""; return true; }
    if (n == "_quality") { out = "HIGH"; return true; }
    if (n == "_highquality") { out = 1; return true; }
    if (n == "_focusrect") { out = true; return true; }
    if (n == "_soundbuftime") { out = 5; return true; }
    if (n == "_lockroot") { out = false; return true; }
    return false;
}

void ClipObject::setOwn(VM& vm, const std::string& name, const Value& v) {
    if (!clip) return;
    DisplayObject* h = clip->holder();
    if (!name.empty() && name[0] == '_') {
        std::string n = name;
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const double d = vm.toNumber(v);
        // Debug aid: FP_TRACE_PROP=_x logs every assignment of that property.
        static const char* traceProp = std::getenv("FP_TRACE_PROP");
        if (traceProp && n == traceProp) {
            std::fprintf(stderr, "[prop] %s.%s = %s (holder %s)\n", vm.targetPath(clip).c_str(), n.c_str(),
                         vm.toString(v).c_str(), h ? "yes" : "no");
        }
        auto numeric = [&](auto apply) {
            if (h && !std::isnan(d)) { apply(); h->dynamicTransform = true; }
        };
        if (n == "_x") { numeric([&] { h->matrix.tx = static_cast<float>(d * 20); }); return; }
        if (n == "_y") { numeric([&] { h->matrix.ty = static_cast<float>(d * 20); }); return; }
        if (n == "_xscale" || n == "_yscale" || n == "_rotation") {
            numeric([&] {
                h->decompose();
                if (n == "_xscale") h->xscale = d;
                else if (n == "_yscale") h->yscale = d;
                else h->rotation = std::fmod(d, 360.0);
                h->recompose();
            });
            return;
        }
        if (n == "_alpha") { numeric([&] { h->cxform.mul[3] = static_cast<float>(d / 100.0); }); return; }
        if (n == "_visible") { if (h) h->visible = vm.toBoolean(v); return; }
        if (n == "_width" || n == "_height") {
            numeric([&] {
                float b[4];
                h->decompose();
                const Matrix m = h->matrix;
                if (!vm.player.geometry.clipBounds(*clip, m, b)) return;
                const double cur = (n == "_width" ? b[2] - b[0] : b[3] - b[1]) / 20.0;
                if (cur <= 0) return;
                if (n == "_width") h->xscale *= d / cur; else h->yscale *= d / cur;
                h->recompose();
            });
            return;
        }
        if (n == "_name") { if (h) h->name = vm.toString(v); return; }
        if (n == "_currentframe" || n == "_totalframes" || n == "_framesloaded" || n == "_target" ||
            n == "_parent" || n == "_root" || n == "_xmouse" || n == "_ymouse" || n == "_url") {
            return; // read-only
        }
    }
    Object::setOwn(vm, name, v);
}

bool TextFieldObject::getOwn(VM& vm, const std::string& name, Value& out) {
    if (Object::getOwn(vm, name, out)) return true;
    if (!field) return false;
    if (name == "text" || name == "htmlText") { out = field->text; return true; }
    if (name == "length") { out = static_cast<double>(field->text.size()); return true; }
    if (name == "_x") { out = field->matrix.tx / 20.0; return true; }
    if (name == "_y") { out = field->matrix.ty / 20.0; return true; }
    if (name == "_visible") { out = field->visible; return true; }
    if (name == "_alpha") { out = field->cxform.mul[3] * 100.0; return true; }
    if (name == "_name") { out = field->name; return true; }
    if (name == "_parent") { out = clipValue(field->parent); return true; }
    if (name == "textColor") {
        const auto& def = vm.player.movie.editTexts.at(field->character);
        out = static_cast<double>((def.color.r << 16) | (def.color.g << 8) | def.color.b);
        return true;
    }
    return false;
}

void TextFieldObject::setOwn(VM& vm, const std::string& name, const Value& v) {
    if (field) {
        if (name == "text") { field->text = vm.toString(v); return; }
        if (name == "htmlText") { field->text = stripHtml(vm.toString(v)); return; }
        const double d = vm.toNumber(v);
        if (name == "_x" && !std::isnan(d)) { field->matrix.tx = static_cast<float>(d * 20); return; }
        if (name == "_y" && !std::isnan(d)) { field->matrix.ty = static_cast<float>(d * 20); return; }
        if (name == "_visible") { field->visible = vm.toBoolean(v); return; }
        if (name == "_alpha" && !std::isnan(d)) { field->cxform.mul[3] = static_cast<float>(d / 100.0); return; }
    }
    Object::setOwn(vm, name, v);
}

void ClipObject::keys(std::vector<std::string>& out) const {
    Object::keys(out);
    if (!clip) return;
    for (const auto& [depth, child] : clip->children()) {
        if (child->clip && !child->name.empty()) out.push_back(child->name);
    }
}

// ------------------------------------------------------------------ MovieClip methods

Value clipGotoFrame(VM& vm, Clip& clip, const Value& frame, bool play) {
    int f = -1;
    if (frame.isString()) {
        f = clip.frameForLabel(frame.s);
        if (f < 0) {
            const double n = vm.toNumber(frame);
            if (!std::isnan(n)) f = static_cast<int>(n) - 1;
        }
    } else {
        const double n = vm.toNumber(frame);
        if (!std::isnan(n)) f = static_cast<int>(n) - 1;
    }
    if (f >= 0) clip.gotoFrame(f, play || vm.player.ignoreStops);
    return {};
}

void installMovieClip(VM& vm) {
    auto& p = vm.clipProto;
    def(vm, p, "play", [](VM& vm, const Value& t, Args&) { if (auto* c = self(vm, t)) c->play(); return Value(); });
    def(vm, p, "stop", [](VM& vm, const Value& t, Args&) { if (auto* c = self(vm, t)) if (!vm.player.ignoreStops) c->stop(); return Value(); });
    def(vm, p, "gotoAndPlay", [](VM& vm, const Value& t, Args& a) { if (auto* c = self(vm, t)) clipGotoFrame(vm, *c, arg(a, a.size() > 1 ? 1 : 0), true); return Value(); });
    def(vm, p, "gotoAndStop", [](VM& vm, const Value& t, Args& a) { if (auto* c = self(vm, t)) clipGotoFrame(vm, *c, arg(a, a.size() > 1 ? 1 : 0), false); return Value(); });
    def(vm, p, "nextFrame", [](VM& vm, const Value& t, Args&) { if (auto* c = self(vm, t)) c->gotoFrame(c->currentFrame() + 1, false); return Value(); });
    def(vm, p, "prevFrame", [](VM& vm, const Value& t, Args&) { if (auto* c = self(vm, t)) c->gotoFrame(c->currentFrame() - 1, false); return Value(); });

    def(vm, p, "attachMovie", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        if (!c) return Value();
        const auto it = vm.player.movie.exports.find(vm.toString(arg(a, 0)));
        if (it == vm.player.movie.exports.end()) return Value();
        auto* obj = c->attach(it->second, vm.toString(arg(a, 1)), vm.toInt32(arg(a, 2)) + kDepthOffset, true);
        if (!obj || !obj->clip) return Value();
        copyInit(vm, *obj->clip, arg(a, 3));
        return clipValue(obj->clip.get());
    });
    def(vm, p, "createEmptyMovieClip", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        if (!c) return Value();
        auto* obj = c->attach(0xffff, vm.toString(arg(a, 0)), vm.toInt32(arg(a, 1)) + kDepthOffset, true);
        return obj && obj->clip ? clipValue(obj->clip.get()) : Value();
    });
    def(vm, p, "duplicateMovieClip", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        if (!c || !c->parent() || !c->holder()) return Value();
        DisplayObject* src = c->holder();
        auto* obj = c->parent()->attach(src->character, vm.toString(arg(a, 0)), vm.toInt32(arg(a, 1)) + kDepthOffset, true);
        if (!obj) return Value();
        obj->matrix = src->matrix;
        obj->cxform = src->cxform;
        obj->visible = src->visible;
        obj->clipActions = src->clipActions;
        if (obj->clip) {
            vm.queueClipEvent(*obj->clip, ClipEvent::Load);
            copyInit(vm, *obj->clip, arg(a, 2));
        }
        return obj->clip ? clipValue(obj->clip.get()) : Value();
    });
    def(vm, p, "removeMovieClip", [](VM& vm, const Value& t, Args&) {
        Clip* c = self(vm, t);
        if (c && c->parent() && c->holder() && c->holder()->depth >= kDepthOffset) c->parent()->removeChild(c->holder()->depth);
        return Value();
    });
    def(vm, p, "unloadMovie", [](VM& vm, const Value& t, Args&) {
        Clip* c = self(vm, t);
        if (c && c->parent() && c->holder()) c->parent()->removeChild(c->holder()->depth);
        return Value();
    });
    def(vm, p, "getDepth", [](VM& vm, const Value& t, Args&) {
        Clip* c = self(vm, t);
        return c && c->holder() ? Value(c->holder()->depth - kDepthOffset) : Value();
    });
    def(vm, p, "getNextHighestDepth", [](VM& vm, const Value& t, Args&) {
        Clip* c = self(vm, t);
        if (!c) return Value();
        int d = kDepthOffset;
        for (const auto& [depth, child] : c->children()) d = std::max(d, depth + 1);
        return Value(d - kDepthOffset);
    });
    def(vm, p, "getInstanceAtDepth", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        auto* obj = c ? c->childAt(vm.toInt32(arg(a, 0)) + kDepthOffset) : nullptr;
        return obj && obj->clip ? clipValue(obj->clip.get()) : Value();
    });
    def(vm, p, "swapDepths", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        if (!c || !c->parent() || !c->holder()) return Value();
        int target;
        if (Clip* other = arg(a, 0).isObject() ? vm.clipOf(a[0]) : nullptr) {
            if (!other->holder() || other->parent() != c->parent()) return Value();
            target = other->holder()->depth;
        } else {
            target = vm.toInt32(arg(a, 0)) + kDepthOffset;
        }
        c->parent()->swapDepths(c->holder()->depth, target);
        return Value();
    });
    def(vm, p, "hitTest", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        if (!c) return Value(false);
        if (a.size() >= 2 && !a[0].isObject() && !(a[0].isString() && vm.toNumber(a[0]) != vm.toNumber(a[0]))) {
            const float x = static_cast<float>(vm.toNumber(a[0]) * 20), y = static_cast<float>(vm.toNumber(a[1]) * 20);
            return Value(vm.player.geometry.clipHit(*c, c->worldMatrix(), x, y, vm.toBoolean(arg(a, 2))));
        }
        Clip* other = vm.clipOf(arg(a, 0));
        if (!other) return Value(false);
        float b1[4], b2[4];
        if (!worldBounds(vm, *c, b1) || !worldBounds(vm, *other, b2)) return Value(false);
        return Value(b1[0] <= b2[2] && b2[0] <= b1[2] && b1[1] <= b2[3] && b2[1] <= b1[3]);
    });
    def(vm, p, "getBounds", [](VM& vm, const Value& t, Args& a) {
        Clip* c = self(vm, t);
        auto r = vm.newObject();
        if (!c) return Value(r);
        Matrix m = c->worldMatrix();
        if (Clip* space = vm.clipOf(arg(a, 0))) {
            Matrix inv;
            if (space->worldMatrix().invert(inv)) m = inv * m;
        } else if (arg(a, 0).isUndefined()) {
            m = Matrix{};
        }
        float b[4] = {0, 0, 0, 0};
        vm.player.geometry.clipBounds(*c, m, b);
        r->props["xMin"] = Value(b[0] / 20.0);
        r->props["yMin"] = Value(b[1] / 20.0);
        r->props["xMax"] = Value(b[2] / 20.0);
        r->props["yMax"] = Value(b[3] / 20.0);
        return Value(r);
    });
    auto convert = [](bool toGlobal) {
        return [toGlobal](VM& vm, const Value& t, Args& a) {
            Clip* c = self(vm, t);
            const Value pt = arg(a, 0);
            if (!c || !pt.isObject()) return Value();
            Matrix m = c->worldMatrix();
            if (!toGlobal && !m.invert(m)) return Value();
            float x = static_cast<float>(vm.toNumber(vm.getMember(pt, "x")) * 20), y = static_cast<float>(vm.toNumber(vm.getMember(pt, "y")) * 20);
            float ox, oy;
            m.apply(x, y, ox, oy);
            vm.setMember(pt, "x", Value(ox / 20.0));
            vm.setMember(pt, "y", Value(oy / 20.0));
            return Value();
        };
    };
    def(vm, p, "localToGlobal", convert(true));
    def(vm, p, "globalToLocal", convert(false));
    def(vm, p, "getBytesLoaded", [](VM&, const Value&, Args&) { return Value(1000); });
    def(vm, p, "getBytesTotal", [](VM&, const Value&, Args&) { return Value(1000); });
    for (const char* noop : {"startDrag", "stopDrag", "setMask", "attachAudio", "loadMovie", "getURL", "lineStyle",
                             "beginFill", "beginGradientFill", "moveTo", "lineTo", "curveTo", "endFill", "clear",
                             "loadVariables", "createTextField"}) {
        def(vm, p, noop, [](VM&, const Value&, Args&) { return Value(); });
    }
    auto ctor = vm.nativeFunction([](VM&, const Value& t, Args&) { return t; });
    ctor->props["prototype"] = Value(vm.clipProto);
    vm.global->props["MovieClip"] = Value(ctor);
}

} // namespace avm1
} // namespace fp
